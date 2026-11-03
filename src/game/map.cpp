#include "game/map.h"
#include "log.h"
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

// Attrib::StringHash32: Bob Jenkins' lookup2, which the game's collection keys use.
static void HashMix(uint32_t& a, uint32_t& b, uint32_t& c) {
    a -= b; a -= c; a ^= c >> 13;
    b -= c; b -= a; b ^= a << 8;
    c -= a; c -= b; c ^= b >> 13;
    a -= b; a -= c; a ^= c >> 12;
    b -= c; b -= a; b ^= a << 16;
    c -= a; c -= b; c ^= b >> 5;
    a -= b; a -= c; a ^= c >> 3;
    b -= c; b -= a; b ^= a << 10;
    c -= a; c -= b; c ^= b >> 15;
}

static uint32_t StringHash32(const std::string& text) {
    const uint8_t* k = reinterpret_cast<const uint8_t*>(text.data());
    size_t remaining = text.size();
    uint32_t a = 0x9E3779B9, b = 0x9E3779B9, c = 0xABCDEF00;
    const auto word = [](const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); };
    while (remaining >= 12) {
        a += word(k);
        b += word(k + 4);
        c += word(k + 8);
        HashMix(a, b, c);
        k += 12;
        remaining -= 12;
    }
    c += static_cast<uint32_t>(text.size());
    if (remaining >= 11) c += static_cast<uint32_t>(k[10]) << 24;
    if (remaining >= 10) c += static_cast<uint32_t>(k[9]) << 16;
    if (remaining >= 9) c += static_cast<uint32_t>(k[8]) << 8;
    if (remaining >= 8) b += static_cast<uint32_t>(k[7]) << 24;
    if (remaining >= 7) b += static_cast<uint32_t>(k[6]) << 16;
    if (remaining >= 6) b += static_cast<uint32_t>(k[5]) << 8;
    if (remaining >= 5) b += k[4];
    if (remaining >= 4) a += static_cast<uint32_t>(k[3]) << 24;
    if (remaining >= 3) a += static_cast<uint32_t>(k[2]) << 16;
    if (remaining >= 2) a += static_cast<uint32_t>(k[1]) << 8;
    if (remaining >= 1) a += k[0];
    HashMix(a, b, c);
    return c;
}

std::string MapNameForKey(uint32_t key) {
    for (const auto& entry : MAP_TRANSLATION_TABLE) {
        if (StringHash32("tracks/" + entry.first) == key) return entry.second;
    }
    return "";
}

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
        Log::Debug("[OpenProStreet] COULDNT FIND TRANSLATION FOR ID: %s\n", rawIdentifier.c_str());
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
