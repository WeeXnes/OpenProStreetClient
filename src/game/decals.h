#ifndef OPENPROSTREET_DECALS_H
#define OPENPROSTREET_DECALS_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace Decals
{
    struct CustomDecal
    {
        std::string filename;
        std::string displayName;
        uint32_t hash;
        int width;
        int height;
        int channels;
        std::vector<uint32_t> argbPixels;
        void* d3dTexture; // IDirect3DTexture9*
    };

    void Init(uintptr_t baseModule);
    void ReloadDecals();
    const std::vector<CustomDecal>& GetCustomDecals();
}

#endif // OPENPROSTREET_DECALS_H
