#include "decals.h"
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <d3d9.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

namespace Decals
{
    static uintptr_t g_baseModule = 0;
    static std::vector<CustomDecal> g_customDecals;
    static void* g_origFindTexture = nullptr;
    static void* g_origGetCustomizationPartsList = nullptr;

    #pragma pack(push, 1)
    struct EATexture
    {
        void* vtable;            // 0x00
        uint32_t unk04;          // 0x04
        uint32_t unk08;          // 0x08
        uint32_t hash;           // 0x0C
        uint32_t className;      // 0x10
        IDirect3DTexture9* pD3DTexture; // 0x14
        uint16_t width;          // 0x18
        uint16_t height;         // 0x1A
        uint8_t mipLevels;       // 0x1C
        uint8_t format;          // 0x1D
        uint16_t unk1E;          // 0x1E
        uint32_t unk20;          // 0x20
    };
    #pragma pack(pop)

    static std::vector<EATexture> g_eaTextures;

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

                    // Build EATexture container
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

    static IDirect3DDevice9* GetD3D9Device()
    {
        if (!g_baseModule) return nullptr;
        uintptr_t pDevBase = g_baseModule + 0x6CE7E4; // 0xACE7E4
        if (IsBadReadPtr((void*)pDevBase, sizeof(uintptr_t)) == 0) {
            uintptr_t devPtr = *(uintptr_t*)pDevBase;
            if (devPtr != 0 && !IsBadReadPtr((void*)devPtr, sizeof(uintptr_t))) {
                return (IDirect3DDevice9*)devPtr;
            }
        }
        return nullptr;
    }

    static void EnsureD3DTextureCreated(size_t index)
    {
        if (index >= g_customDecals.size() || index >= g_eaTextures.size()) return;
        if (g_eaTextures[index].pD3DTexture != nullptr) return;

        IDirect3DDevice9* pDev = GetD3D9Device();
        if (!pDev) return;

        CustomDecal& decal = g_customDecals[index];
        IDirect3DTexture9* pTex = nullptr;

        HRESULT hr = pDev->CreateTexture(
            (UINT)decal.width,
            (UINT)decal.height,
            1,
            0,
            D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED,
            &pTex,
            NULL
        );

        if (SUCCEEDED(hr) && pTex) {
            D3DLOCKED_RECT rect;
            if (SUCCEEDED(pTex->LockRect(0, &rect, NULL, 0))) {
                uint8_t* pDest = (uint8_t*)rect.pBits;
                for (int y = 0; y < decal.height; ++y) {
                    memcpy(pDest + (y * rect.Pitch), &decal.argbPixels[y * decal.width], decal.width * 4);
                }
                pTex->UnlockRect(0);
            }
            g_eaTextures[index].pD3DTexture = pTex;
            decal.d3dTexture = pTex;
            printf("[ProStreetRPC] D3D9 Texture generated for decal '%s' (0x%08X)\n", decal.displayName.c_str(), decal.hash);
        }
    }

    static void* __attribute__((thiscall)) TexturePack_FindTexture_Hook(void* thisPtr, uint32_t hash)
    {
        for (size_t i = 0; i < g_customDecals.size(); ++i) {
            if (g_customDecals[i].hash == hash && i < g_eaTextures.size()) {
                EnsureD3DTextureCreated(i);
                return (void*)&g_eaTextures[i];
            }
        }

        return reinterpret_cast<void*(__attribute__((thiscall))*)(void*, uint32_t)>(g_origFindTexture)(thisPtr, hash);
    }

    static void* __attribute__((thiscall)) DAL_GetCustomizationPartsList_Hook(void* thisPtr, uint8_t slotType)
    {
        void* res = reinterpret_cast<void*(__attribute__((thiscall))*)(void*, uint8_t)>(g_origGetCustomizationPartsList)(thisPtr, slotType);

        if (res && !g_customDecals.empty() && !IsBadReadPtr(res, 64)) {
            uint32_t* pCount = (uint32_t*)res;
            uint32_t count = *pCount;
            uint32_t* hashes = (uint32_t*)((uintptr_t)res + 4);

            if (count > 0 && count < 200 && !IsBadReadPtr(hashes, count * 4)) {
                bool alreadyInjected = false;
                for (uint32_t i = 0; i < count; ++i) {
                    if (hashes[i] == g_customDecals[0].hash) {
                        alreadyInjected = true;
                        break;
                    }
                }

                if (!alreadyInjected) {
                    for (size_t i = 0; i < g_customDecals.size() && (count + i) < 500; ++i) {
                        hashes[count + i] = g_customDecals[i].hash;
                    }
                    *pCount = count + (uint32_t)g_customDecals.size();
                    printf("[ProStreetRPC] Injected %u custom decal(s) into Decal Slot %u (New total: %u)\n",
                           (unsigned)g_customDecals.size(), (unsigned)slotType, *pCount);
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

        // Hook TexturePack::FindTexture at 0x004614B0 (exact 6 bytes)
        uintptr_t loc_4614B0 = baseModule + 0x614B0;
        if (InstallDetour((void*)loc_4614B0, (void*)TexturePack_FindTexture_Hook, &g_origFindTexture, 6)) {
            printf("[ProStreetRPC] TexturePack::FindTexture hook installed at 0x%p\n", (void*)loc_4614B0);
        }

        // Hook DALCarCustomization::GetPartsList at 0x00533410 (exact 6 bytes)
        uintptr_t loc_533410 = baseModule + 0x133410;
        if (InstallDetour((void*)loc_533410, (void*)DAL_GetCustomizationPartsList_Hook, &g_origGetCustomizationPartsList, 6)) {
            printf("[ProStreetRPC] DAL::GetCustomizationPartsList hook installed at 0x%p\n", (void*)loc_533410);
        }
    }
}
