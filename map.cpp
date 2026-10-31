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
    // MassiveAds (0xB309C0) +0x05 holds the loaded event's track name. It is set while an event is
    // loading and stays set afterwards, so it only means "current track" during or just after a race.
    uintptr_t adsBase = baseModule + 0x7309C0;
    if (IsBadReadPtr((void*)adsBase, sizeof(uintptr_t))) {
        return "";
    }
    uintptr_t ads = *(uintptr_t*)adsBase;
    if (ads == 0) {
        return "";
    }
    return ReadStringFromMemory(ads + 0x4);
}
