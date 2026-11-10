#include "game/d3d_hooks.h"

#include "log.h"

namespace D3DHooks
{
    // nfs.exe v1.1, as an offset from the image base.
    static const uintptr_t IAT_DIRECT3D_CREATE = 0x5675BC;

    static const size_t VTABLE_CREATE_DEVICE = 16; // IDirect3D9::CreateDevice
    static const size_t VTABLE_RESET = 16;         // IDirect3DDevice9::Reset
    static const size_t VTABLE_PRESENT = 17;       // IDirect3DDevice9::Present

    typedef IDirect3D9* (WINAPI *Direct3DCreateFn)(UINT);
    typedef HRESULT (WINAPI *CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    typedef HRESULT (WINAPI *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (WINAPI *PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

    static Direct3DCreateFn g_realDirect3DCreate = nullptr;
    static CreateDeviceFn g_realCreateDevice = nullptr;
    static ResetFn g_realReset = nullptr;
    static PresentFn g_realPresent = nullptr;
    static bool g_installed = false;

    static Listener g_listeners[4];
    static int g_listenerCount = 0;

    static void PatchPointer(void** slot, void* value)
    {
        DWORD oldProtect;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return;
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    }

    static HRESULT WINAPI PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
    {
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].beforePresent) g_listeners[i].beforePresent(device);
        return g_realPresent(device, source, dest, window, dirty);
    }

    static HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
    {
        for (int i = 0; i < g_listenerCount; ++i)
        {
            if (g_listeners[i].adjustParameters) g_listeners[i].adjustParameters(parameters);
            if (g_listeners[i].beforeReset) g_listeners[i].beforeReset(device);
        }
        const HRESULT result = g_realReset(device, parameters);
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].afterReset) g_listeners[i].afterReset(device, SUCCEEDED(result));
        return result;
    }

    static HRESULT WINAPI CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device)
    {
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].adjustParameters) g_listeners[i].adjustParameters(parameters);
        const HRESULT result = g_realCreateDevice(d3d, adapter, type, window, flags, parameters, device);
        if (FAILED(result) || !device || !*device)
        {
            Log::Error("The game couldn't create its display (0x%08lX).\n", static_cast<unsigned long>(result));
            return result;
        }
        // Devices of one Direct3D share their vtable, so this happens once.
        if (!g_realReset)
        {
            void** vtable = *reinterpret_cast<void***>(*device);
            g_realReset = reinterpret_cast<ResetFn>(vtable[VTABLE_RESET]);
            PatchPointer(&vtable[VTABLE_RESET], reinterpret_cast<void*>(ResetHook));
            g_realPresent = reinterpret_cast<PresentFn>(vtable[VTABLE_PRESENT]);
            PatchPointer(&vtable[VTABLE_PRESENT], reinterpret_cast<void*>(PresentHook));
        }
        HWND focus = window ? window : (parameters ? parameters->hDeviceWindow : nullptr);
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].deviceCreated) g_listeners[i].deviceCreated(*device, focus);
        return result;
    }

    static IDirect3D9* WINAPI Direct3DCreateHook(UINT version)
    {
        IDirect3D9* d3d = g_realDirect3DCreate(version);
        if (d3d && !g_realCreateDevice)
        {
            void** vtable = *reinterpret_cast<void***>(d3d);
            g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[VTABLE_CREATE_DEVICE]);
            PatchPointer(&vtable[VTABLE_CREATE_DEVICE], reinterpret_cast<void*>(CreateDeviceHook));
        }
        return d3d;
    }

    bool Install(uintptr_t baseModule)
    {
        if (g_installed) return true;
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        void* real = d3d9 ? reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
        void** entry = reinterpret_cast<void**>(baseModule + IAT_DIRECT3D_CREATE);
        if (!real || IsBadReadPtr(entry, sizeof(void*)) || *entry != real) return false;
        g_realDirect3DCreate = reinterpret_cast<Direct3DCreateFn>(real);
        PatchPointer(entry, reinterpret_cast<void*>(Direct3DCreateHook));
        g_installed = true;
        Log::Debug("[OpenProStreet] Display hooks installed.\n");
        return true;
    }

    void AddListener(const Listener& listener)
    {
        if (g_listenerCount < 4) g_listeners[g_listenerCount++] = listener;
    }
}
