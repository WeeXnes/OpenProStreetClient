#include "decals.h"
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

namespace Decals
{
    static uintptr_t g_baseModule = 0;
    static std::vector<CustomDecal> g_customDecals;
    static void* g_origFindTexture = nullptr;
    static void* g_origGetCategoryData = nullptr;

    #pragma pack(push, 1)
    struct EATexture
    {
        void* vtable;            // 0x00
        uint32_t unk04;          // 0x04
        uint32_t unk08;          // 0x08
        uint32_t hash;           // 0x0C
        uint32_t className;      // 0x10
        void* pD3DTexture;       // 0x14 (IDirect3DTexture9*)
        uint16_t width;          // 0x18
        uint16_t height;         // 0x1A
        uint8_t mipLevels;       // 0x1C
        uint8_t format;          // 0x1D
        uint16_t unk1E;          // 0x1E
        uint32_t unk20;          // 0x20
    };
    #pragma pack(pop)

    static std::vector<EATexture> g_eaTextures;
    static std::vector<uint32_t> g_expandedHashes;
    static bool g_injectedIntoCategory = false;

    // Standard EA BinHash implementation (used across all NFS games for texture & car identifiers)
    static uint32_t bStringHash(const char* str)
    {
        uint32_t hash = 0xFFFFFFFF;
        if (!str) return hash;

        while (*str) {
            hash = (hash * 33) + (uint8_t)std::toupper((unsigned char)*str);
            str++;
        }
        return hash;
    }

    static std::vector<uint8_t> ReadFileToMemory(const std::string& path)
    {
        std::vector<uint8_t> buffer;
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            return buffer;
        }

        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize > 0 && fileSize < 50 * 1024 * 1024) { // Max 50 MB per PNG
            buffer.resize(fileSize);
            DWORD bytesRead = 0;
            ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
            if (bytesRead != fileSize) {
                buffer.clear();
            }
        }
        CloseHandle(hFile);
        return buffer;
    }

    static void LoadDecalsFromDir(const std::string& dirPath)
    {
        std::string searchPattern = dirPath + "\\*.png";
        WIN32_FIND_DATAA findData;
        HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);

        if (hFind == INVALID_HANDLE_VALUE) {
            return;
        }

        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string filename = findData.cFileName;
                std::string fullPath = dirPath + "\\" + filename;

                std::vector<uint8_t> fileData = ReadFileToMemory(fullPath);
                if (fileData.empty()) continue;

                int width = 0, height = 0, channels = 0;
                // Force 4 channels (RGBA)
                stbi_uc* pixels = stbi_load_from_memory(fileData.data(), (int)fileData.size(), &width, &height, &channels, 4);
                if (pixels && width > 0 && height > 0) {
                    CustomDecal decal;
                    decal.filename = filename;

                    // Display name without .png extension
                    size_t dotPos = filename.rfind('.');
                    decal.displayName = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;

                    // Compute unique EA BinHash for this decal
                    std::string hashKey = "PLUGIN_DEC_" + decal.displayName;
                    decal.hash = bStringHash(hashKey.c_str());
                    decal.width = width;
                    decal.height = height;
                    decal.channels = 4;
                    decal.d3dTexture = nullptr;

                    // Convert RGBA to D3D 32-bit ARGB (0xAARRGGBB)
                    decal.argbPixels.resize((size_t)width * height);
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            size_t srcIdx = (y * width + x) * 4;
                            uint8_t r = pixels[srcIdx + 0];
                            uint8_t g = pixels[srcIdx + 1];
                            uint8_t b = pixels[srcIdx + 2];
                            uint8_t a = pixels[srcIdx + 3];
                            decal.argbPixels[y * width + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                        }
                    }

                    stbi_image_free(pixels);

                    // Build mock eTexture container
                    EATexture eaTex;
                    memset(&eaTex, 0, sizeof(eaTex));
                    eaTex.hash = decal.hash;
                    eaTex.width = (uint16_t)width;
                    eaTex.height = (uint16_t)height;
                    eaTex.mipLevels = 1;
                    eaTex.format = 21; // D3DFMT_A8R8G8B8
                    eaTex.pD3DTexture = nullptr;
                    g_eaTextures.push_back(eaTex);

                    g_customDecals.push_back(decal);

                    printf("[ProStreetRPC] Custom Decal Loaded: '%s' (%dx%d, Hash: 0x%08X)\n", decal.displayName.c_str(), width, height, decal.hash);
                }
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);
    }

    void ReloadDecals()
    {
        g_customDecals.clear();
        g_eaTextures.clear();
        g_injectedIntoCategory = false;

        // Create scripts/decals directory if it doesn't exist
        CreateDirectoryA("scripts", NULL);
        CreateDirectoryA("scripts\\decals", NULL);
        CreateDirectoryA("decals", NULL);

        LoadDecalsFromDir("scripts\\decals");
        LoadDecalsFromDir("decals");

        printf("[ProStreetRPC] Total custom PNG decal(s) registered: %u\n", (unsigned)g_customDecals.size());
    }

    const std::vector<CustomDecal>& GetCustomDecals()
    {
        return g_customDecals;
    }

    static void* __attribute__((thiscall)) TexturePack_FindTexture_Hook(void* thisPtr, uint32_t hash)
    {
        for (size_t i = 0; i < g_customDecals.size(); ++i) {
            if (g_customDecals[i].hash == hash && i < g_eaTextures.size()) {
                return (void*)&g_eaTextures[i];
            }
        }

        return reinterpret_cast<void*(__attribute__((thiscall))*)(void*, uint32_t)>(g_origFindTexture)(thisPtr, hash);
    }

    static void* __attribute__((cdecl)) GetCategoryVinylData_Hook(uint32_t categoryKey)
    {
        void* res = reinterpret_cast<void*(__attribute__((cdecl))*)(uint32_t)>(g_origGetCategoryData)(categoryKey);
        
        // Category 4 = Flags (Flaggen), Category 0 = Aftermarket/Sponsors
        if (res && (categoryKey == 4 || categoryKey == 0) && !g_customDecals.empty()) {
            uintptr_t catPtr = (uintptr_t)res;
            uint32_t* origArray = *(uint32_t**)catPtr;
            uint32_t origCount = *(uint32_t*)(catPtr + 0x04);

            if (origArray && origCount > 0 && origCount < 500) {
                // Check if already injected
                bool alreadyInjected = false;
                for (uint32_t i = 0; i < origCount; ++i) {
                    if (origArray[i] == g_customDecals[0].hash) {
                        alreadyInjected = true;
                        break;
                    }
                }

                if (!alreadyInjected) {
                    g_expandedHashes.clear();
                    g_expandedHashes.reserve(origCount + g_customDecals.size());
                    for (uint32_t i = 0; i < origCount; ++i) {
                        g_expandedHashes.push_back(origArray[i]);
                    }
                    for (const auto& d : g_customDecals) {
                        g_expandedHashes.push_back(d.hash);
                    }

                    *(uint32_t**)catPtr = g_expandedHashes.data();
                    *(uint32_t*)(catPtr + 0x04) = (uint32_t)g_expandedHashes.size();
                    printf("[ProStreetRPC] Injected %u custom decal(s) into category %u (Total: %u)\n", 
                           (unsigned)g_customDecals.size(), categoryKey, (unsigned)g_expandedHashes.size());
                }
            }
        }

        return res;
    }

    static bool InstallDetour(void* target, void* detour, void** original, size_t patchSize = 6)
    {
        if (!target || IsBadReadPtr(target, patchSize)) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

        BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline) {
            VirtualProtect(target, patchSize, oldProtect, &oldProtect);
            return false;
        }

        memcpy(trampoline, target, patchSize);

        trampoline[patchSize] = 0xE9;
        uintptr_t jmpBackOffset = ((uintptr_t)target + patchSize) - ((uintptr_t)trampoline + patchSize + 5);
        memcpy(&trampoline[patchSize + 1], &jmpBackOffset, 4);

        if (original) {
            *original = (void*)trampoline;
        }

        std::vector<BYTE> jmpBuf(patchSize, 0x90);
        jmpBuf[0] = 0xE9;
        uintptr_t detourOffset = (uintptr_t)detour - ((uintptr_t)target + 5);
        memcpy(&jmpBuf[1], &detourOffset, 4);
        memcpy(target, jmpBuf.data(), patchSize);

        VirtualProtect(target, patchSize, oldProtect, &oldProtect);
        return true;
    }

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        g_baseModule = baseModule;

        printf("[ProStreetRPC] Initializing Custom PNG Decals Subsystem...\n");
        ReloadDecals();

        uintptr_t loc_4614B0 = baseModule + 0x614B0;
        if (InstallDetour((void*)loc_4614B0, (void*)TexturePack_FindTexture_Hook, &g_origFindTexture, 6)) {
            printf("[ProStreetRPC] TexturePack::FindTexture hook installed at 0x%p\n", (void*)loc_4614B0);
        }

        uintptr_t loc_754E00 = baseModule + 0x354E00;
        if (InstallDetour((void*)loc_754E00, (void*)GetCategoryVinylData_Hook, &g_origGetCategoryData, 6)) {
            printf("[ProStreetRPC] Category Vinyl Populator hook installed at 0x%p\n", (void*)loc_754E00);
        }
    }
}
