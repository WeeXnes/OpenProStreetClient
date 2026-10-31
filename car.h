#ifndef PROSTREETRPC_CAR_H
#define PROSTREETRPC_CAR_H

#include <windows.h>
#include <string>
#include <cstdint>

// Reads the local player's current car model name (e.g. "RX8") from the game's car database, or "" if none is set
std::string ReadRawCarIdentifier(uintptr_t baseModule);

// Translates an internal car model name (e.g. "SKYLINER35", "RX7", "CIVICSI") into a human-readable display name
std::string TranslateCarIdentifier(const std::string& rawIdentifier);

#endif // PROSTREETRPC_CAR_H
