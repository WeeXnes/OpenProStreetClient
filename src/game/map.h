#pragma once
#include <string>
#include <windows.h>

std::string ReadRawMapIdentifier(uintptr_t baseModule);
std::string TranslateMapIdentifier(const std::string& rawIdentifier);

// The name of the track whose identifier hashes to this key. Empty if it isn't a known track.
std::string MapNameForKey(uint32_t key);
