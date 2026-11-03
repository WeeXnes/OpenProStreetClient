#include "game/input_focus.h"

#include <cstring>
#include <mutex>
#include <set>

#include "log.h"

namespace InputFocus
{
    // nfs.exe v1.1, as an offset from the image base.
    static const uintptr_t IAT_DIRECT_INPUT_CREATE = 0x567030;

    // IDirectInput8A and IDirectInputDevice8A vtable slots.
    static const size_t VTABLE_CREATE_DEVICE = 3;
    static const size_t VTABLE_GET_DEVICE_STATE = 9;
    static const size_t VTABLE_GET_DEVICE_DATA = 10;

    // From dinput.h, without linking dxguid.
    static const GUID SYS_KEYBOARD = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const GUID SYS_MOUSE = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const DWORD MOUSE_BUTTON0_OFFSET = 12; // DIMOFS_BUTTON0: lower offsets are the axes

    // DIDEVICEOBJECTDATA, from dinput.h.
    struct DeviceObjectData {
        DWORD offset;
        DWORD data;
        DWORD timeStamp;
        DWORD sequence;
        UINT_PTR appData;
    };

    typedef HRESULT (WINAPI *DirectInputCreateFn)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    typedef HRESULT (WINAPI *CreateDeviceFn)(void*, REFGUID, void**, IUnknown*);
    typedef HRESULT (WINAPI *GetDeviceStateFn)(void*, DWORD, void*);
    typedef HRESULT (WINAPI *GetDeviceDataFn)(void*, DWORD, DeviceObjectData*, DWORD*, DWORD);

    static DirectInputCreateFn g_realDirectInputCreate = nullptr;
    static CreateDeviceFn g_realCreateDevice = nullptr;
    static GetDeviceStateFn g_realGetDeviceState = nullptr;
    static GetDeviceDataFn g_realGetDeviceData = nullptr;

    static std::mutex g_lock;
    static std::set<void*> g_keyboards;
    static std::set<void*> g_mice;

    static void PatchPointer(void** slot, void* value)
    {
        DWORD oldProtect;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return;
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    }

    // True while one of this process's windows is in front.
    static bool HasFocus()
    {
        HWND front = GetForegroundWindow();
        DWORD process = 0;
        if (front) GetWindowThreadProcessId(front, &process);
        return process == GetCurrentProcessId();
    }

    enum class Kind { Other, Keyboard, Mouse };

    static Kind KindOf(void* device)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_keyboards.count(device)) return Kind::Keyboard;
        if (g_mice.count(device)) return Kind::Mouse;
        return Kind::Other;
    }

    // The whole state of a keyboard (256 keys) or mouse reads as untouched while unfocused.
    static HRESULT WINAPI GetDeviceStateHook(void* device, DWORD size, void* data)
    {
        const HRESULT result = g_realGetDeviceState(device, size, data);
        if (SUCCEEDED(result) && data && KindOf(device) != Kind::Other && !HasFocus()) memset(data, 0, size);
        return result;
    }

    // Buffered input keeps its releases, so a key held while tabbing out isn't stuck afterwards.
    // Presses and mouse movement are dropped.
    static HRESULT WINAPI GetDeviceDataHook(void* device, DWORD objectSize, DeviceObjectData* events, DWORD* count, DWORD flags)
    {
        const HRESULT result = g_realGetDeviceData(device, objectSize, events, count, flags);
        if (FAILED(result) || !events || !count || objectSize < sizeof(DeviceObjectData) || HasFocus()) return result;
        const Kind kind = KindOf(device);
        if (kind == Kind::Other) return result;

        DWORD kept = 0;
        for (DWORD i = 0; i < *count; ++i) {
            DeviceObjectData* event = reinterpret_cast<DeviceObjectData*>(reinterpret_cast<uint8_t*>(events) + i * objectSize);
            const bool isButton = kind == Kind::Keyboard || event->offset >= MOUSE_BUTTON0_OFFSET;
            const bool isRelease = isButton && (event->data & 0x80) == 0;
            if (!isRelease) continue;
            if (kept != i) memcpy(reinterpret_cast<uint8_t*>(events) + kept * objectSize, event, objectSize);
            ++kept;
        }
        *count = kept;
        return result;
    }

    static HRESULT WINAPI CreateDeviceHook(void* directInput, REFGUID type, void** device, IUnknown* outer)
    {
        const HRESULT result = g_realCreateDevice(directInput, type, device, outer);
        if (FAILED(result) || !device || !*device) return result;

        const bool keyboard = IsEqualGUID(type, SYS_KEYBOARD) != 0;
        const bool mouse = IsEqualGUID(type, SYS_MOUSE) != 0;
        if (!keyboard && !mouse) return result;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            (keyboard ? g_keyboards : g_mice).insert(*device);
        }
        // Devices of one DirectInput share their vtable, so this only happens once.
        void** vtable = *reinterpret_cast<void***>(*device);
        if (vtable[VTABLE_GET_DEVICE_STATE] != reinterpret_cast<void*>(GetDeviceStateHook)) {
            g_realGetDeviceState = reinterpret_cast<GetDeviceStateFn>(vtable[VTABLE_GET_DEVICE_STATE]);
            g_realGetDeviceData = reinterpret_cast<GetDeviceDataFn>(vtable[VTABLE_GET_DEVICE_DATA]);
            PatchPointer(&vtable[VTABLE_GET_DEVICE_STATE], reinterpret_cast<void*>(GetDeviceStateHook));
            PatchPointer(&vtable[VTABLE_GET_DEVICE_DATA], reinterpret_cast<void*>(GetDeviceDataHook));
            Log::Debug("[OpenProStreet] Keyboard and mouse input is ignored while the game is in the background.\n");
        }
        return result;
    }

    static HRESULT WINAPI DirectInputCreateHook(HINSTANCE instance, DWORD version, REFIID iid, void** out, IUnknown* outer)
    {
        const HRESULT result = g_realDirectInputCreate(instance, version, iid, out, outer);
        if (SUCCEEDED(result) && out && *out && !g_realCreateDevice) {
            void** vtable = *reinterpret_cast<void***>(*out);
            g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[VTABLE_CREATE_DEVICE]);
            PatchPointer(&vtable[VTABLE_CREATE_DEVICE], reinterpret_cast<void*>(CreateDeviceHook));
        }
        return result;
    }

    void Init(uintptr_t baseModule)
    {
        // The game's dinput8.dll can be an ASI loader that forwards to the real one. Either way, the
        // import must hold what that module exports, or another plugin already hooked it.
        HMODULE dinput = GetModuleHandleA("dinput8.dll");
        void* real = dinput ? reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
        void** entry = reinterpret_cast<void**>(baseModule + IAT_DIRECT_INPUT_CREATE);
        if (!real || IsBadReadPtr(entry, sizeof(void*)) || *entry != real) {
            Log::Warn("The game's input setup isn't as expected, so it keeps reading input in the background.\n");
            return;
        }
        g_realDirectInputCreate = reinterpret_cast<DirectInputCreateFn>(real);
        PatchPointer(entry, reinterpret_cast<void*>(DirectInputCreateHook));
    }
}
