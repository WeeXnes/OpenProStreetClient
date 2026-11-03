#ifndef OPENPROSTREET_RACE_MODE_H
#define OPENPROSTREET_RACE_MODE_H

#include <windows.h>
#include <string>
#include <cstdint>

// The name of an event's mode, or empty if it isn't known. A drag race needs the event's
// race name id for its distance.
std::string RaceModeName(int mode, int raceNameId = 0);

// True only from race start until the race ends, not while an event is merely loaded in the menus
bool IsRaceActive(uintptr_t baseModule);

// Reads and translates the race / event mode, or the last one seen once the race has ended
std::string ReadRaceMode(uintptr_t baseModule);

#endif // OPENPROSTREET_RACE_MODE_H
