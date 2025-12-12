#include "library.h"

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <discord.h>

discord::Core* discordCore = nullptr;

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
    activity.GetAssets().SetLargeText("NFS ProStreet v1.1");
    activity.GetTimestamps().SetStart(time(nullptr));
    activity.SetName("Need for Speed: ProStreet");

    int loop_count = 0;

    while (true) {
        if (loop_count % 20 == 0) { // Log every 10 seconds (20 * 500ms)
            printf("[ProStreetRPC] Discord thread is running.\n");
        }
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

            if (hasMoney || hasXp) {
                if (hasMoney) {
                    std::string formatted_balance = format_with_commas(money);
                    activity.SetState((formatted_balance + "$").c_str());
                } else {
                    activity.SetState("Money: N/A");
                }

                if (hasXp) {
                    std::string formatted_xp = format_with_commas(xp);
                    activity.SetDetails(("Raceday: " + formatted_xp + " XP").c_str());
                } else {
                    activity.SetDetails("Raceday: N/A XP");
                }

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