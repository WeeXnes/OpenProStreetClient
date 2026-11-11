#pragma once

#include <cstdint>

// FELanStateManager in nfs.exe v1.1: the menus of LAN play, from the account screen to the game
// list. Automatic login and instant quick match both drive these menus, so their hooks are here,
// and each passes the calls on to them.
namespace LanMenu
{
    // The parts of the manager both use, as offsets from the manager.
    static const int STATE = 0x04;          // the state now
    static const int NEXT_STATE = 0x10;     // where a screen change in progress goes
    static const int PREVIOUS_STATE = 0x14;
    static const int POPUP = 0x18;          // 1: show the new state's popup once the change is done
    static const int PACKAGE = 0x9C;        // the screen a change in progress loads, or null
    static const int CHANGING = 0xA0;       // byte: a screen change is waiting for a screen to close

    int State(void* manager);
    int PreviousState(void* manager);

    // No screen change is in progress, so the screen for the state is the one showing.
    bool IsSettled(void* manager);

    // Patches the manager's "state entered" and "screen shown" slots. Call it after AutoLogin and
    // QuickMatch have read their settings.
    void Init(uintptr_t baseModule);
}
