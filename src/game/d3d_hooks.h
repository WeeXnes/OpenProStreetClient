#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// The game's Direct3D 9 device, shared by everything that needs it: borderless mode changes how the
// device is made, and the overlay draws on each frame. The hooks go in once, whoever asks first.
namespace D3DHooks
{
    struct Listener
    {
        // Before the device is made or reset: a chance to change its settings.
        void (*adjustParameters)(D3DPRESENT_PARAMETERS* parameters) = nullptr;
        void (*deviceCreated)(IDirect3DDevice9* device, HWND window) = nullptr;
        // Around a reset: anything made on the device has to be released before and made again after.
        void (*beforeReset)(IDirect3DDevice9* device) = nullptr;
        void (*afterReset)(IDirect3DDevice9* device, bool succeeded) = nullptr;
        // Each frame, before it's shown.
        void (*beforePresent)(IDirect3DDevice9* device) = nullptr;
    };

    // Hooks the game's Direct3DCreate9 import. False when it isn't what v1.1 has, for example
    // because another plugin already replaced it.
    bool Install(uintptr_t baseModule);

    // Up to four listeners.
    void AddListener(const Listener& listener);
}
