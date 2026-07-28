#include "map.h"
#include <unordered_map>
#include <cstdio>

static const std::unordered_map<std::string, std::string> MAP_TRANSLATION_TABLE = {
    {"portland", "Portland International Raceway"},
    {"willow_springs", "Willow Springs International Raceway"},
    {"chicago_gd", "Chicago Airfield"},
    {"leipzig", "Leipzig Test Track"},
    {"ebisu", "Ebisu Circuit"},
    {"mondello_park", "Mondello Park"},
    {"autopolis", "Autopolis"},
    {"texas_speedway", "Texas World Speedway"},
    {"shuto_gd", "Tokyo Docklands"},
    {"autobahn_gd", "Autobahnring"},
    {"nevada_gd", "Nevada Highway"},
    {"infineon", "Infineon Raceway"}
};

bool IsValidMapIdentifier(const std::string& str) {
    if (str.length() < 2 || str.length() > 64) {
        return false;
    }

    bool hasAlphaNum = false;
    for (char c : str) {
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || uc > 126) {
            return false;
        }
        if (isalnum(uc)) {
            hasAlphaNum = true;
        }
    }
    return hasAlphaNum;
}

std::string TranslateMapIdentifier(const std::string& rawIdentifier) {
    if (rawIdentifier.empty()) {
        return "";
    }

    auto it = MAP_TRANSLATION_TABLE.find(rawIdentifier);
    if (it != MAP_TRANSLATION_TABLE.end()) {
        return it->second;
    }

    static std::string lastMissingId = "";
    if (rawIdentifier != lastMissingId) {
        printf("[ProStreetRPC] COULDNT FIND TRANSLATION FOR ID: %s\n", rawIdentifier.c_str());
        lastMissingId = rawIdentifier;
    }

    return rawIdentifier;
}

std::string ReadStringFromMemory(uintptr_t strPtr, size_t maxLen = 64) {
    if (!strPtr || IsBadReadPtr((void*)strPtr, 1)) {
        return "";
    }

    size_t startOffset = 0;
    bool foundStart = false;
    for (size_t i = 0; i < 16; ++i) {
        if (IsBadReadPtr((void*)(strPtr + i), 1)) {
            break;
        }
        char c = *(char*)(strPtr + i);
        if (c == '\0') {
            break;
        }
        unsigned char uc = (unsigned char)c;
        if (isalnum(uc)) {
            startOffset = i;
            foundStart = true;
            break;
        }
    }

    if (!foundStart) {
        return "";
    }

    std::string result;
    result.reserve(32);
    for (size_t i = startOffset; i < maxLen; ++i) {
        if (IsBadReadPtr((void*)(strPtr + i), 1)) {
            break;
        }
        char c = *(char*)(strPtr + i);
        if (c == '\0') {
            break;
        }
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || uc > 126) {
            break;
        }
        result.push_back(c);
    }

    if (!IsValidMapIdentifier(result)) {
        return "";
    }

    return result;
}

std::string ReadRawMapIdentifier(uintptr_t baseModule) {
    uintptr_t primaryBase = baseModule + 0x7309C0;
    if (IsBadReadPtr((void*)primaryBase, sizeof(uintptr_t)) == 0) {
        uintptr_t ptr1 = *(uintptr_t*)primaryBase;
        if (ptr1 != 0) {
            std::string rawStr = ReadStringFromMemory(ptr1 + 0x4);
            if (!rawStr.empty()) {
                return rawStr;
            }

            if (IsBadReadPtr((void*)(ptr1 + 0x4), sizeof(uintptr_t)) == 0) {
                uintptr_t strPtr = *(uintptr_t*)(ptr1 + 0x4);
                rawStr = ReadStringFromMemory(strPtr);
                if (!rawStr.empty()) {
                    return rawStr;
                }
            }
        }
    }

    uintptr_t secondaryBase = baseModule + 0x7309B4;
    if (IsBadReadPtr((void*)secondaryBase, sizeof(uintptr_t)) == 0) {
        uintptr_t ptr2 = *(uintptr_t*)secondaryBase;
        if (ptr2 != 0) {
            std::string rawStr = ReadStringFromMemory(ptr2 + 0x54);
            if (!rawStr.empty()) {
                return rawStr;
            }

            if (IsBadReadPtr((void*)(ptr2 + 0x54), sizeof(uintptr_t)) == 0) {
                uintptr_t strPtr = *(uintptr_t*)(ptr2 + 0x54);
                rawStr = ReadStringFromMemory(strPtr);
                if (!rawStr.empty()) {
                    return rawStr;
                }
            }
        }
    }

    return "";
}
