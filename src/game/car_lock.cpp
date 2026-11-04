#include "game/car_lock.h"
#include "game/car.h"
#include "log.h"
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>

namespace CarLock
{
    // FELanCarSelectStateManager (this) keeps one slot per car to choose, 0x3C bytes each, from +0xFC
    // to +0x100. +0xCC is the slot being chosen and +0xCD how many there are; past the last one, the
    // screen picks the backup car. A slot lists cars of 0x20 bytes from +0x08 to +0x0C, each with its
    // handle at +0x14, and their tab flags, one byte per car, at +0x18: 1 is career cars, 2 bonus cars.
    // +0x38 is the car the slot already holds.
    static const uintptr_t SLOTS = 0xFC;
    static const uintptr_t SLOTS_END = 0x100;
    static const uintptr_t SLOT_SIZE = 0x3C;
    static const uintptr_t EXCLUDED_CAR = 0x10C;

    // 0x7E1C20, which the list builder (0x7F59D0) calls to learn which tabs have cars.
    static const uintptr_t AVAILABLE_TABS = 0x3E1C20;
    static const uint8_t AVAILABLE_TABS_START[] = {0x83, 0xEC, 0x2C, 0x53, 0x8B, 0xC1};

    // With both tabs available, 0x7F5A4C picks career cars only if the current car is one.
    // 0x7F5AB6 is where it picks them.
    static const uintptr_t BOTH_TABS = 0x3F5A4C;
    static const uintptr_t PICK_CAREER = 0x3F5AB6;
    static const uint8_t BOTH_TABS_START[] = {0x8A, 0x85, 0xCC, 0x00, 0x00, 0x00};

    // The builder's two loops over the cars: one for a slot (0x7F5BC0), one for the backup car
    // (0x7F5D82). Each patch point sits after the check that skips a slot's own car, runs the two
    // instructions it replaces, and goes on, or skips to the next car.
    struct ListPatch {
        uintptr_t site;
        uint8_t original[10];
        uintptr_t next;
        uintptr_t after;
    };
    static const ListPatch LIST_PATCHES[] = {
        // mov cl,[ebp+0x118] / mov esi,[esp+0x18]
        {0x3F5BDD, {0x8A, 0x8D, 0x18, 0x01, 0x00, 0x00, 0x8B, 0x74, 0x24, 0x18}, 0x3F5CF8, 0x3F5BE7},
        // mov dl,[ebp+0x118] / mov ecx,[esp+0x1C]
        {0x3F5D9C, {0x8A, 0x95, 0x18, 0x01, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x1C}, 0x3F5E3E, 0x3F5DA6},
    };

    // In the backup loop, 0x7F5D8F loads the car's handle, and 0x7F5D93 skips the car its slot
    // already holds. Patched so that skip can be turned off.
    static const uintptr_t BACKUP_OWN_CAR = 0x3F5D8F;
    static const uint8_t BACKUP_OWN_CAR_START[] = {0x8B, 0x7C, 0x24, 0x50, 0x3B, 0x78, 0x38, 0x0F, 0x84, 0xA2, 0x00, 0x00, 0x00};
    static const uintptr_t BACKUP_NEXT_CAR = 0x3F5E3E;
    static const uintptr_t BACKUP_CHECK_TABS = 0x3F5D9C;

    static uintptr_t g_baseModule = 0;
    static std::mutex g_lock;
    static std::set<std::string> g_allowed;
    static bool g_showAll = false; // set when none of the player's cars is allowed
    // Set when the player has only one allowed car: it's then offered as the backup car too, instead
    // of every car. The patched loop reads it, so it's a plain byte.
    static volatile uint8_t g_backupSameCar = 0;

    void SetAllowedCars(const std::string& models) {
        std::set<std::string> allowed;
        std::stringstream list(models);
        std::string model;
        while (std::getline(list, model, ',')) {
            if (!model.empty()) allowed.insert(model);
        }
        std::lock_guard<std::mutex> lock(g_lock);
        if (allowed != g_allowed) {
            if (allowed.empty()) Log::Event(Log::Tag::Client, "every car is allowed here\n");
            else Log::Event(Log::Tag::Client, "this race day allows %u car model(s), so the others are hidden\n", static_cast<unsigned>(allowed.size()));
        }
        g_allowed.swap(allowed);
        g_showAll = false;
        g_backupSameCar = 0;
    }

    // A car whose model isn't known stays, so a gap in the car table never hides a car the server
    // would accept.
    static bool IsAllowedLocked(uint32_t carHandle) {
        if (g_allowed.empty() || g_showAll) return true;
        const std::string model = CarModelForHandle(g_baseModule, carHandle);
        return model.empty() || g_allowed.count(model) != 0;
    }

    static bool __cdecl IsCarShown(uint32_t carHandle) {
        std::lock_guard<std::mutex> lock(g_lock);
        return IsAllowedLocked(carHandle);
    }

    // The tab flags of a slot's shown cars, leaving out the car to skip.
    static uint8_t SlotTabs(uintptr_t slot, uint32_t skippedCar) {
        const uintptr_t begin = *reinterpret_cast<uintptr_t*>(slot + 0x08);
        const uintptr_t end = *reinterpret_cast<uintptr_t*>(slot + 0x0C);
        const uint8_t* flags = *reinterpret_cast<uint8_t**>(slot + 0x18);
        uint8_t tabs = 0;
        for (uintptr_t car = begin; car != end; car += 0x20, ++flags) {
            const uint32_t handle = *reinterpret_cast<uint32_t*>(car + 0x14);
            if (handle != skippedCar && IsAllowedLocked(handle)) tabs |= *flags;
        }
        return tabs;
    }

    static bool ChoosingBackup(uintptr_t self) {
        return *reinterpret_cast<uint8_t*>(self + 0xCC) >= *reinterpret_cast<uint8_t*>(self + 0xCD);
    }

    static uint8_t AvailableTabs(uintptr_t self) {
        const uint8_t current = *reinterpret_cast<uint8_t*>(self + 0xCC);
        const uintptr_t slots = *reinterpret_cast<uintptr_t*>(self + SLOTS);
        if (!ChoosingBackup(self)) {
            return SlotTabs(slots + current * SLOT_SIZE, *reinterpret_cast<uint32_t*>(self + EXCLUDED_CAR));
        }
        // The backup car comes from any slot with a car to spare, other than the one it holds, or
        // that one too when it's the player's only allowed car.
        uint8_t tabs = 0;
        const uintptr_t end = *reinterpret_cast<uintptr_t*>(self + SLOTS_END);
        for (uintptr_t slot = slots; slot != end; slot += SLOT_SIZE) {
            const uintptr_t cars = (*reinterpret_cast<uintptr_t*>(slot + 0x0C) - *reinterpret_cast<uintptr_t*>(slot + 0x08)) / 0x20;
            if (cars >= 2) tabs |= SlotTabs(slot, g_backupSameCar ? 0 : *reinterpret_cast<uint32_t*>(slot + 0x38));
        }
        return tabs;
    }

    // Replaces 0x7E1C20, so a tab with no allowed car isn't offered: picking from an empty tab hangs
    // the game. A player with one allowed car gets it as their backup car too. If no car at all is
    // allowed, the screen shows every car, and the server's chat message says which ones can race.
    static uint8_t __attribute__((thiscall)) AvailableTabsHook(void* self) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uintptr_t screen = reinterpret_cast<uintptr_t>(self);
        g_showAll = false;
        g_backupSameCar = 0;
        uint8_t tabs = AvailableTabs(screen);
        if (tabs != 0 || g_allowed.empty()) return tabs;
        if (ChoosingBackup(screen)) {
            g_backupSameCar = 1;
            tabs = AvailableTabs(screen);
            if (tabs != 0) {
                Log::Event(Log::Tag::Client, "you have one car this race day allows, so it's your backup car too\n");
                return tabs;
            }
            g_backupSameCar = 0;
        }
        Log::Event(Log::Tag::Client, "none of your cars is allowed in this race day, so all of them are shown\n");
        g_showAll = true;
        return AvailableTabs(reinterpret_cast<uintptr_t>(self));
    }

    static uint8_t* WriteJump(uint8_t* at, uint8_t opcode, uintptr_t target) {
        *at = opcode;
        *reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(target - (reinterpret_cast<uintptr_t>(at) + 5));
        return at + 5;
    }

    static bool Matches(uintptr_t at, const uint8_t* bytes, size_t size) {
        return !IsBadReadPtr(reinterpret_cast<void*>(at), size) && memcmp(reinterpret_cast<void*>(at), bytes, size) == 0;
    }

    static void PatchJump(uintptr_t at, uintptr_t target, size_t size) {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &oldProtect);
        WriteJump(reinterpret_cast<uint8_t*>(at), 0xE9, target);
        memset(reinterpret_cast<uint8_t*>(at) + 5, 0x90, size - 5);
        VirtualProtect(reinterpret_cast<void*>(at), size, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    }

    static void PatchList(uint8_t*& stubs, const ListPatch& patch) {
        const uintptr_t site = g_baseModule + patch.site;
        uint8_t* stub = stubs;
        uint8_t* p = stub;
        *p++ = 0x60;                                             // pushad
        *p++ = 0x57;                                             // push edi: the car's handle
        p = WriteJump(p, 0xE8, reinterpret_cast<uintptr_t>(&IsCarShown));
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                   // add esp,4
        *p++ = 0x84; *p++ = 0xC0;                                // test al,al (popad keeps the flags)
        *p++ = 0x61;                                             // popad
        *p++ = 0x0F; *p++ = 0x84;                                // jz next car
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>((g_baseModule + patch.next) - (reinterpret_cast<uintptr_t>(p) + 4));
        p += 4;
        memcpy(p, patch.original, sizeof(patch.original));
        p += sizeof(patch.original);
        p = WriteJump(p, 0xE9, g_baseModule + patch.after);
        stubs = p;
        PatchJump(site, reinterpret_cast<uintptr_t>(stub), sizeof(patch.original));
    }

    // Replaces the backup loop's own car check: with g_backupSameCar set, the slot's own car is
    // offered too, and goes on to the tab check like any other.
    static void PatchBackupOwnCar(uint8_t*& stubs) {
        const uintptr_t site = g_baseModule + BACKUP_OWN_CAR;
        uint8_t* p = stubs;
        uint8_t* stub = p;
        *p++ = 0x8B; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x50;     // mov edi,[esp+0x50]: the car's handle
        *p++ = 0x80; *p++ = 0x3D;                                // cmp byte [g_backupSameCar],0
        *reinterpret_cast<uint32_t*>(p) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_backupSameCar));
        p += 4;
        *p++ = 0x00;
        *p++ = 0x0F; *p++ = 0x85;                                // jne: offer the slot's own car too
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>((g_baseModule + BACKUP_CHECK_TABS) - (reinterpret_cast<uintptr_t>(p) + 4));
        p += 4;
        *p++ = 0x3B; *p++ = 0x78; *p++ = 0x38;                   // cmp edi,[eax+0x38]: the slot's own car
        *p++ = 0x0F; *p++ = 0x84;                                // je next car
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>((g_baseModule + BACKUP_NEXT_CAR) - (reinterpret_cast<uintptr_t>(p) + 4));
        p += 4;
        p = WriteJump(p, 0xE9, g_baseModule + BACKUP_CHECK_TABS);
        stubs = p;
        PatchJump(site, reinterpret_cast<uintptr_t>(stub), sizeof(BACKUP_OWN_CAR_START));
    }

    void Init(uintptr_t baseModule) {
        g_baseModule = baseModule;
        bool expected = Matches(baseModule + AVAILABLE_TABS, AVAILABLE_TABS_START, sizeof(AVAILABLE_TABS_START)) &&
                        Matches(baseModule + BOTH_TABS, BOTH_TABS_START, sizeof(BOTH_TABS_START)) &&
                        Matches(baseModule + BACKUP_OWN_CAR, BACKUP_OWN_CAR_START, sizeof(BACKUP_OWN_CAR_START));
        for (const ListPatch& patch : LIST_PATCHES) {
            expected = expected && Matches(baseModule + patch.site, patch.original, sizeof(patch.original));
        }
        if (!expected) {
            Log::Warn("The car selection code isn't as expected, so disallowed cars won't be hidden.\n");
            return;
        }

        uint8_t* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stubs) return;
        for (const ListPatch& patch : LIST_PATCHES) PatchList(stubs, patch);
        PatchBackupOwnCar(stubs);
        PatchJump(baseModule + AVAILABLE_TABS, reinterpret_cast<uintptr_t>(&AvailableTabsHook), sizeof(AVAILABLE_TABS_START));
        // Career cars first whenever both tabs have some: bonus cars only matter in challenge race days.
        PatchJump(baseModule + BOTH_TABS, baseModule + PICK_CAREER, sizeof(BOTH_TABS_START));
        Log::Debug("[OpenProStreet] Car lock and career-first car selection hooked\n");
    }
}
