#include "game/vault_fixes.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unordered_map>

#include "log.h"

namespace VaultFixes
{
    // nfs.exe v1.1, as offsets from the image base. 0x6DA5F0 reads GLOBAL\ATTRIBUTES.BIN into a
    // buffer, 0x6DA603 hands it to the vault loader, and the buffer is freed straight after.
    static const uintptr_t CALL_LOAD_ATTRIBUTES = 0x2DA603;
    static const uintptr_t VAULT_LOADER = 0x2D7100;

    // Keys are the Attrib hash (Jenkins lookup2, init 0xABCDEF00) of these names.
    static const uint32_t CLASS_VEHICLE = 0xCA832D18;            // "vehicle"
    static const uint32_t FIELD_TANK_SLAPPER_TIMER = 0x08D47A03; // in each car's fixed layout
    static const uint32_t FIELD_SPEED_LIMITER = 0xA2D9ECB4;      // an array of 2 floats
    static const uint32_t CLASS_CAMERA_VIBRATION = 0xE3C144F9;   // "cameravibration"
    static const uint32_t FIELD_ENABLE = 0x3D460262;             // "Enable", a bool in each preset's layout
    static const uint32_t TYPE_CLASS =0x5E970CBC;               // "Attrib::ClassLoadData"
    static const uint32_t TYPE_COLLECTION = 0x8E112EB7;          // "Attrib::CollectionLoadData"
    static const float NO_SPEED_LIMIT = 900.0f;                  // what the "Speed Limiter Removed" script sets

    typedef void* (__cdecl *VaultLoaderFn)(void*, const char*, int, int, int);
    static VaultLoaderFn g_loader = nullptr;
    static bool g_tankSlapper = true;
    static bool g_speedLimiter = true;
    static bool g_cameraVibration = true;

    // A view of one vault's two blobs with bounds checks, since a modded file could be anything.
    struct Blob {
        uint8_t* data;
        uint32_t size;
        bool Has(uint32_t offset, uint32_t length) const { return offset <= size && length <= size - offset; }
        uint32_t U32(uint32_t offset) const { uint32_t v; memcpy(&v, data + offset, 4); return v; }
        uint16_t U16(uint32_t offset) const { uint16_t v; memcpy(&v, data + offset, 2); return v; }
    };

    struct Counts { int tank = 0, speed = 0, cameras = 0, alreadyDone = 0; };

    static void SetFloat(Blob& bin, uint32_t offset, float value, Counts& counts, int& changed) {
        float current;
        memcpy(&current, bin.data + offset, 4);
        if (current == value) { ++counts.alreadyDone; return; }
        memcpy(bin.data + offset, &value, 4);
        ++changed;
    }

    // One vault: a VLT index (chunks ExpN, PtrN, ...) and a BIN data blob. Exports point at class and
    // collection records in the VLT; PtrN fixups turn their pointer fields into BIN offsets.
    static void PatchVault(Blob vlt, Blob bin, Counts& counts) {
        uint32_t expAt = 0, expSize = 0, ptrAt = 0, ptrSize = 0;
        for (uint32_t o = 0; vlt.Has(o, 8);) {
            const uint32_t id = vlt.U32(o), size = vlt.U32(o + 4);
            if (size < 8 || !vlt.Has(o, size)) break;
            if (id == 0x4578704E) { expAt = o + 8; expSize = size - 8; }       // ExpN
            else if (id == 0x5074724E) { ptrAt = o + 8; ptrSize = size - 8; }  // PtrN
            o += size;
        }
        if (!expSize || !ptrSize) return;

        // PtrN records: {offset in VLT, type, index, destination}. Type 3 is a pointer; the ones read
        // here (class definitions, layouts, array data) all point into the BIN, and every use is
        // bounds-checked against it.
        std::unordered_map<uint32_t, uint32_t> toBin;
        for (uint32_t p = ptrAt; p + 12 <= ptrAt + ptrSize; p += 12) {
            if (vlt.U16(p + 4) == 3) toBin[vlt.U32(p)] = vlt.U32(p + 8);
        }
        auto binPointer = [&](uint32_t at, uint32_t& out) {
            const auto it = toBin.find(at);
            if (it == toBin.end()) return false;
            out = it->second;
            return true;
        };

        const uint32_t count = vlt.U32(expAt);
        if (expSize < 4 || count > (expSize - 4) / 16) return;

        // Where a field sits in its class's fixed layout, or UINT32_MAX if it isn't there with that size.
        auto layoutOffset = [&](uint32_t classKey, uint32_t fieldKey, uint16_t size) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t e = expAt + 4 + i * 16, type = vlt.U32(e + 4), at = vlt.U32(e + 12);
                if (type != TYPE_CLASS || !vlt.Has(at, 28) || vlt.U32(at) != classKey) continue;
                const uint32_t numDefs = vlt.U32(at + 8);
                uint32_t defs;
                if (!binPointer(at + 12, defs) || numDefs > 4096 || !bin.Has(defs, numDefs * 16)) break;
                for (uint32_t d = 0; d < numDefs; ++d) {
                    const uint32_t def = defs + d * 16;
                    // {key, type, offset u16, size u16, max count u16, flags u8, alignment u8}; flag 2 = in the layout
                    if (bin.U32(def) == fieldKey && bin.U16(def + 10) == size && (bin.data[def + 14] & 2))
                        return static_cast<uint32_t>(bin.U16(def + 8));
                }
                break;
            }
            return UINT32_MAX;
        };
        const uint32_t tankOffset = layoutOffset(CLASS_VEHICLE, FIELD_TANK_SLAPPER_TIMER, 4);
        const uint32_t vibrationOffset = layoutOffset(CLASS_CAMERA_VIBRATION, FIELD_ENABLE, 1);

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t e = expAt + 4 + i * 16, type = vlt.U32(e + 4), at = vlt.U32(e + 12);
            if (type != TYPE_COLLECTION || !vlt.Has(at, 32)) continue;

            // Every camera's vibration preset (default, bumperhood, close, far, staging): Enable = false.
            uint32_t layout;
            if (vlt.U32(at + 4) == CLASS_CAMERA_VIBRATION) {
                if (g_cameraVibration && vibrationOffset != UINT32_MAX && binPointer(at + 28, layout) && bin.Has(layout + vibrationOffset, 1)) {
                    uint8_t& enable = bin.data[layout + vibrationOffset];
                    if (enable == 0) ++counts.alreadyDone;
                    else { enable = 0; ++counts.cameras; }
                }
                continue;
            }
            // {key, class, parent, reserve, unused, entries, types u16, types length u16, layout pointer, types..., entries...}
            if (vlt.U32(at + 4) != CLASS_VEHICLE) continue;
            const uint32_t numEntries = vlt.U32(at + 20), typesLen = vlt.U16(at + 26);
            const uint32_t entriesAt = at + 32 + typesLen * 4;
            if (numEntries > 4096 || !vlt.Has(entriesAt, numEntries * 12)) continue;

            if (g_tankSlapper && tankOffset != UINT32_MAX && binPointer(at + 28, layout) && bin.Has(layout + tankOffset, 4))
                SetFloat(bin, layout + tankOffset, 0.0f, counts, counts.tank);

            if (!g_speedLimiter) continue;
            for (uint32_t n = 0; n < numEntries; ++n) {
                // {key, data or pointer, type index u16, node flags u8, entry flags u8}
                const uint32_t entry = entriesAt + n * 12;
                uint32_t array;
                if (vlt.U32(entry) != FIELD_SPEED_LIMITER || !binPointer(entry + 4, array) || !bin.Has(array, 8)) continue;
                // An array starts with {allocated u16, count u16, element size u16, padding u16}.
                const uint16_t used = bin.U16(array + 2), elementSize = bin.U16(array + 4);
                if (elementSize != 4 || used > 8 || !bin.Has(array + 8, used * 4u)) continue;
                for (uint16_t k = 0; k < used; ++k) SetFloat(bin, array + 8 + k * 4, NO_SPEED_LIMIT, counts, counts.speed);
            }
        }
    }

    // GLOBAL\ATTRIBUTES.BIN is a VPAK: {"VPAK", vault count, name table offset, size}, then per vault
    // {name offset, BIN size, VLT size, BIN offset, VLT offset}, offsets from the start of the file.
    static void PatchPack(uint8_t* pack) {
        if (!pack || IsBadReadPtr(pack, 16) || memcmp(pack, "VPAK", 4) != 0) {
            Log::Warn("The game's attribute file isn't in the expected format. Car fixes are off.\n");
            return;
        }
        uint32_t vaults;
        memcpy(&vaults, pack + 4, 4);
        Counts counts;
        for (uint32_t v = 0; v < vaults && v < 16; ++v) {
            uint32_t entry[5];
            memcpy(entry, pack + 16 + v * 20, sizeof(entry));
            Blob bin{pack + entry[3], entry[1]}, vlt{pack + entry[4], entry[2]};
            if (IsBadWritePtr(bin.data, bin.size) || IsBadReadPtr(vlt.data, vlt.size)) continue;
            PatchVault(vlt, bin, counts);
        }
        Log::Event(Log::Tag::Client, "Car fixes: tank slapper off on %d cars, speed limiter off on %d values, vibration off on %d cameras (%d already set)\n",
                   counts.tank, counts.speed, counts.cameras, counts.alreadyDone);
    }

    static void* __cdecl LoadAttributesHook(void* pack, const char* name, int a, int b, int c) {
        PatchPack(static_cast<uint8_t*>(pack)); // every read and write is bounds-checked
        return g_loader(pack, name, a, b, c);
    }

    void Init(uintptr_t baseModule) {
        const std::string& settings = Log::SettingsPath();
        if (!settings.empty()) {
            g_tankSlapper = GetPrivateProfileIntA("Physics", "NoTankSlapper", 1, settings.c_str()) != 0;
            g_speedLimiter = GetPrivateProfileIntA("Physics", "NoSpeedLimiter", 1, settings.c_str()) != 0;
            g_cameraVibration = GetPrivateProfileIntA("Display", "NoCameraVibration", 1, settings.c_str()) != 0;
        }
        if (!g_tankSlapper && !g_speedLimiter && !g_cameraVibration) return;

        // Only patch a call that still goes where v1.1's does.
        uint8_t* call = reinterpret_cast<uint8_t*>(baseModule + CALL_LOAD_ATTRIBUTES);
        const uintptr_t loader = baseModule + VAULT_LOADER;
        int32_t rel;
        memcpy(&rel, call + 1, 4);
        if (call[0] != 0xE8 || reinterpret_cast<uintptr_t>(call) + 5 + rel != loader) {
            Log::Warn("Car fixes are off: the game's attribute loading isn't where it's expected.\n");
            return;
        }
        g_loader = reinterpret_cast<VaultLoaderFn>(loader);
        DWORD oldProtect;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) return;
        const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LoadAttributesHook) - (reinterpret_cast<uintptr_t>(call) + 5));
        memcpy(call + 1, &hookRel, 4);
        VirtualProtect(call + 1, 4, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        Log::Debug("[OpenProStreet] Car fix hook installed.\n");
    }
}
