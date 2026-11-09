#ifndef OPENPROSTREET_TWEAKS_H
#define OPENPROSTREET_TWEAKS_H

#include <windows.h>
#include <cstdint>

namespace Tweaks
{
    // Initializes the hidden liveries unlocker and the game's replaced texts: the copyright line with
    // the client version, "OpenProStreet" on the LAN Play button, and OpenProStreet accounts in place
    // of LAN accounts
    void Init(uintptr_t baseModule);

    // The client version for the main menu's copyright line. Call before Init.
    void SetVersion(const char* version);
}

#endif // OPENPROSTREET_TWEAKS_H
