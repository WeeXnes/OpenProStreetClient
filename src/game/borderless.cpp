#include "game/borderless.h"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <string>

#include "game/hooks.h"
#include "log.h"

namespace Borderless
{
    // nfs.exe v1.1, as offsets from the image base.
    static const uintptr_t IAT_CREATE_WINDOW = 0x5674DC;
    static const uintptr_t IAT_DIRECT3D_CREATE = 0x5675BC;
    static const uintptr_t SET_INACTIVE = 0x305FA0; // 0x705FA0: called on WM_ACTIVATEAPP

    // The same index in both vtables.
    static const size_t VTABLE_CREATE_DEVICE = 16; // IDirect3D9::CreateDevice
    static const size_t VTABLE_RESET = 16;         // IDirect3DDevice9::Reset

    typedef HWND (WINAPI *CreateWindowFn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    typedef IDirect3D9* (WINAPI *Direct3DCreateFn)(UINT);
    typedef HRESULT (WINAPI *CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    typedef HRESULT (WINAPI *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef void (__attribute__((thiscall)) *SetInactiveFn)(void*, unsigned char);

    static CreateWindowFn g_realCreateWindow = nullptr;
    static Direct3DCreateFn g_realDirect3DCreate = nullptr;
    static CreateDeviceFn g_realCreateDevice = nullptr;
    static ResetFn g_realReset = nullptr;
    static void* g_origSetInactive = nullptr;

    static void PatchPointer(void** slot, void* value)
    {
        DWORD oldProtect;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return;
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    }

    // A windowed device can't ask for a refresh rate. It presents to whatever size the window is,
    // so the game's resolution setting still decides the size it renders at.
    static void ForceWindowed(D3DPRESENT_PARAMETERS* parameters)
    {
        if (!parameters) return;
        parameters->Windowed = TRUE;
        parameters->FullScreen_RefreshRateInHz = 0;
    }

    static HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
    {
        ForceWindowed(parameters);
        return g_realReset(device, parameters);
    }

    static HRESULT WINAPI CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device)
    {
        ForceWindowed(parameters);
        const HRESULT result = g_realCreateDevice(d3d, adapter, type, window, flags, parameters, device);
        if (SUCCEEDED(result) && device && *device && !g_realReset) {
            void** vtable = *reinterpret_cast<void***>(*device);
            g_realReset = reinterpret_cast<ResetFn>(vtable[VTABLE_RESET]);
            PatchPointer(&vtable[VTABLE_RESET], reinterpret_cast<void*>(ResetHook));
        }
        if (FAILED(result)) Log::Error("Could not create the borderless display (0x%08lX).\n", static_cast<unsigned long>(result));
        return result;
    }

    static IDirect3D9* WINAPI Direct3DCreateHook(UINT version)
    {
        IDirect3D9* d3d = g_realDirect3DCreate(version);
        if (d3d && !g_realCreateDevice) {
            void** vtable = *reinterpret_cast<void***>(d3d);
            g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[VTABLE_CREATE_DEVICE]);
            PatchPointer(&vtable[VTABLE_CREATE_DEVICE], reinterpret_cast<void*>(CreateDeviceHook));
        }
        return d3d;
    }

    static HWND WINAPI CreateWindowHook(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param)
    {
        if (!IS_INTRESOURCE(className) && strcmp(className, "GameFrame") == 0) {
            style = WS_POPUP;
            x = 0;
            y = 0;
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
            Log::Event(Log::Tag::Client, "Borderless fullscreen at %dx%d\n", width, height);
        }
        return g_realCreateWindow(exStyle, className, title, style, x, y, width, height, parent, menu, instance, param);
    }

    // Losing focus normally stops the game's frames until it's back (0x4B4590), and asks for a
    // device reset on the way back. A windowed device isn't lost, so neither is needed. Being
    // minimized still stops it, because that goes through WM_SIZE instead.
    static void __attribute__((thiscall)) SetInactiveHook(void* display, unsigned char)
    {
        reinterpret_cast<SetInactiveFn>(g_origSetInactive)(display, 0);
    }

    // Only replaces an import that holds the function it should, so a different build of the game
    // or another plugin's hook is left alone.
    static void* HookImport(uintptr_t slot, const char* module, const char* name, void* hook)
    {
        HMODULE loaded = GetModuleHandleA(module);
        void* real = loaded ? reinterpret_cast<void*>(GetProcAddress(loaded, name)) : nullptr;
        void** entry = reinterpret_cast<void**>(slot);
        if (!real || IsBadReadPtr(entry, sizeof(void*)) || *entry != real) return nullptr;
        PatchPointer(entry, hook);
        return real;
    }

    void Init(uintptr_t baseModule)
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty() || GetPrivateProfileIntA("Display", "Borderless", 0, settings.c_str()) == 0) return;

        g_realCreateWindow = reinterpret_cast<CreateWindowFn>(HookImport(baseModule + IAT_CREATE_WINDOW, "user32.dll", "CreateWindowExA", reinterpret_cast<void*>(CreateWindowHook)));
        if (!g_realCreateWindow) {
            Log::Warn("Borderless fullscreen is off: another plugin may already be handling the game's window.\n");
            return;
        }
        g_realDirect3DCreate = reinterpret_cast<Direct3DCreateFn>(HookImport(baseModule + IAT_DIRECT3D_CREATE, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(Direct3DCreateHook)));
        if (!g_realDirect3DCreate) {
            PatchPointer(reinterpret_cast<void**>(baseModule + IAT_CREATE_WINDOW), reinterpret_cast<void*>(g_realCreateWindow));
            Log::Warn("Borderless fullscreen is off: another plugin may already be handling the game's graphics.\n");
            return;
        }
        if (!Hooks::InstallDetour(reinterpret_cast<void*>(baseModule + SET_INACTIVE), reinterpret_cast<void*>(SetInactiveHook), &g_origSetInactive)) {
            Log::Error("Could not install the borderless focus hook.\n");
        }
        Log::Debug("[OpenProStreet] Borderless fullscreen hooks installed.\n");
    }
}
