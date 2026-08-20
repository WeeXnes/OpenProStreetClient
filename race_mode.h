#ifndef PROSTREETRPC_RACE_MODE_H
#define PROSTREETRPC_RACE_MODE_H

#include <windows.h>
#include <string>
#include <cstdint>

// Reads and translates the current race / event mode from game memory
std::string ReadRaceMode(uintptr_t baseModule);

#endif // PROSTREETRPC_RACE_MODE_H
