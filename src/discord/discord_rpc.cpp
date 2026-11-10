#include "discord/discord_rpc.h"
#include "game/map.h"
#include "game/overlay.h"
#include "game/car.h"
#include "game/race_mode.h"
#include "game/race_results.h"
#include "utils.h"
#include "log.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>

DWORD WINAPI RpcThread(LPVOID param) {
    Log::Debug("[OpenProStreet] RPC thread started.\n");
    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);
    if (!baseModule) {
        Log::Error("Failed to get base module.\n");
        return 1;
    }
    Log::Debug("[OpenProStreet] Base module found at: 0x%p\n", (void*)baseModule);

    discord::Activity activity{};
    activity.SetType(discord::ActivityType::Playing);
    activity.SetApplicationId(656565390411038731);
    activity.GetAssets().SetLargeImage("gcrown");
    activity.GetAssets().SetLargeText(("OpenProStreet v" + version + " by WeeXnes").c_str());
    activity.GetTimestamps().SetStart(time(nullptr));
    activity.SetName("Need for Speed: ProStreet");

    int loop_count = 0;

    while (true) {
        loop_count++;

        try {
            // Lazy / Safe Discord connection (retries every 5s if Discord wasn't open on boot)
            if (!discordCore) {
                static time_t lastAttempt = 0;
                time_t now = time(nullptr);
                if (now - lastAttempt >= 5) {
                    lastAttempt = now;
                    discord::Result res = discord::Core::Create(656565390411038731, DiscordCreateFlags_NoRequireDiscord, &discordCore);
                    if (res == discord::Result::Ok) {
                        Log::Event(Log::Tag::Discord, "Discord core connected successfully.\n");
                    }
                }
            }

            // Read Money
            uintptr_t moneyAddr = 0;
            uintptr_t moneyBasePtr = baseModule + 0x6B0EA8;
            if (IsBadReadPtr((void*)moneyBasePtr, sizeof(uintptr_t)) == 0) {
                uintptr_t moneyPtrVal = *(uintptr_t*)moneyBasePtr;
                if (moneyPtrVal != 0 && IsBadReadPtr((void*)(moneyPtrVal + 0x1E0), sizeof(unsigned long long)) == 0) {
                     moneyAddr = moneyPtrVal + 0x1E0;
                }
            }

            // Read Raceday XP
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

            bool raceActive = IsRaceActive(baseModule);

            // Read Map
            std::string rawMap = raceActive ? ReadRawMapIdentifier(baseModule) : "";
            std::string displayMap = TranslateMapIdentifier(rawMap);
            bool hasMap = !displayMap.empty();

            static std::string lastLoggedMap = "";
            if (displayMap != lastLoggedMap) {
                if (!displayMap.empty()) {
                    Log::Debug("[OpenProStreet] Current map: '%s'\n", displayMap.c_str());
                    Log::Summary(Log::Tag::Game, "track: %s\n", Log::Highlight(displayMap).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Map cleared.\n");
                }
                lastLoggedMap = displayMap;
            }

            // Read Car
            std::string rawCar = IsCurrentCarConfirmed(baseModule) ? ReadRawCarIdentifier(baseModule) : "";
            std::string displayCar = TranslateCarIdentifier(rawCar);
            bool hasCar = !displayCar.empty();

            static std::string lastLoggedCar = "";
            if (displayCar != lastLoggedCar) {
                if (!displayCar.empty()) {
                    Log::Debug("[OpenProStreet] Current car: '%s' (raw: '%s')\n", displayCar.c_str(), rawCar.c_str());
                    Log::Summary(Log::Tag::Game, "car: %s\n", Log::Highlight(displayCar).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Car cleared.\n");
                }
                lastLoggedCar = displayCar;
            }

            // Read Race Mode
            std::string raceMode = raceActive ? ReadRaceMode(baseModule) : "";
            bool hasRaceMode = !raceMode.empty();

            static std::string lastLoggedMode = "";
            if (raceMode != lastLoggedMode) {
                if (!raceMode.empty()) {
                    Log::Debug("[OpenProStreet] Current race mode: '%s'\n", raceMode.c_str());
                    Log::Summary(Log::Tag::Game, "race type: %s\n", Log::Highlight(raceMode).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Race mode cleared.\n");
                }
                lastLoggedMode = raceMode;
            }

            // The overlay shows where the player stands on this track and mode.
            Overlay::OnGameState(raceActive, rawMap, displayMap, raceMode);

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

            // Update Large Image Hover Text
            if (hasMap) {
                if (hasRaceMode) {
                    activity.GetAssets().SetLargeText((displayMap + " - " + raceMode).c_str());
                } else {
                    activity.GetAssets().SetLargeText(displayMap.c_str());
                }
            } else {
                activity.GetAssets().SetLargeText(("OpenProStreet v" + version + " by WeeXnes").c_str());
            }

            // Build dynamic details cycle list
            std::vector<std::string> detailsCycle;

            if (hasMap) {
                detailsCycle.push_back(displayMap);
            }
            if (hasRaceMode) {
                detailsCycle.push_back("Event: " + raceMode);
            }
            if (hasCar) {
                detailsCycle.push_back("Car: " + displayCar);
            }
            if (hasXp) {
                detailsCycle.push_back("Raceday: " + format_with_commas(xp) + " XP");
            }

            if (!detailsCycle.empty()) {
                // Cycle every 4 seconds (8 loops of 500ms)
                size_t cycleIndex = (loop_count / 8) % detailsCycle.size();
                activity.SetDetails(detailsCycle[cycleIndex].c_str());

                if (hasMoney) {
                    std::string formatted_balance = format_with_commas(money);
                    activity.SetState((formatted_balance + "$").c_str());
                } else {
                    activity.SetState("In Career");
                }
            } else {
                if (hasMoney) {
                    std::string formatted_balance = format_with_commas(money);
                    activity.SetState((formatted_balance + "$").c_str());
                    activity.SetDetails("In Menus");
                } else {
                    activity.SetState("In Menus");
                    activity.SetDetails("Navigating");
                }
            }

            if (discordCore) {
                discordCore->ActivityManager().UpdateActivity(activity, [](discord::Result res) {});
                discordCore->RunCallbacks();
            }

        } catch (...) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                Log::Warn("Exception caught in Discord thread.\n");
            } else {
                Log::Debug("[OpenProStreet] Exception caught in Discord thread.\n");
            }
            activity.SetState("In Menus");
            activity.SetDetails("Error reading data");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}
