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

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        g_baseModule = baseModule;

        printf("[ProStreetRPC] Initializing Custom PNG Decals Subsystem...\n");
        ReloadDecals();
    }
}
