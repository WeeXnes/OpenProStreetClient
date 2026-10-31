#pragma once
#include <string>
#include <windows.h>

std::string ReadRawMapIdentifier(uintptr_t baseModule);
std::string TranslateMapIdentifier(const std::string& rawIdentifier);
