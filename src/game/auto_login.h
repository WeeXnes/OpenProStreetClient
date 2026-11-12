#ifndef OPENPROSTREET_AUTO_LOGIN_H
#define OPENPROSTREET_AUTO_LOGIN_H

#include <cstdint>

// Logs in with the player's OpenProStreet account, saved under [Account] in the client's ini (the
// launcher writes it), when the player presses "log in" on the account screen. The log in screen,
// where the name and password would be typed, is skipped. Nothing is pressed for the player. If
// that login fails, the next press of "log in" opens the log in screen as usual.
namespace AutoLogin
{
    void Init(uintptr_t baseModule);
    bool IsOn();

    // Called by LanMenu as a menu state is entered, to learn how a login went.
    void OnStateEntered(void* manager);
}

#endif // OPENPROSTREET_AUTO_LOGIN_H
