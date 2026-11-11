#include "game/d3d_hooks.h"

#include "log.h"
#include "minhook/MinHook.h"

namespace D3DHooks
{
    // Vtable indices, the same in every Direct3D 9.
    static const size_t D3D_CREATE_DEVICE = 16;  // IDirect3D9::CreateDevice
    static const size_t DEVICE_RESET = 16;       // IDirect3DDevice9::Reset
    static const size_t DEVICE_PRESENT = 17;     // IDirect3DDevice9::Present
    static const size_t DEVICE_END_SCENE = 42;   // IDirect3DDevice9::EndScene
    static const size_t CHAIN_PRESENT = 3;       // IDirect3DSwapChain9::Present

    typedef IDirect3D9* (WINAPI *Direct3DCreateFn)(UINT);
    typedef HRESULT (WINAPI *CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    typedef HRESULT (WINAPI *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (WINAPI *PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    typedef HRESULT (WINAPI *EndSceneFn)(IDirect3DDevice9*);
    typedef HRESULT (WINAPI *ChainPresentFn)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

    static Direct3DCreateFn g_realDirect3DCreate = nullptr;
    static CreateDeviceFn g_realCreateDevice = nullptr;
    static ResetFn g_realReset = nullptr;
    static PresentFn g_realPresent = nullptr;
    static EndSceneFn g_realEndScene = nullptr;
    static ChainPresentFn g_realChainPresent = nullptr;

    static Listener g_listeners[4];
    static int g_listenerCount = 0;
    static bool g_installed = false;
    // Drawing calls EndScene and may present through the swap chain itself: those aren't the game's.
    static thread_local bool g_inside = false;

    const char* ModuleOf(void* address)
    {
        static char path[MAX_PATH];
        HMODULE module = nullptr;
        lstrcpyA(path, "unknown");
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module) && module)
            GetModuleFileNameA(module, path, sizeof(path));
        return path;
    }

    static bool Hook(void* target, void* detour, void** original, const char* name)
    {
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            Log::Warn("Couldn't hook %s, so the overlay can't draw.\n", name);
            return false;
        }
        Log::Debug("[OpenProStreet] Hooked %s in %s\n", name, ModuleOf(target));
        return true;
    }

    static void BeforePresent(IDirect3DDevice9* device, IDirect3DSwapChain9* chain)
    {
        g_inside = true;
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].beforePresent) g_listeners[i].beforePresent(device, chain);
        g_inside = false;
    }

    static HRESULT WINAPI PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
    {
        if (!g_inside) BeforePresent(device, nullptr);
        // The device's Present may go through the swap chain's: that one is skipped.
        const bool outer = !g_inside;
        g_inside = true;
        const HRESULT result = g_realPresent(device, source, dest, window, dirty);
        if (outer) g_inside = false;
        return result;
    }

    static HRESULT WINAPI ChainPresentHook(IDirect3DSwapChain9* chain, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty, DWORD flags)
    {
        if (!g_inside) {
            IDirect3DDevice9* device = nullptr;
            if (SUCCEEDED(chain->GetDevice(&device)) && device) {
                BeforePresent(device, chain);
                device->Release();
            }
        }
        return g_realChainPresent(chain, source, dest, window, dirty, flags);
    }

    static HRESULT WINAPI EndSceneHook(IDirect3DDevice9* device)
    {
        if (!g_inside) {
            g_inside = true;
            for (int i = 0; i < g_listenerCount; ++i)
                if (g_listeners[i].beforeEndScene) g_listeners[i].beforeEndScene(device);
            g_inside = false;
        }
        return g_realEndScene(device);
    }

    static HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
    {
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].beforeReset) g_listeners[i].beforeReset(device);
        const HRESULT result = g_realReset(device, parameters);
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].afterReset) g_listeners[i].afterReset(device, SUCCEEDED(result));
        return result;
    }

    static HRESULT WINAPI CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device)
    {
        const HRESULT result = g_realCreateDevice(d3d, adapter, type, window, flags, parameters, device);
        if (FAILED(result) || !device || !*device) return result;

        // The functions are shared by every device, so each is hooked once.
        void** vtable = *reinterpret_cast<void***>(*device);
        if (!g_realPresent) {
            Hook(vtable[DEVICE_PRESENT], reinterpret_cast<void*>(PresentHook), reinterpret_cast<void**>(&g_realPresent), "IDirect3DDevice9::Present");
            Hook(vtable[DEVICE_END_SCENE], reinterpret_cast<void*>(EndSceneHook), reinterpret_cast<void**>(&g_realEndScene), "IDirect3DDevice9::EndScene");
            Hook(vtable[DEVICE_RESET], reinterpret_cast<void*>(ResetHook), reinterpret_cast<void**>(&g_realReset), "IDirect3DDevice9::Reset");
        }
        if (!g_realChainPresent) {
            IDirect3DSwapChain9* chain = nullptr;
            if (SUCCEEDED((*device)->GetSwapChain(0, &chain)) && chain) {
                void** chainVtable = *reinterpret_cast<void***>(chain);
                Hook(chainVtable[CHAIN_PRESENT], reinterpret_cast<void*>(ChainPresentHook), reinterpret_cast<void**>(&g_realChainPresent),
                     "IDirect3DSwapChain9::Present");
                chain->Release();
            }
        }
        HWND focus = window ? window : (parameters ? parameters->hDeviceWindow : nullptr);
        for (int i = 0; i < g_listenerCount; ++i)
            if (g_listeners[i].deviceCreated) g_listeners[i].deviceCreated(*device, focus, parameters);
        return result;
    }

    static IDirect3D9* WINAPI Direct3DCreateHook(UINT version)
    {
        IDirect3D9* d3d = g_realDirect3DCreate(version);
        if (d3d && !g_realCreateDevice) {
            void** vtable = *reinterpret_cast<void***>(d3d);
            Hook(vtable[D3D_CREATE_DEVICE], reinterpret_cast<void*>(CreateDeviceHook), reinterpret_cast<void**>(&g_realCreateDevice), "IDirect3D9::CreateDevice");
        }
        return d3d;
    }

    bool Install()
    {
        if (g_installed) return true;
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        void* create = d3d9 ? reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
        if (!create) return false;
        const MH_STATUS started = MH_Initialize();
        if (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (!Hook(create, reinterpret_cast<void*>(Direct3DCreateHook), reinterpret_cast<void**>(&g_realDirect3DCreate), "Direct3DCreate9")) return false;
        g_installed = true;
        Log::Debug("[OpenProStreet] Display hooks installed, through %s\n", ModuleOf(create));
        return true;
    }

    void AddListener(const Listener& listener)
    {
        if (g_listenerCount < 4) g_listeners[g_listenerCount++] = listener;
    }
}
