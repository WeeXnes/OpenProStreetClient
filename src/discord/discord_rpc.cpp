#include "discord/discord_rpc.h"
#include "game/atmosphere.h"
#include "game/car.h"
#include "game/event_vote.h"
#include "game/map.h"
#include "game/overlay.h"
#include "game/quick_match.h"
#include "game/race_mode.h"
#include "game/race_results.h"
#include "net/server_link.h"
#include "log.h"
#include "utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace
{
    const discord::ClientId DISCORD_APP_ID = 656565390411038731;
    const int UPDATE_MIN_INTERVAL_SEC = 4;

    struct ActivitySnapshot
    {
        std::string details;
        std::string state;
        std::string largeText;
        std::string partyId;
        std::string joinSecret;
        int32_t     partySize = 0;
        int32_t     partyMax = 0;
        int64_t     startTs = 0;
        int64_t     endTs = 0;
        bool        inRace = false;
        bool        inRoom = false;
        bool        online = false;

        bool ContentDiffers(const ActivitySnapshot& o) const
        {
            return details != o.details || state != o.state || largeText != o.largeText ||
                   partyId != o.partyId || joinSecret != o.joinSecret ||
                   partySize != o.partySize || partyMax != o.partyMax ||
                   startTs != o.startTs || endTs != o.endTs;
        }

        bool MajorTransition(const ActivitySnapshot& o) const
        {
            return inRace != o.inRace || inRoom != o.inRoom || online != o.online;
        }
    };

    std::string Truncate(const std::string& text, size_t maxLen)
    {
        if (text.size() <= maxLen) return text;
        if (maxLen <= 3) return text.substr(0, maxLen);
        return text.substr(0, maxLen - 3) + "...";
    }

    std::string Ordinal(int rank)
    {
        if (rank <= 0) return "";
        const int mod100 = rank % 100;
        const char* suffix = "th";
        if (mod100 < 11 || mod100 > 13) {
            if (rank % 10 == 1) suffix = "st";
            else if (rank % 10 == 2) suffix = "nd";
            else if (rank % 10 == 3) suffix = "rd";
        }
        return std::to_string(rank) + suffix;
    }

    std::string FormatTimeShort(float sec)
    {
        if (sec <= 0.0f) return "";
        const int totalTenths = static_cast<int>(sec * 10.0f + 0.5f);
        const int mins = totalTenths / 600;
        const int secs = (totalTenths / 10) % 60;
        const int tenths = totalTenths % 10;
        char buf[32];
        if (mins > 0) snprintf(buf, sizeof(buf), "%d:%02d.%d", mins, secs, tenths);
        else snprintf(buf, sizeof(buf), "%d.%ds", secs, tenths);
        return buf;
    }

    std::string Capitalize(const std::string& s)
    {
        if (s.empty()) return "";
        std::string out = s;
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - ('a' - 'A'));
        return out;
    }

    std::string CurrentAtmosphereName(const Overlay::LobbySnapshot& lobby)
    {
        if (!lobby.atmosphere.empty()) return Capitalize(lobby.atmosphere);
        if (!Atmosphere::IsEnabled()) return "Day";
        switch (Atmosphere::Current()) {
            case Atmosphere::Dusk: return "Dusk";
            case Atmosphere::Night: return "Night";
            default: return "Day";
        }
    }

    std::string PauseReasonLabel(const std::string& reason, const std::string& who)
    {
        if (reason == "vote") return "Vote in progress";
        if (reason == "car") return who.empty() ? "Picking a car" : ("Waiting for " + who);
        if (reason == "wait") return "Waiting for players";
        if (reason == "ready") return "Waiting for ready";
        return "Countdown paused";
    }

    std::string BuildRaceState(const std::string& raceMode, const std::string& car,
                               const std::vector<RaceResults::LiveRacer>& racers,
                               const RaceResults::LivePace& pace,
                               bool online, bool hasXp, unsigned long long xp)
    {
        const RaceResults::LiveRacer* local = nullptr;
        for (const auto& r : racers) {
            if (r.isLocal) { local = &r; break; }
        }

        std::vector<std::string> parts;
        if (local && local->rank > 0) {
            std::string pos = Ordinal(local->rank) + " / " + std::to_string(racers.size());
            if (local->finished) pos = "Finished " + Ordinal(local->rank);
            parts.push_back(pos);
        }

        const bool isDrift = raceMode.find("Drift") != std::string::npos;
        const float score = (local && local->score > 0.0f) ? local->score : pace.score;
        const float bestLap = (local && local->bestLap > 0.0f) ? local->bestLap : pace.bestLap;
        const int laps = local ? local->lapsCompleted : pace.lapsCompleted;

        if (isDrift && score > 0.0f) {
            parts.push_back(format_with_commas(static_cast<unsigned long long>(score + 0.5f)) + " pts");
        } else if (bestLap > 0.0f) {
            parts.push_back("Best " + FormatTimeShort(bestLap));
        } else if (laps > 0) {
            parts.push_back("Lap " + std::to_string(laps + 1));
        } else if (!car.empty()) {
            parts.push_back(car);
        } else if (!online && hasXp && xp > 0) {
            parts.push_back(format_with_commas(xp) + " XP");
        }

        if (parts.empty()) return !car.empty() ? ("Racing · " + car) : (online ? "Racing online" : "In Race");
        std::string out = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) out += " · " + parts[i];
        return out;
    }

    static std::string BuildLaunchCommand(HMODULE clientModule)
    {
        char asiPath[MAX_PATH] = {};
        if (clientModule && GetModuleFileNameA(clientModule, asiPath, MAX_PATH) > 0) {
            char sysDir[MAX_PATH] = {};
            UINT sysLen = GetSystemWow64DirectoryA(sysDir, MAX_PATH);
            if (sysLen == 0 || sysLen >= MAX_PATH) sysLen = GetSystemDirectoryA(sysDir, MAX_PATH);
            const std::string rundll = (sysLen > 0 && sysLen < MAX_PATH)
                ? (std::string(sysDir) + "\\rundll32.exe")
                : "rundll32.exe";
            return "\"" + rundll + "\" \"" + std::string(asiPath) + "\",LaunchGame";
        }
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
            std::string cmd(exePath);
            if (cmd.find(' ') != std::string::npos && cmd.front() != '"') cmd = "\"" + cmd + "\"";
            return cmd;
        }
        return "";
    }

    static void AttachDiscordCallbacks(discord::Core* core, HMODULE clientModule)
    {
        if (!core) return;
        const std::string cmd = BuildLaunchCommand(clientModule);
        if (!cmd.empty()) {
            const discord::Result regRes = core->ActivityManager().RegisterCommand(cmd.c_str());
            Log::Debug("[OpenProStreet] Discord RegisterCommand(%s) -> %d\n", cmd.c_str(), static_cast<int>(regRes));
        }

        core->ActivityManager().OnActivityJoin.Connect([](const char* secret) {
            if (!secret || !secret[0]) return;
            Log::Event(Log::Tag::Discord, "Discord OnActivityJoin triggered.\n");
            QuickMatch::RequestInviteJoin(secret);
        });

        core->ActivityManager().OnActivityJoinRequest.Connect([](const discord::User& user) {
            const std::string name = user.GetUsername() ? user.GetUsername() : "Someone";
            Log::Event(Log::Tag::Discord, "Discord join request from %s.\n", Log::Highlight(name).c_str());
            Overlay::PushJoinRequest(static_cast<int64_t>(user.GetId()), name);
        });

        core->ActivityManager().OnActivityInvite.Connect([](discord::ActivityActionType, const discord::User& user, const discord::Activity&) {
            const std::string name = user.GetUsername() ? user.GetUsername() : "Someone";
            Log::Event(Log::Tag::Discord, "Discord game invite received from %s.\n", Log::Highlight(name).c_str());
            Overlay::PushNotice("best", name + " invited you",
                                std::vector<std::string>{"Accept the invite in Discord to join their Race Day."});
        });
    }

    uintptr_t ResolveMoneyAddr(uintptr_t baseModule)
    {
        uintptr_t basePtr = baseModule + 0x6B0EA8;
        if (IsBadReadPtr(reinterpret_cast<void*>(basePtr), sizeof(uintptr_t)) != 0) return 0;
        uintptr_t ptrVal = *reinterpret_cast<uintptr_t*>(basePtr);
        if (ptrVal == 0 || IsBadReadPtr(reinterpret_cast<void*>(ptrVal + 0x1E0), sizeof(unsigned long long)) != 0) return 0;
        return ptrVal + 0x1E0;
    }

    uintptr_t ResolveXpAddr(uintptr_t baseModule)
    {
        uintptr_t basePtr = baseModule + 0x6B9FB8;
        if (IsBadReadPtr(reinterpret_cast<void*>(basePtr), sizeof(uintptr_t)) != 0) return 0;
        uintptr_t ptr1 = *reinterpret_cast<uintptr_t*>(basePtr);
        if (ptr1 == 0 || IsBadReadPtr(reinterpret_cast<void*>(ptr1 + 0x3C), sizeof(uintptr_t)) != 0) return 0;
        uintptr_t ptr2 = *reinterpret_cast<uintptr_t*>(ptr1 + 0x3C);
        if (ptr2 == 0 || IsBadReadPtr(reinterpret_cast<void*>(ptr2 + 0x1C), sizeof(unsigned long long)) != 0) return 0;
        return ptr2 + 0x1C;
    }
}

DWORD WINAPI RpcThread(LPVOID param) {
    HMODULE clientModule = static_cast<HMODULE>(param);
    Log::Debug("[OpenProStreet] RPC thread started.\n");
    uintptr_t baseModule = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    if (!baseModule) {
        Log::Error("Failed to get base module.\n");
        return 1;
    }
    Log::Debug("[OpenProStreet] Base module found at: 0x%p\n", reinterpret_cast<void*>(baseModule));

    const int64_t launchEpoch = static_cast<int64_t>(time(nullptr));
    int64_t raceStartedEpoch = 0;
    int64_t lobbyEnteredEpoch = 0;
    bool wasRacing = false;
    bool wasInRoom = false;

    ActivitySnapshot lastSent;
    bool hasSentOnce = false;
    time_t lastUpdateAt = 0;
    std::string lastLoggedMap, lastLoggedCar, lastLoggedMode;

    while (true) {
        try {
            if (!discordCore) {
                static time_t lastAttempt = 0;
                time_t now = time(nullptr);
                if (now - lastAttempt >= 5) {
                    lastAttempt = now;
                    discord::Result res = discord::Core::Create(DISCORD_APP_ID, DiscordCreateFlags_NoRequireDiscord, &discordCore);
                    if (res == discord::Result::Ok) {
                        Log::Event(Log::Tag::Discord, "Discord core connected successfully.\n");
                        AttachDiscordCallbacks(discordCore, clientModule);
                        hasSentOnce = false;
                    }
                }
            }

            const uintptr_t moneyAddr = ResolveMoneyAddr(baseModule);
            const uintptr_t xpAddr = ResolveXpAddr(baseModule);
            const bool hasMoney = (moneyAddr != 0);
            const bool hasXp = (xpAddr != 0);
            const unsigned long long money = hasMoney ? *reinterpret_cast<unsigned long long*>(moneyAddr) : 0;
            const unsigned long long xp = hasXp ? *reinterpret_cast<unsigned long long*>(xpAddr) : 0;

            const bool raceActive = IsRaceActive(baseModule);
            const std::string rawMap = raceActive ? ReadRawMapIdentifier(baseModule) : "";
            const std::string displayMap = TranslateMapIdentifier(rawMap);
            const bool hasMap = !displayMap.empty();

            if (displayMap != lastLoggedMap) {
                if (!displayMap.empty()) {
                    Log::Debug("[OpenProStreet] Current map: '%s'\n", displayMap.c_str());
                    Log::Summary(Log::Tag::Game, "track: %s\n", Log::Highlight(displayMap).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Map cleared.\n");
                }
                lastLoggedMap = displayMap;
            }

            const std::string rawCar = IsCurrentCarConfirmed(baseModule) ? ReadRawCarIdentifier(baseModule) : "";
            const std::string displayCar = TranslateCarIdentifier(rawCar);
            const bool hasCar = !displayCar.empty();

            if (displayCar != lastLoggedCar) {
                if (!displayCar.empty()) {
                    Log::Debug("[OpenProStreet] Current car: '%s' (raw: '%s')\n", displayCar.c_str(), rawCar.c_str());
                    Log::Summary(Log::Tag::Game, "car: %s\n", Log::Highlight(displayCar).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Car cleared.\n");
                }
                lastLoggedCar = displayCar;
            }

            const std::string raceMode = raceActive ? ReadRaceMode(baseModule) : "";
            const bool hasRaceMode = !raceMode.empty();

            if (raceMode != lastLoggedMode) {
                if (!raceMode.empty()) {
                    Log::Debug("[OpenProStreet] Current race mode: '%s'\n", raceMode.c_str());
                    Log::Summary(Log::Tag::Game, "race type: %s\n", Log::Highlight(raceMode).c_str());
                } else {
                    Log::Debug("[OpenProStreet] Race mode cleared.\n");
                }
                lastLoggedMode = raceMode;
            }

            // Keep the overlay synchronized with current game state.
            Overlay::OnGameState(raceActive, rawMap, displayMap, raceMode);

            const bool online = ServerLink::IsConnected();
            const Overlay::LobbySnapshot lobby = online ? Overlay::GetLobbySnapshot() : Overlay::LobbySnapshot();
            const int pingMs = online ? ServerLink::GetPingMs() : -1;
            const std::string atmName = CurrentAtmosphereName(lobby);
            const int64_t nowEpoch = static_cast<int64_t>(time(nullptr));

            if (raceActive && !wasRacing) raceStartedEpoch = nowEpoch;
            if (!raceActive) raceStartedEpoch = 0;
            wasRacing = raceActive;

            if (lobby.inRoom && !wasInRoom) lobbyEnteredEpoch = nowEpoch;
            if (!lobby.inRoom) lobbyEnteredEpoch = 0;
            wasInRoom = lobby.inRoom;

            ActivitySnapshot next;
            next.inRace = raceActive && hasMap;
            next.inRoom = online && lobby.inRoom;
            next.online = online;

            if (next.inRace) {
                next.details = displayMap + (hasRaceMode ? (" · " + raceMode) : "");
                const std::vector<RaceResults::LiveRacer> racers = RaceResults::GetLiveRacers();
                const RaceResults::LivePace pace = RaceResults::GetLivePace();
                next.state = BuildRaceState(raceMode, displayCar, racers, pace, online, hasXp, xp);
                next.startTs = raceStartedEpoch > 0 ? raceStartedEpoch : launchEpoch;

                std::string tip = hasCar ? displayCar : displayMap;
                tip += " · " + atmName;
                if (online && pingMs >= 0) tip += " · " + std::to_string(pingMs) + " ms";
                else if (!online && hasXp && xp > 0) tip += " · " + format_with_commas(xp) + " XP";
                next.largeText = tip;

                if (online && lobby.inRoom && !lobby.players.empty()) {
                    const std::string idSource = !lobby.roomId.empty() ? lobby.roomId : (!lobby.room.empty() ? lobby.room : "raceday");
                    next.partyId = "room:" + idSource;
                    next.partySize = static_cast<int32_t>(lobby.players.size());
                    next.partyMax = 8;
                    next.joinSecret = lobby.inviteSecret;
                }
            } else if (online && lobby.inRoom) {
                const EventVote::View vote = EventVote::Snapshot();
                const std::string roomTitle = !lobby.room.empty() ? lobby.room : "Race Day Lobby";
                std::string details = roomTitle;
                if (lobby.eventCount > 0) {
                    details += " · Event " + std::to_string(lobby.eventIndex + 1) + "/" + std::to_string(lobby.eventCount);
                } else if (vote.current >= 0 && vote.current < static_cast<int>(vote.events.size())) {
                    details += " · " + vote.events[vote.current];
                }
                next.details = details;

                if (lobby.timer.seconds > 0 && !lobby.timer.paused) {
                    next.state = "Starting soon";
                    if (lobby.timer.total > 0) {
                        next.state += " (" + std::to_string(lobby.timer.ready) + "/" + std::to_string(lobby.timer.total) + " ready)";
                    }
                    const int64_t baseTs = lobby.timerUpdatedAt > 0 ? lobby.timerUpdatedAt : nowEpoch;
                    next.endTs = baseTs + lobby.timer.seconds;
                } else {
                    if (lobby.timer.paused) {
                        next.state = PauseReasonLabel(lobby.timer.reason, lobby.timer.who);
                    } else if (lobby.players.size() <= 1) {
                        next.state = hasCar ? ("Waiting for racers · " + displayCar) : "Waiting for racers";
                    } else {
                        next.state = hasCar ? ("In Lobby · " + displayCar) : "In Lobby";
                    }
                    next.startTs = lobbyEnteredEpoch > 0 ? lobbyEnteredEpoch : launchEpoch;
                }

                if (!lobby.players.empty()) {
                    const std::string idSource = !lobby.roomId.empty() ? lobby.roomId : roomTitle;
                    next.partyId = "room:" + idSource;
                    next.partySize = static_cast<int32_t>(lobby.players.size());
                    next.partyMax = 8;
                    next.joinSecret = lobby.inviteSecret;
                }

                std::string tip = atmName;
                if (hasCar) tip += " · " + displayCar;
                if (pingMs >= 0) tip += " · " + std::to_string(pingMs) + " ms";
                next.largeText = tip;
            } else if (online) {
                next.details = hasCar ? ("Online Menus · " + displayCar) : "Online Menus";
                next.state = "Connected to OpenProStreet" + (pingMs >= 0 ? (" · " + std::to_string(pingMs) + " ms") : "");
                next.startTs = launchEpoch;
                next.largeText = "OpenProStreet v" + version + " · Online";
            } else {
                // Singleplayer / Offline Career mode
                next.details = hasCar ? ("In Garage · " + displayCar) : "In Career Menus";
                if (hasMoney && hasXp) {
                    next.state = "$" + format_with_commas(money) + " · " + format_with_commas(xp) + " XP";
                } else if (hasMoney) {
                    next.state = "Career · $" + format_with_commas(money);
                } else if (hasXp) {
                    next.state = "Raceday · " + format_with_commas(xp) + " XP";
                } else {
                    next.state = "Singleplayer Menus";
                }
                next.startTs = launchEpoch;
                next.largeText = "OpenProStreet v" + version + " · Singleplayer";
            }

            next.details = Truncate(next.details, 127);
            next.state = Truncate(next.state, 127);
            next.largeText = Truncate(next.largeText, 127);
            next.partyId = Truncate(next.partyId, 127);
            next.joinSecret = Truncate(next.joinSecret, 127);

            if (discordCore) {
                int64_t reqUserId = 0;
                bool reqAccept = false;
                if (Overlay::ConsumeJoinRequestReply(&reqUserId, &reqAccept) && reqUserId != 0) {
                    discordCore->ActivityManager().SendRequestReply(
                        static_cast<discord::UserId>(reqUserId),
                        reqAccept ? discord::ActivityJoinRequestReply::Yes : discord::ActivityJoinRequestReply::Ignore,
                        [](discord::Result) {});
                }

                const time_t now = time(nullptr);
                const bool due = !hasSentOnce || next.MajorTransition(lastSent) ||
                                 (next.ContentDiffers(lastSent) && (now - lastUpdateAt >= UPDATE_MIN_INTERVAL_SEC));
                if (due) {
                    discord::Activity activity{};
                    activity.SetType(discord::ActivityType::Playing);
                    activity.SetApplicationId(DISCORD_APP_ID);
                    activity.SetName("Need for Speed: ProStreet");
                    activity.GetAssets().SetLargeImage("gcrown");
                    activity.GetAssets().SetLargeText(next.largeText.c_str());
                    activity.SetDetails(next.details.c_str());
                    activity.SetState(next.state.c_str());
                    if (next.endTs > 0) activity.GetTimestamps().SetEnd(next.endTs);
                    else if (next.startTs > 0) activity.GetTimestamps().SetStart(next.startTs);
                    if (!next.partyId.empty() && next.partyMax > 0) {
                        activity.GetParty().SetId(next.partyId.c_str());
                        activity.GetParty().GetSize().SetCurrentSize(next.partySize);
                        activity.GetParty().GetSize().SetMaxSize(next.partyMax);
                    }
                    if (!next.joinSecret.empty()) {
                        activity.GetSecrets().SetJoin(next.joinSecret.c_str());
                    }
                    discordCore->ActivityManager().UpdateActivity(activity, [](discord::Result) {});
                    lastSent = next;
                    hasSentOnce = true;
                    lastUpdateAt = now;
                }
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
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}
