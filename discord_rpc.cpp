#include "discord_rpc.h"
#include "map.h"
#include "utils.h"
#include <cstdio>
#include <thread>
#include <chrono>

DWORD WINAPI RpcThread(LPVOID param) {
    printf("[ProStreetRPC] RPC thread started.\n");
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
