#ifndef OPENPROSTREET_CAR_LOCK_H
#define OPENPROSTREET_CAR_LOCK_H

#include <windows.h>
#include <cstdint>
#include <string>

// Hides the cars a race day doesn't allow from the LAN car selection screen. The server sends the
// room's allowed cars when the player joins, and also refuses any other car, so an old or modified
// plugin can't get around the rule.
namespace CarLock
{
    void Init(uintptr_t baseModule);

    // Model names from the server, separated by commas. Empty allows every car.
    void SetAllowedCars(const std::string& models);
}

#endif // OPENPROSTREET_CAR_LOCK_H
