#include "library.h"

#include <windows.h>
#include <iostream>

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <unordered_map>

#include <discord.h>

discord::Core* discordCore = nullptr;

const std::string version = "1.5";


// Translation table for known internal map identifiers.
// To extend with newly discovered map identifiers, simply add new key-value pairs to this map.
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

// Helper function to format numbers with commas
std::string format_with_commas(long long value) {
    std::string s = std::to_string(value);
    int n = s.length() - 3;
    while (n > 0) {
        s.insert(n, ",");
        n -= 3;
    }
    return s;
}

// Validates if a string is a valid printable ASCII map identifier.
bool IsValidMapIdentifier(const std::string& str) {
    if (str.length() < 2 || str.length() > 64) {
        return false;
    }
    
    bool hasAlphaNum = false;
    for (char c : str) {
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || uc > 126) {
            return false; // Non-printable / uninitialized garbage byte
        }
        if (isalnum(uc)) {
            hasAlphaNum = true;
        }
    }
    return hasAlphaNum;
}

// Translates a raw map identifier using the translation table.
// If the identifier is not present in the translation table, returns the raw string as-is without modification or normalization.
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

// Safely reads a null-terminated C string from memory up to maxLen bytes, skipping any non-printable prefix bytes.
std::string ReadStringFromMemory(uintptr_t strPtr, size_t maxLen = 64) {
    if (!strPtr || IsBadReadPtr((void*)strPtr, 1)) {
        return "";
    }

    // Step 1: Find first printable alphanumeric byte within first 16 bytes (to skip unprintable prefix/control bytes)
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

    // Step 2: Read printable ASCII characters from startOffset until null terminator or non-printable byte
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
            break; // Stop at end of printable ASCII string segment
        }
        result.push_back(c);
    }

    if (!IsValidMapIdentifier(result)) {
        return "";
    }

    return result;
}

// Reads the raw map identifier from memory using primary and secondary (fallback) pointer chains.
// Primary pointer:   "nfs.exe" + 0x7309C0 -> +0x4
// Secondary pointer: "nfs.exe" + 0x7309B4 -> +0x54
std::string ReadRawMapIdentifier(uintptr_t baseModule) {
    // Try Primary Pointer: "nfs.exe" + 0x7309C0 -> +0x4
    uintptr_t primaryBase = baseModule + 0x7309C0;
    if (IsBadReadPtr((void*)primaryBase, sizeof(uintptr_t)) == 0) {
        uintptr_t ptr1 = *(uintptr_t*)primaryBase;
        if (ptr1 != 0) {
            // 1. Try direct string buffer at (ptr1 + 0x4)
            std::string rawStr = ReadStringFromMemory(ptr1 + 0x4);
            if (!rawStr.empty()) {
                return rawStr;
            }
            // 2. Try dereferenced char* pointer at *(ptr1 + 0x4)
            if (IsBadReadPtr((void*)(ptr1 + 0x4), sizeof(uintptr_t)) == 0) {
                uintptr_t strPtr = *(uintptr_t*)(ptr1 + 0x4);
                rawStr = ReadStringFromMemory(strPtr);
                if (!rawStr.empty()) {
                    return rawStr;
                }
            }
        }
    }

    // Secondary Pointer (fallback): "nfs.exe" + 0x7309B4 -> +0x54
    uintptr_t secondaryBase = baseModule + 0x7309B4;
    if (IsBadReadPtr((void*)secondaryBase, sizeof(uintptr_t)) == 0) {
        uintptr_t ptr2 = *(uintptr_t*)secondaryBase;
        if (ptr2 != 0) {
            // 1. Try direct string buffer at (ptr2 + 0x54)
            std::string rawStr = ReadStringFromMemory(ptr2 + 0x54);
            if (!rawStr.empty()) {
                return rawStr;
            }
            // 2. Try dereferenced char* pointer at *(ptr2 + 0x54)
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

DWORD WINAPI DiscordThread(LPVOID param) {
    printf("[ProStreetRPC] Discord thread started.\n");
    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);
    if (!baseModule) {
        printf("[ProStreetRPC] Failed to get base module.\n");
        return 1; // Cannot proceed
    }
    printf("[ProStreetRPC] Base module found at: 0x%p\n", (void*)baseModule);
    
    discord::Activity activity{};
    activity.SetType(discord::ActivityType::Playing);
    activity.SetApplicationId(656565390411038731);
    activity.GetAssets().SetLargeImage("gcrown");
    activity.GetAssets().SetLargeText(("ProStreetRPC v" + version + " by WeeXnes").c_str());
    activity.GetTimestamps().SetStart(time(nullptr));
    activity.SetName("Need for Speed: ProStreet");

    int loop_count = 0;

    while (true) {
        loop_count++;

        try {
            // Money pointer path: [[base + 0x6B0EA8]] + 0x1E0
            uintptr_t moneyAddr = 0;
            uintptr_t moneyBasePtr = baseModule + 0x6B0EA8;
            if (IsBadReadPtr((void*)moneyBasePtr, sizeof(uintptr_t)) == 0) {
                uintptr_t moneyPtrVal = *(uintptr_t*)moneyBasePtr;
                if (moneyPtrVal != 0 && IsBadReadPtr((void*)(moneyPtrVal + 0x1E0), sizeof(unsigned long long)) == 0) {
                     moneyAddr = moneyPtrVal + 0x1E0;
                }
            }
            
            // XP pointer path: [[[base + 0x6B9FB8] + 0x3C]] + 0x1C
            uintptr_t xpAddr = 0;
            uintptr_t xpBasePtr = baseModule + 0x6B9FB8;
            if (IsBadReadPtr((void*)xpBasePtr, sizeof(uintptr_t)) == 0) {
                uintptr_t xpPtrVal1 = *(uintptr_t*)xpBasePtr;
                if (xpPtrVal1 != 0 && IsBadReadPtr((void*)(xpPtrVal1 + 0x3C), sizeof(uintptr_t)) == 0) {
                    uintptr_t xpPtrVal2 = *(uintptr_t*)(xpPtrVal1 + 0x3C);
                    if (xpPtrVal2 != 0 && IsBadReadPtr((void*)(xpPtrVal2 + 0x1C), sizeof(unsigned long long)) == 0) {
                        xpAddr = xpPtrVal2 + 0x1C;
                    }
                }
            }

            // Map pointer path
            std::string rawMap = ReadRawMapIdentifier(baseModule);
            std::string displayMap = TranslateMapIdentifier(rawMap);
            bool hasMap = !displayMap.empty();

            // Print the current map, if it changed
            static std::string lastLoggedMap = "";
            if (displayMap != lastLoggedMap) {
                if (!displayMap.empty()) {
                    printf("[ProStreetRPC] Current map: '%s'\n", displayMap.c_str());
                } else {
                    printf("[ProStreetRPC] Map cleared.\n");
                }
                lastLoggedMap = displayMap;
            }

            unsigned long long money = 0;
            unsigned long long xp = 0;

            bool hasMoney = (moneyAddr != 0);
            bool hasXp = (xpAddr != 0);

            if (hasMoney) {
                money = *(unsigned long long*)moneyAddr;
            }

            if (hasXp) {
                xp = *(unsigned long long*)xpAddr;
            }

            if (hasMap) {
                activity.GetAssets().SetLargeText(displayMap.c_str());
            } else {
                activity.GetAssets().SetLargeText(("ProStreetRPC v" + version + " by WeeXnes").c_str());
            }

            if (hasMap || hasMoney || hasXp) {
                if (hasMoney) {
                    std::string formatted_balance = format_with_commas(money);
                    activity.SetState((formatted_balance + "$").c_str());
                } else {
                    activity.SetState("Money: N/A");
                }

                // Cycle between Map Name and Raceday XP every 5 seconds (10 loops * 500ms) on the first line (Details)
                std::string detailsText = "";
                if (hasMap && hasXp) {
                    int cycle = (loop_count / 10) % 2;
                    if (cycle == 0) {
                        detailsText = displayMap;
                    } else {
                        detailsText = "Raceday: " + format_with_commas(xp) + " XP";
                    }
                } else if (hasMap) {
                    detailsText = displayMap;
                } else if (hasXp) {
                    detailsText = "Raceday: " + format_with_commas(xp) + " XP";
                } else {
                    detailsText = "Navigating";
                }
                activity.SetDetails(detailsText.c_str());

            } else {
                activity.SetState("In Menus");
                activity.SetDetails("Navigating");
            }

            if (discordCore) {
                discordCore->ActivityManager().UpdateActivity(activity, [](discord::Result res) {});
                discordCore->RunCallbacks();
            }

        } catch (...) {
            printf("[ProStreetRPC] Exception caught in Discord thread.\n");
            activity.SetState("In Menus");
            activity.SetDetails("Error reading data");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        printf("[ProStreetRPC] ASI attached. (https://code.weexnes.dev/ProStreetRPC)\n");
        char procname[MAX_PATH];
        GetModuleFileNameA(NULL, procname, MAX_PATH);
        if (strstr(procname, "nfs.exe") == NULL) {
            printf("[ProStreetRPC] Not nfs.exe, exiting.\n");
            return TRUE;
        }

        printf("[ProStreetRPC] nfs.exe detected.\n");

        DisableThreadLibraryCalls(hModule);

        discord::Result result = discord::Core::Create(656565390411038731, DiscordCreateFlags_NoRequireDiscord, &discordCore);
        if (result != discord::Result::Ok) {
            printf("[ProStreetRPC] Failed to create Discord core: %d\n", (int)result);
            return FALSE;
        }
        printf("[ProStreetRPC] Discord core created successfully.\n");

        HANDLE hThread = CreateThread(NULL, 0, DiscordThread, NULL, 0, NULL);
        if (hThread) {
            printf("[ProStreetRPC] Discord thread created successfully.\n");
            CloseHandle(hThread);
        } else {
            printf("[ProStreetRPC] Failed to create Discord thread.\n");
        }
    }

    return TRUE;
}