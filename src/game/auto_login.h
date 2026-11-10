#ifndef OPENPROSTREET_AUTO_LOGIN_H
#define OPENPROSTREET_AUTO_LOGIN_H

#include <cstdint>

// Logs in to the player's OpenProStreet account as soon as the account screen opens, with the
// name and password saved under [Account] in the client's ini (the launcher writes them). If that
// login fails, the screen stays as it is and the player logs in by hand.
namespace AutoLogin
{
    void Init(uintptr_t baseModule);
}

#endif // OPENPROSTREET_AUTO_LOGIN_H
