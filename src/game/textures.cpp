#include "game/textures.h"

#include "game/d3d_hooks.h"
#include "log.h"
#include "minhook/MinHook.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Textures
{
    // Vtable indices, the same in every Direct3D 9.
    static const size_t DEVICE_UPDATE_TEXTURE = 31;
    static const size_t DEVICE_SET_TEXTURE = 65;
    static const size_t TEXTURE_RELEASE = 2;
    static const size_t TEXTURE_UNLOCK_RECT = 20;

    // D3DX's "take it from the file" values.
    static const UINT FROM_FILE_SIZE = 0xFFFFFFFF;     // D3DX_DEFAULT
    static const UINT FROM_FILE_MIPS = 0xFFFFFFFD;     // D3DX_FROM_FILE
    static const DWORD DEFAULT_FILTER = 0xFFFFFFFF;    // D3DX_DEFAULT

    // A replacement larger than this isn't loaded: the game has 2 GB of address space for everything.
    static const DWORD MAX_FILE_BYTES = 96u * 1024u * 1024u;

    typedef HRESULT (WINAPI *UpdateTextureFn)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
    typedef HRESULT (WINAPI *SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    typedef ULONG (WINAPI *ReleaseFn)(IDirect3DTexture9*);
    typedef HRESULT (WINAPI *UnlockRectFn)(IDirect3DTexture9*, UINT);
    typedef HRESULT (WINAPI *CreateFromMemoryFn)(IDirect3DDevice9*, const void*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                 DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);

    static UpdateTextureFn g_realUpdateTexture = nullptr;
    static SetTextureFn g_realSetTexture = nullptr;
    static ReleaseFn g_realRelease = nullptr;
    static UnlockRectFn g_realUnlockRect = nullptr;
    static CreateFromMemoryFn g_createFromMemory = nullptr;

    struct Entry
    {
        uint32_t id = 0;
        IDirect3DTexture9* replacement = nullptr;
        bool failed = false;    // the file couldn't be loaded: don't try again each frame
    };

    static bool g_log = false;
    static bool g_hooked = false;
    static IDirect3DDevice9* g_device = nullptr;
    // Replacement files by texture id.
    static std::unordered_map<uint32_t, std::string> g_files;
    // The game's textures that have a replacement file, by address. Guarded by g_lock.
    static std::unordered_map<IDirect3DBaseTexture9*, Entry> g_entries;
    static std::set<uint32_t> g_seen;
    static std::mutex g_lock;
    // Set while this code uses Direct3D itself, so its own textures aren't taken for the game's.
    static thread_local bool g_inside = false;

    static uint32_t g_crcTable[256];

    static void BuildCrcTable()
    {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            g_crcTable[n] = c;
        }
    }

    // TexMod's checksum: a CRC-32 that starts at all ones and isn't inverted at the end.
    static uint32_t TexModCrc(const unsigned char* data, size_t bytes)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < bytes; ++i) crc = g_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    // Bits per pixel as TexMod counts them. 0 for a format it doesn't know.
    static unsigned BitsOf(D3DFORMAT format)
    {
        switch (static_cast<DWORD>(format)) {
        case D3DFMT_DXT1: return 4;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 8;
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: return 8;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_A8P8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_L16:
        case D3DFMT_D16: case D3DFMT_R16F:
            return 16;
        case D3DFMT_R8G8B8: return 24;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10: case D3DFMT_G16R16: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16:
        case D3DFMT_A2W10V10U10: case D3DFMT_G16R16F: case D3DFMT_R32F:
            return 32;
        case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_Q16W16V16U16: case D3DFMT_G32R32F: return 64;
        case D3DFMT_A32B32G32R32F: return 128;
        default: return 0;
        }
    }

    // Loads the replacement for an entry. Called with g_lock held and g_inside set.
    static void LoadReplacement(Entry& entry)
    {
        const std::unordered_map<uint32_t, std::string>::const_iterator file = g_files.find(entry.id);
        if (file == g_files.end() || !g_device || !g_createFromMemory) { entry.failed = true; return; }

        const HANDLE handle = CreateFileA(file->second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { entry.failed = true; return; }
        const DWORD bytes = GetFileSize(handle, nullptr);
        std::vector<unsigned char> data;
        DWORD read = 0;
        if (bytes != INVALID_FILE_SIZE && bytes > 0 && bytes <= MAX_FILE_BYTES) {
            data.resize(bytes);
            if (!ReadFile(handle, data.data(), bytes, &read, nullptr)) read = 0;
        }
        CloseHandle(handle);
        if (read == 0 || read != bytes) { entry.failed = true; return; }

        // The default pool keeps no second copy in the game's memory. Such textures are lost when the
        // device resets, and are loaded again the next time the game uses them.
        IDirect3DTexture9* texture = nullptr;
        const HRESULT result = g_createFromMemory(g_device, data.data(), bytes, FROM_FILE_SIZE, FROM_FILE_SIZE, FROM_FILE_MIPS, 0,
                                                  D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, DEFAULT_FILTER, DEFAULT_FILTER, 0, nullptr, nullptr, &texture);
        if (FAILED(result) || !texture) {
            entry.failed = true;
            Log::Warn("Texture %s couldn't be loaded (0x%08lX), so the game's own is shown.\n", file->second.c_str(), static_cast<unsigned long>(result));
            return;
        }
        entry.replacement = texture;
    }

    // Works out a texture's id after the game filled it, and notes it when a replacement file exists.
    static void Identify(IDirect3DTexture9* texture)
    {
        D3DSURFACE_DESC desc;
        if (FAILED(texture->GetLevelDesc(0, &desc))) return;
        // Only textures the game fills once from its files: not render targets or ones it redraws.
        if (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) return;
        if (desc.Pool != D3DPOOL_MANAGED && desc.Pool != D3DPOOL_SYSTEMMEM) return;
        const unsigned bits = BitsOf(desc.Format);
        const size_t bytes = static_cast<size_t>(bits) * desc.Width * desc.Height / 8;
        if (!bytes) return;

        D3DLOCKED_RECT rect;
        if (FAILED(texture->LockRect(0, &rect, nullptr, D3DLOCK_READONLY))) return;
        const uint32_t id = rect.pBits ? TexModCrc(static_cast<const unsigned char*>(rect.pBits), bytes) : 0;
        g_realUnlockRect(texture, 0);

        const bool hasFile = g_files.find(id) != g_files.end();
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_log && g_seen.insert(id).second)
            Log::Debug("[OpenProStreet] Textures: %08X, %ux%u, format %u%s\n", static_cast<unsigned>(id), desc.Width, desc.Height,
                       static_cast<unsigned>(desc.Format), hasFile ? ", replaced" : "");
        const std::unordered_map<IDirect3DBaseTexture9*, Entry>::iterator found = g_entries.find(texture);
        if (found != g_entries.end()) {
            if (found->second.id == id) return;
            // The game wrote something else into it.
            if (found->second.replacement) found->second.replacement->Release();
            g_entries.erase(found);
        }
        if (!hasFile) return;
        Entry entry;
        entry.id = id;
        // Loaded here, while the game is loading anyway, so nothing stutters when it's first drawn.
        LoadReplacement(entry);
        g_entries[texture] = entry;
    }

    static HRESULT WINAPI UnlockRectHook(IDirect3DTexture9* texture, UINT level)
    {
        const HRESULT result = g_realUnlockRect(texture, level);
        if (level == 0 && SUCCEEDED(result) && !g_inside) {
            g_inside = true;
            Identify(texture);
            g_inside = false;
        }
        return result;
    }

    static ULONG WINAPI ReleaseHook(IDirect3DTexture9* texture)
    {
        const ULONG left = g_realRelease(texture);
        if (left == 0 && !g_inside) {
            IDirect3DTexture9* replacement = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const std::unordered_map<IDirect3DBaseTexture9*, Entry>::iterator found = g_entries.find(texture);
                if (found != g_entries.end()) {
                    replacement = found->second.replacement;
                    g_entries.erase(found);
                }
            }
            if (replacement) {
                g_inside = true;
                replacement->Release();
                g_inside = false;
            }
        }
        return left;
    }

    // The game fills a texture in main memory and copies it to one on the graphics card: the copy is the
    // one it draws with, so it takes over the id.
    static HRESULT WINAPI UpdateTextureHook(IDirect3DDevice9* device, IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* target)
    {
        const HRESULT result = g_realUpdateTexture(device, source, target);
        if (SUCCEEDED(result) && !g_inside && source && target) {
            g_inside = true;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const std::unordered_map<IDirect3DBaseTexture9*, Entry>::iterator found = g_entries.find(source);
                if (found != g_entries.end() && g_entries.find(target) == g_entries.end()) {
                    Entry entry;
                    entry.id = found->second.id;
                    LoadReplacement(entry);
                    g_entries[target] = entry;
                }
            }
            g_inside = false;
        }
        return result;
    }

    static HRESULT WINAPI SetTextureHook(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
    {
        if (texture && !g_inside) {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_entries.empty()) {
                const std::unordered_map<IDirect3DBaseTexture9*, Entry>::iterator found = g_entries.find(texture);
                if (found != g_entries.end()) {
                    Entry& entry = found->second;
                    if (!entry.replacement && !entry.failed) {
                        g_inside = true;
                        LoadReplacement(entry);
                        g_inside = false;
                    }
                    if (entry.replacement) texture = entry.replacement;
                }
            }
        }
        return g_realSetTexture(device, stage, texture);
    }

    // Replacements live in the default pool, which a reset empties.
    static void BeforeReset(IDirect3DDevice9*)
    {
        std::vector<IDirect3DTexture9*> lost;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (std::unordered_map<IDirect3DBaseTexture9*, Entry>::iterator it = g_entries.begin(); it != g_entries.end(); ++it) {
                if (it->second.replacement) lost.push_back(it->second.replacement);
                it->second.replacement = nullptr;
                it->second.failed = false;
            }
        }
        g_inside = true;
        for (size_t i = 0; i < lost.size(); ++i) lost[i]->Release();
        g_inside = false;
    }

    static bool Hook(void* target, void* detour, void** original, const char* name)
    {
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            Log::Warn("Couldn't hook %s, so texture packs are off.\n", name);
            return false;
        }
        return true;
    }

    static void DeviceCreated(IDirect3DDevice9* device, HWND, const D3DPRESENT_PARAMETERS*)
    {
        g_device = device;
        if (g_hooked) return;
        g_hooked = true;

        const HMODULE d3dx = GetModuleHandleA("d3dx9_34.dll");
        if (d3dx) g_createFromMemory = reinterpret_cast<CreateFromMemoryFn>(GetProcAddress(d3dx, "D3DXCreateTextureFromFileInMemoryEx"));
        if (!g_createFromMemory && !g_files.empty()) {
            Log::Warn("Texture packs are off: the game's d3dx9_34.dll wasn't found.\n");
            if (!g_log) return;
        }

        // Every texture shares its functions, so a throwaway one gives their addresses.
        IDirect3DTexture9* probe = nullptr;
        g_inside = true;
        const HRESULT made = device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr);
        g_inside = false;
        if (FAILED(made) || !probe) {
            Log::Warn("Texture packs are off: a test texture couldn't be made.\n");
            return;
        }
        void** textureVtable = *reinterpret_cast<void***>(probe);
        void** deviceVtable = *reinterpret_cast<void***>(device);
        bool ok = Hook(textureVtable[TEXTURE_UNLOCK_RECT], reinterpret_cast<void*>(UnlockRectHook), reinterpret_cast<void**>(&g_realUnlockRect),
                       "IDirect3DTexture9::UnlockRect");
        if (ok && !g_files.empty()) {
            ok = Hook(textureVtable[TEXTURE_RELEASE], reinterpret_cast<void*>(ReleaseHook), reinterpret_cast<void**>(&g_realRelease),
                      "IDirect3DTexture9::Release") &&
                 Hook(deviceVtable[DEVICE_UPDATE_TEXTURE], reinterpret_cast<void*>(UpdateTextureHook), reinterpret_cast<void**>(&g_realUpdateTexture),
                      "IDirect3DDevice9::UpdateTexture") &&
                 Hook(deviceVtable[DEVICE_SET_TEXTURE], reinterpret_cast<void*>(SetTextureHook), reinterpret_cast<void**>(&g_realSetTexture),
                      "IDirect3DDevice9::SetTexture");
        }
        g_inside = true;
        probe->Release();
        g_inside = false;
        Log::Debug("[OpenProStreet] Textures: %u replacement file(s)%s\n", static_cast<unsigned>(g_files.size()), ok ? "" : ", hooks failed");
    }

    // The id in "<8 hex characters>.dds", or in TexMod's own "<game>_0x<8 hex characters>.dds".
    static bool IdFromName(const char* name, uint32_t* id)
    {
        const size_t length = std::strlen(name);
        if (length < 12 || _stricmp(name + length - 4, ".dds") != 0) return false;
        uint32_t value = 0;
        for (size_t i = length - 12; i < length - 4; ++i) {
            const char c = name[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        *id = value;
        return true;
    }

    static void AddFolder(const std::string& folder)
    {
        WIN32_FIND_DATAA entry;
        const HANDLE search = FindFirstFileA((folder + "\\*.dds").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            uint32_t id = 0;
            if (IdFromName(entry.cFileName, &id)) g_files[id] = folder + "\\" + entry.cFileName;
        } while (FindNextFileA(search, &entry));
        FindClose(search);
    }

    static void FindFiles(const std::string& root)
    {
        std::vector<std::string> packs;
        WIN32_FIND_DATAA entry;
        const HANDLE search = FindFirstFileA((root + "\\*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && entry.cFileName[0] != '.') packs.push_back(entry.cFileName);
        } while (FindNextFileA(search, &entry));
        FindClose(search);
        std::sort(packs.begin(), packs.end());
        for (size_t i = 0; i < packs.size(); ++i) AddFolder(root + "\\" + packs[i]);
        AddFolder(root);
    }

    void Init(uintptr_t)
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty()) return;
        if (GetPrivateProfileIntA("Textures", "Enabled", 1, settings.c_str()) == 0) return;
        g_log = GetPrivateProfileIntA("Textures", "Log", 0, settings.c_str()) != 0;
        const size_t slash = settings.find_last_of("\\/");
        if (slash != std::string::npos) FindFiles(settings.substr(0, slash) + "\\OpenProStreetTextures");
        if (g_files.empty() && !g_log) return;

        BuildCrcTable();
        if (!D3DHooks::Install()) {
            Log::Warn("Texture packs are off: Direct3D couldn't be hooked.\n");
            return;
        }
        D3DHooks::Listener listener;
        listener.deviceCreated = DeviceCreated;
        listener.beforeReset = BeforeReset;
        D3DHooks::AddListener(listener);
    }
}
