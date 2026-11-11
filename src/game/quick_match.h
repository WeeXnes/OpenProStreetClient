#pragma once

#include <cstdint>
#include <string>

namespace QuickMatch
{
    // Sends "Schnelles Spiel" to the OpenProStreet server through the LAN flow, then joins the
    // quick play race day the server lists first, or leaves the list to the player if it can't.
    void Init(uintptr_t baseModule);
    bool IsOn();

    // Called by LanMenu: as a menu state is entered, and once a screen of the LAN menus shows.
    void OnStateEntered(void* manager);
    void OnScreenShown(void* manager);

    // The server's quick play race day for this player, from the client channel. Empty: none yet.
    void SetServerRoom(const std::string& gameName);
}
