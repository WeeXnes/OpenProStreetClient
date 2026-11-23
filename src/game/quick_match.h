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

    // Discord invites: accepts an invite secret (from OnActivityJoin), resolves it with the server,
    // and joins that room automatically when entering LAN Play.
    void RequestInviteJoin(const std::string& secret);
    void SetInviteRoom(const std::string& gameName, const std::string& displayName, const std::string& error);
    void OnServerConnected();
}
