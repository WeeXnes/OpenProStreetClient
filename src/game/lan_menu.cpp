#include "game/lan_menu.h"

#include <windows.h>

#include "game/auto_login.h"
#include "game/quick_match.h"
#include "log.h"

namespace LanMenu
{
    // FELanStateManager's vtable (0x9A2AA4), as offsets from the image base.
    //
    // Changing state goes one of two ways. The setter (0x5970F0) stores the state and calls slot 80
    // (0x7D2180) at once. A screen change (0x5C4320, 0x5C43E0, 0x5C4340) only notes the new state at
    // +0x10 and asks the screen showing to close. Once it has, 0x5B97F0 stores the state, calls slot
    // 80, and only then loads the new screen (+0x9C). So slot 80 runs before the state's screen
    // exists, and starting another change from there overlaps the two.
    //
    // Slot 74 (0x7D2360) is called from 0x5978F0 (slot 6), which counts a screen that started
    // showing (+0xA4). The game moves from "logged in" (0x14) to looking for games (0x15) there, and
    // nowhere else, so it runs once the lobby's screen is up. That's the point the client acts at.
    static const uintptr_t SCREEN_SHOWN_SLOT = 0x5A2BCC;  // 0x9A2BCC
    static const uintptr_t SCREEN_SHOWN = 0x3D2360;       // 0x7D2360
    static const uintptr_t STATE_ENTERED_SLOT = 0x5A2BE4; // 0x9A2BE4
    static const uintptr_t STATE_ENTERED = 0x3D2180;      // 0x7D2180

    typedef void (__attribute__((thiscall)) *ManagerFn)(void*);

    static ManagerFn g_screenShown = nullptr;
    static ManagerFn g_stateEntered = nullptr;

    static int Field(void* manager, int offset)
    {
        return *reinterpret_cast<int*>(static_cast<char*>(manager) + offset);
    }

    int State(void* manager)
    {
        return Field(manager, STATE);
    }

    int PreviousState(void* manager)
    {
        return Field(manager, PREVIOUS_STATE);
    }

    bool IsSettled(void* manager)
    {
        return *(static_cast<uint8_t*>(manager) + CHANGING) == 0 && Field(manager, PACKAGE) == 0;
    }

    // What the menus were doing, for the detailed log, so a stuck menu can be traced.
    static void LogCall(const char* what, void* manager)
    {
        if (!Log::IsVerbose()) return;
        const char* package = *reinterpret_cast<const char**>(static_cast<char*>(manager) + PACKAGE);
        Log::Debug("[OpenProStreet] LAN menu %s: state 0x%X, next 0x%X, previous 0x%X, popup %d, changing %u, package %s\n",
                   what, State(manager), Field(manager, NEXT_STATE), Field(manager, PREVIOUS_STATE), Field(manager, POPUP),
                   static_cast<unsigned>(*(static_cast<uint8_t*>(manager) + CHANGING)), package ? package : "none");
    }

    static void __attribute__((thiscall)) StateEnteredHook(void* manager)
    {
        LogCall("state entered", manager);
        g_stateEntered(manager);
        AutoLogin::OnStateEntered(manager);
        QuickMatch::OnStateEntered(manager);
    }

    // Both act before the game's own code, which only moves on from 0x14 while the state is still 0x14.
    static void __attribute__((thiscall)) ScreenShownHook(void* manager)
    {
        LogCall("screen shown", manager);
        AutoLogin::OnScreenShown(manager);
        QuickMatch::OnScreenShown(manager);
        g_screenShown(manager);
    }

    static bool SlotHolds(uintptr_t slot, uintptr_t expected)
    {
        const uintptr_t* p = reinterpret_cast<const uintptr_t*>(slot);
        return !IsBadReadPtr(p, sizeof(*p)) && *p == expected;
    }

    static bool PatchPointer(uintptr_t slot, uintptr_t value)
    {
        uintptr_t* p = reinterpret_cast<uintptr_t*>(slot);
        DWORD oldProtect;
        if (!VirtualProtect(p, sizeof(*p), PAGE_READWRITE, &oldProtect)) return false;
        *p = value;
        VirtualProtect(p, sizeof(*p), oldProtect, &oldProtect);
        return true;
    }

    void Init(uintptr_t baseModule)
    {
        if (!AutoLogin::IsOn() && !QuickMatch::IsOn()) return;

        // Both slots or neither, and only if they hold the game's own functions.
        if (!SlotHolds(baseModule + SCREEN_SHOWN_SLOT, baseModule + SCREEN_SHOWN) ||
            !SlotHolds(baseModule + STATE_ENTERED_SLOT, baseModule + STATE_ENTERED))
        {
            Log::Warn("Automatic login and instant quick match are off: the LAN menu isn't what the client expects.\n");
            return;
        }
        g_screenShown = reinterpret_cast<ManagerFn>(baseModule + SCREEN_SHOWN);
        g_stateEntered = reinterpret_cast<ManagerFn>(baseModule + STATE_ENTERED);
        if (!PatchPointer(baseModule + SCREEN_SHOWN_SLOT, reinterpret_cast<uintptr_t>(ScreenShownHook)) ||
            !PatchPointer(baseModule + STATE_ENTERED_SLOT, reinterpret_cast<uintptr_t>(StateEnteredHook)))
        {
            Log::Warn("Automatic login and instant quick match are off: the LAN menu couldn't be changed.\n");
            return;
        }
        Log::Debug("[OpenProStreet] LAN menu hooks installed.\n");
    }
}
