#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// The game's Direct3D 9 frames, shared by everything that draws on them: the overlay and the
// display test. The hooks go in the functions themselves with MinHook, not in a device's vtable,
// so every device and every caller goes through them. Client 9.9's display test showed that
// drawing from here reaches the screen.
namespace D3DHooks
{
    struct Listener
    {
        void (*deviceCreated)(IDirect3DDevice9* device, HWND window, const D3DPRESENT_PARAMETERS* parameters) = nullptr;
        // Around a reset: anything made on the device has to be released before and made again after.
        void (*beforeReset)(IDirect3DDevice9* device) = nullptr;
        void (*afterReset)(IDirect3DDevice9* device, bool succeeded) = nullptr;
        // Each frame, just before it's shown. chain is set when the game presents through a swap
        // chain rather than the device.
        void (*beforePresent)(IDirect3DDevice9* device, IDirect3DSwapChain9* chain) = nullptr;
        // At the end of each of the game's scenes, which may draw to a surface other than the screen.
        void (*beforeEndScene)(IDirect3DDevice9* device) = nullptr;
    };

    // Hooks Direct3DCreate9 in d3d9.dll, and the rest once the game makes its device. Call it before
    // the game starts Direct3D. Safe to call more than once.
    bool Install();

    // Up to four listeners.
    void AddListener(const Listener& listener);

    // Which file a function is in, such as Windows' d3d9.dll or DXVK's.
    const char* ModuleOf(void* address);
}
