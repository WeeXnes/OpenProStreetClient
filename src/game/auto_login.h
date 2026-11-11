#ifndef OPENPROSTREET_AUTO_LOGIN_H
#define OPENPROSTREET_AUTO_LOGIN_H

#include <cstdint>

// Logs in to the player's OpenProStreet account as soon as the account screen opens, with the
// name and password saved under [Account] in the client's ini (the launcher writes them). It
// presses the game's own "log in" and OK buttons, each once its screen is showing. If that login
// fails, the screens stay as they are and the player logs in by hand.
namespace AutoLogin
{
    void Init(uintptr_t baseModule);
    bool IsOn();

    // Called by LanMenu: as a menu state is entered, and once a screen of the LAN menus shows.
    void OnStateEntered(void* manager);
    void OnScreenShown(void* manager);
}

#endif // OPENPROSTREET_AUTO_LOGIN_H
