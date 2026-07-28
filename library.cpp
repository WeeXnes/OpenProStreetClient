#include "library.h"
#include <discord.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

discord::Core* discordCore = nullptr;

const std::string version = "2.3";

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

std::string format_with_commas(long long value) {
    std::string s = std::to_string(value);
    int n = s.length() - 3;
    while (n > 0) {
        s.insert(n, ",");
        n -= 3;
    }
    return s;
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

DWORD WINAPI DiscordThread(LPVOID param) {
    printf("[ProStreetRPC] Discord thread started.\n");
    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);
    if (!baseModule) {
        printf("[ProStreetRPC] Failed to get base module.\n");
        return 1;
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

            uintptr_t moneyAddr = 0;
            uintptr_t moneyBasePtr = baseModule + 0x6B0EA8;
            if (IsBadReadPtr((void*)moneyBasePtr, sizeof(uintptr_t)) == 0) {
                uintptr_t moneyPtrVal = *(uintptr_t*)moneyBasePtr;
                if (moneyPtrVal != 0 && IsBadReadPtr((void*)(moneyPtrVal + 0x1E0), sizeof(unsigned long long)) == 0) {
                     moneyAddr = moneyPtrVal + 0x1E0;
                }
            }

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

            std::string rawMap = ReadRawMapIdentifier(baseModule);
            std::string displayMap = TranslateMapIdentifier(rawMap);
            bool hasMap = !displayMap.empty();

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

namespace LANFix
{
    uintptr_t ptrDataConnection = 0x827190;
    uint32_t AddrFamily = 2;

    uint32_t CheckAddressFamily(const char* addr)
    {
        if (!addr || IsBadReadPtr((void*)addr, 1)) {
            return 2;
        }
        try {
            std::string strAddr(addr);
            if (std::regex_match(strAddr, std::regex(R"(([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|:((:[0-9a-fA-F]{1,4}){1,7}|:)|fe80:(:[0-9a-fA-F]{0,4}){0,4}%[0-9a-zA-Z]{1,}|::(ffff(:0{1,4}){0,1}:){0,1}((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.){3,3}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])|([0-9a-fA-F]{1,4}:){1,4}:((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.){3,3}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9]))")))
            {
                return 23;
            }
        } catch (...) {

        }

        return 2;
    }

    bool __attribute__((thiscall)) DataConnectionHook(void* that, char* addr, void* unk)
    {

        if (!addr || addr[0] == '\0' || strcmp(addr, "0.0.0.0") == 0 || strcmp(addr, "localhost") == 0 || strcmp(addr, "127.0.0.1") == 0) {
            addr = (char*)"weexnes.dev";
        }
        AddrFamily = CheckAddressFamily(addr);
        printf("[ProStreetRPC] LANFix DataConnectionHook: addr='%s' -> AddrFamily=%u (%s)\n", addr ? addr : "NULL", AddrFamily, AddrFamily == 23 ? "AF_INET6 (IPv6)" : "AF_INET (IPv4)");
        return reinterpret_cast<bool(__attribute__((thiscall))*)(void*, char*, void*)>(ptrDataConnection)(that, addr, unk);
    }

    uintptr_t FindPattern(uintptr_t startAddr, size_t searchSize, const std::string& pattern)
    {
        std::vector<int> bytes;
        for (size_t i = 0; i < pattern.length(); ) {
            if (pattern[i] == ' ') { i++; continue; }
            if (pattern[i] == '?') {
                bytes.push_back(-1);
                i += (i + 1 < pattern.length() && pattern[i + 1] == '?') ? 2 : 1;
            } else {
                bytes.push_back(std::stoi(pattern.substr(i, 2), nullptr, 16));
                i += 2;
            }
        }

        uintptr_t currAddr = startAddr;
        uintptr_t endAddr = startAddr + searchSize;
        MEMORY_BASIC_INFORMATION mbi;

        while (currAddr < endAddr && VirtualQuery((void*)currAddr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE))) {
                uint8_t* p = (uint8_t*)currAddr;
                size_t regionSize = mbi.RegionSize - (currAddr - (uintptr_t)mbi.BaseAddress);
                if (currAddr + regionSize > endAddr) regionSize = endAddr - currAddr;

                for (size_t i = 0; i + bytes.size() <= regionSize; ++i) {
                    bool found = true;
                    for (size_t j = 0; j < bytes.size(); ++j) {
                        if (bytes[j] != -1 && p[i + j] != (uint8_t)bytes[j]) {
                            found = false;
                            break;
                        }
                    }
                    if (found) return (uintptr_t)(p + i);
                }
            }
            currAddr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
        return 0;
    }

    void PatchCall(uintptr_t callAddr, uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)callAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(uint8_t*)callAddr = 0xE8;
        *(int32_t*)(callAddr + 1) = (int32_t)(targetAddr - (callAddr + 5));
        VirtualProtect((void*)callAddr, 5, oldProtect, &oldProtect);
    }

    void InstallHook1(uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)targetAddr, 12, PAGE_EXECUTE_READWRITE, &oldProtect);

        uint8_t* p = (uint8_t*)targetAddr;
        p[0] = 0xA1;
        *(uint32_t*)(p + 1) = (uint32_t)&AddrFamily;
        p[5] = 0x89;
        p[6] = 0x44;
        p[7] = 0x24;
        p[8] = 0x10;
        p[9] = 0x90;
        p[10] = 0x90;
        p[11] = 0x90;

        VirtualProtect((void*)targetAddr, 12, oldProtect, &oldProtect);
    }

    void InstallHook2(uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)targetAddr, 40, PAGE_EXECUTE_READWRITE, &oldProtect);

        uint8_t code[40] = {
            0x50,
            0x33, 0xC0,
            0x89, 0x44, 0x24, 0x14,
            0x89, 0x44, 0x24, 0x24,
            0x89, 0x44, 0x24, 0x28,
            0x89, 0x44, 0x24, 0x2C,
            0x89, 0x44, 0x24, 0x30,
            0x6A, 0x02,
            0x58,
            0x89, 0x44, 0x24, 0x1C,
            0x6A, 0x11,
            0x58,
            0x89, 0x44, 0x24, 0x20,
            0x58,
            0x90, 0x90
        };
        memcpy((void*)targetAddr, code, sizeof(code));

        VirtualProtect((void*)targetAddr, 40, oldProtect, &oldProtect);
    }

    void Init()
    {
        uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);
        if (!baseModule) return;

        printf("[ProStreetRPC] Initializing Win11 LAN Fix...\n");

        ptrDataConnection = FindPattern(baseModule, 0x800000, "83 EC 20 33 C0 56 6A 11 6A 02 89 44 24 10");
        if (!ptrDataConnection) {
            ptrDataConnection = baseModule + 0x427190;
        }

        uintptr_t loc_823C54 = FindPattern(baseModule, 0x800000, "C7 44 24 1C FF FF FF FF 89 46 04 E8 ? ? ? ? 84 C0 0F 84");
        if (loc_823C54) {
            loc_823C54 += 0xB;
        } else {
            loc_823C54 = baseModule + 0x423C54;
        }

        uintptr_t loc_82719A = ptrDataConnection + 0xA;
        uintptr_t loc_8271A9 = ptrDataConnection + 0x19;

        InstallHook1(loc_82719A);
        InstallHook2(loc_8271A9);
        PatchCall(loc_823C54, (uintptr_t)DataConnectionHook);

        printf("[ProStreetRPC] Win11 LAN Fix applied successfully! (ptrDataConnection: 0x%p, hook Call: 0x%p)\n", (void*)ptrDataConnection, (void*)loc_823C54);
        printf("-------------------------------------------------------------------\n");
        printf("[ProStreetRPC] LAN NOTICE: Leave the address bar empty in the\n");
        printf("[ProStreetRPC] LAN game menu to automatically connect to weexnes.dev!\n");
        printf("-------------------------------------------------------------------\n");
    }
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

        LANFix::Init();

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
