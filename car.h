#ifndef PROSTREETRPC_CAR_H
#define PROSTREETRPC_CAR_H

#include <windows.h>
#include <string>
#include <cstdint>

// Reads the raw car identifier string pointer from game memory
std::string ReadRawCarIdentifier(uintptr_t baseModule);

// Translates an internal car identifier (e.g. "GTR", "RX7", "CIVIC") into a human-readable display name
std::string TranslateCarIdentifier(const std::string& rawIdentifier);

#endif // PROSTREETRPC_CAR_H
