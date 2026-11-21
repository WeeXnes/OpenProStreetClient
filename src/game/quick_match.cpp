#include "game/quick_match.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "game/hooks.h"
#include "game/lan_menu.h"
#include "game/map.h"
#include "game/overlay.h"
#include "game/race_mode.h"
#include "net/server_link.h"
#include "log.h"

namespace QuickMatch
{
    // nfs.exe v1.1, as offsets from the image base.
    static const uintptr_t MAIN_MENU_ON_OPTION = 0x1CA5E0; // 0x5CA5E0: FEOnlineMainStateManager's menu handler
    static const uintptr_t LAN_ON_GAME_LIST = 0x3E15C0;    // 0x7E15C0: FELanStateManager's game list listener

    static const uintptr_t LAN_JOIN_SELECTED = 0x3D2620; // 0x7D2620: FELanStateManager's join button
    static const uintptr_t LOBBY_SCREEN = 0x732B60;      // 0xB32B60: the game list screen, while it exists

    static const int MENU_STATE_MAIN = 2;       // FEOnlineMainStateManager +0x04 while its menu shows
    static const int OPTION_LAN_PLAY = 0x1F;
    static const int OPTION_QUICK_RANKED = 0x25;
    static const int OPTION_QUICK_UNRANKED = 0x26;

    // FELanStateManager states around the game list.
    static const int STATE_LOBBY = 0x14;        // the list shows, and the join button works
    static const int STATE_LOOKING = 0x15;      // asking the server for games
    static const int STATE_JOINING = 0x17;

    // The list screen's list widget is at +0x20, and its selected entry at +0x18 in that (0x5C0890).
    // After filling the list, the listener selects the first entry (0x7E1758). An entry's game name
    // is at +0x18, 64 characters (0x7E06FA), and that's what the join button sends.
    static const int LOBBY_SELECTED_ENTRY = 0x38;
    static const int ENTRY_GAME_NAME = 0x18;
    static const size_t GAME_NAME_SIZE = 64;

    static const int POPUP_REFRESH = 3; // LanMenu::POPUP: 0x5B97F0 asks the screen to refresh after the state change

    // How long after the first list a quick match may wait for its race day, before it's left to the player.
    static const DWORD JOIN_TIMEOUT_MS = 8000;

    static void* g_origOnOption = nullptr;
    static void* g_origOnGameList = nullptr;
    static uintptr_t g_baseModule = 0;
    static bool g_on = false;

    // Idle: LAN play as the game has it. Waiting: a quick match from the menu, until it joins a race
    // day or gives up. Joining: pressing the join button, so the hooks it calls don't press it again.
    enum Phase { PHASE_IDLE = 0, PHASE_WAITING = 1, PHASE_JOINING = 2 };
    static volatile LONG g_phase = PHASE_IDLE;
    static volatile LONG g_generation = 0; // one per quick match, so an old timeout doesn't end a new one
    static volatile bool g_listArrived = false;

    // The race day the server prepared or picked for this quick match or Discord invite.
    static CRITICAL_SECTION g_roomLock;
    static bool g_serverRoomKnown = false;
    static std::string g_serverRoom;
    static bool g_inviteMode = false;
    static std::string g_inviteSecret;
    static bool g_inviteSent = false;

    // What the game's "create race day" menus are built from (0x56C2E0, 0x56C5E0, 0x56C6C0, 0x578EB0).
    static const uintptr_t CAREER_DB_PTR = 0x6B9DC8;  // 0xAB9DC8
    static const uintptr_t EVENT_DB_PTR = 0x6B9D80;   // 0xAB9D80
    static const uintptr_t CAREER_COUNT = 0x24;
    static const uintptr_t CAREER_RACE_DAYS = 0x44;
    static const uintptr_t RACE_DAY_SIZE = 0x98;
    static const uint32_t NO_EVENT = 0x90A4D09A;
    static const int MODE_HIDDEN = 8;
    static const int LOW_DIFFICULTY_LIMIT = 2; // modes 0x13 to 0x15 aren't offered at difficulty 1 or 2
    static const int MAX_RANDOM_EVENTS = 4;
    static const int CATEGORY_WITHOUT_LAP_CHOICE = 2;

    typedef bool (__attribute__((thiscall)) *RaceDayFlagFn)(void*);
    typedef int (__attribute__((thiscall)) *RaceDayEventCountFn)(void*, int);
    typedef uint32_t (__attribute__((thiscall)) *RaceDayEventFn)(void*, int, int);
    typedef void* (__attribute__((thiscall)) *FindEventFn)(void*, uint32_t);
    typedef bool (__attribute__((thiscall)) *EventFlagFn)(void*);
    typedef uint32_t (__attribute__((thiscall)) *EventValueFn)(void*);
    typedef int (__cdecl *ModeCategoryFn)(int);

    struct EventChoice {
        uint32_t key;
        int mode;
        uint32_t laps;
        uint32_t heats;
        uint32_t maxLaps; // above 1 when the player may choose the lap count
        int raceNameId;   // tells a 1/4 mile drag from a 1/2 mile one
    };

    // Every event the menus would offer, by location, from the game's own data. An empty result
    // only means no room can be created, so it isn't an error.
    static std::map<uint32_t, std::vector<EventChoice> > ListEvents() {
        std::map<uint32_t, std::vector<EventChoice> > events;
        const uintptr_t base = g_baseModule;
        char* career = *reinterpret_cast<char**>(base + CAREER_DB_PTR);
        void* eventDb = *reinterpret_cast<void**>(base + EVENT_DB_PTR);
        if (!career || !eventDb) return events;

        RaceDayFlagFn raceDayHidden = reinterpret_cast<RaceDayFlagFn>(base + 0x259A50);
        RaceDayEventCountFn eventCount = reinterpret_cast<RaceDayEventCountFn>(base + 0x261910);
        RaceDayEventFn eventKey = reinterpret_cast<RaceDayEventFn>(base + 0x259AF0);
        FindEventFn findEvent = reinterpret_cast<FindEventFn>(base + 0x25EA10);
        EventFlagFn eventHidden = reinterpret_cast<EventFlagFn>(base + 0x286080);
        EventFlagFn hasLapChoice = reinterpret_cast<EventFlagFn>(base + 0x286180);
        EventValueFn eventLocation = reinterpret_cast<EventValueFn>(base + 0x2828F0);
        EventValueFn eventMode = reinterpret_cast<EventValueFn>(base + 0x282590);
        EventValueFn eventHeats = reinterpret_cast<EventValueFn>(base + 0x257C90);
        EventValueFn eventLaps = reinterpret_cast<EventValueFn>(base + 0x257CF0);
        EventValueFn eventRaceName = reinterpret_cast<EventValueFn>(base + 0x257CC0); // the drag distance
        ModeCategoryFn modeCategory = reinterpret_cast<ModeCategoryFn>(base + 0x257EC0);

        std::set<uint32_t> seen;
        const uint32_t count = *reinterpret_cast<uint32_t*>(career + CAREER_COUNT);
        char* raceDays = *reinterpret_cast<char**>(career + CAREER_RACE_DAYS);
        for (uint32_t day = 0; raceDays && day < count; ++day) {
            void* raceDay = raceDays + day * RACE_DAY_SIZE;
            if (raceDayHidden(raceDay)) continue;
            const int total = eventCount(raceDay, 0);
            for (int i = 0; i < total; ++i) {
                const uint32_t key = eventKey(raceDay, i, 0);
                if (key == NO_EVENT || !seen.insert(key).second) continue;
                void* event = findEvent(eventDb, key);
                if (!event || eventHidden(event)) continue;

                const uint32_t location = eventLocation(event);
                const int mode = static_cast<int>(eventMode(event));
                const int category = modeCategory(mode);
                if (!location || mode == MODE_HIDDEN || category < 1 || category > 4) continue;

                EventChoice choice;
                choice.key = key;
                choice.mode = mode;
                choice.heats = eventHeats(event);
                choice.laps = eventLaps(event);
                choice.raceNameId = static_cast<int>(eventRaceName(event));
                if (choice.heats < 1) choice.heats = 1;
                if (choice.laps < 1) choice.laps = 1;
                // 0x579409: without several heats, some events let the player pick the laps.
                choice.maxLaps = choice.heats == 1 && hasLapChoice(event) && category != CATEGORY_WITHOUT_LAP_CHOICE ? choice.laps : 1;
                events[location].push_back(choice);
            }
        }
        return events;
    }

    // A random location, difficulty and set of events, as the player could pick in the menus.
    static void PickRaceDay(std::string& location, int& difficulty, std::string& text) {
        const std::map<uint32_t, std::vector<EventChoice> > all = ListEvents();
        if (all.empty()) {
            Log::Warn("Quick match: the game has no events to pick from yet.\n");
            return;
        }
        std::mt19937 random(GetTickCount());
        std::map<uint32_t, std::vector<EventChoice> >::const_iterator chosen = all.begin();
        std::advance(chosen, std::uniform_int_distribution<size_t>(0, all.size() - 1)(random));
        difficulty = std::uniform_int_distribution<int>(1, 4)(random);

        std::vector<EventChoice> events;
        for (size_t i = 0; i < chosen->second.size(); ++i) {
            const int mode = chosen->second[i].mode;
            if (difficulty > LOW_DIFFICULTY_LIMIT || mode < 0x13 || mode > 0x15) events.push_back(chosen->second[i]);
        }
        if (events.empty()) return;
        std::shuffle(events.begin(), events.end(), random);
        events.resize(std::uniform_int_distribution<size_t>(1, (std::min)(events.size(), static_cast<size_t>(MAX_RANDOM_EVENTS)))(random));

        location = std::to_string(chosen->first);
        for (size_t i = 0; i < events.size(); ++i) {
            const EventChoice& event = events[i];
            const uint32_t laps = event.maxLaps > 1 ? std::uniform_int_distribution<uint32_t>(1, event.maxLaps)(random) : event.laps;
            if (i != 0) text += ",";
            text += std::to_string(event.key) + ":" + std::to_string(laps) + ":" + std::to_string(event.heats);
        }
        Log::Debug("[OpenProStreet] Quick match: picked %u event(s) at location %s, difficulty %d.\n", static_cast<unsigned>(events.size()), location.c_str(), difficulty);
        Log::Summary(Log::Tag::Match, "race day if a new room is needed: %u event(s), difficulty %d\n", static_cast<unsigned>(events.size()), difficulty);
    }

    // With DumpEvents=1 under [Debug] in the settings file, writes every location and event the
    // menus offer for a server's event_catalog.json. Only the game knows them, so a server admin runs
    // this once. The event data is loaded by the time the player can pick an online option.
    static bool g_dumpEvents = false;

    static void DumpEventCatalog() {
        g_dumpEvents = false;
        const std::map<uint32_t, std::vector<EventChoice> > all = ListEvents();
        if (all.empty()) {
            Log::Warn("Event dump: the game has no events loaded yet.\n");
            g_dumpEvents = true;
            return;
        }
        const char* appData = getenv("LOCALAPPDATA");
        const std::string path = std::string(appData ? appData : ".") + "\\OpenProStreet\\event_catalog.json";
        FILE* f = fopen(path.c_str(), "w");
        if (!f) {
            Log::Warn("Event dump: could not write %s\n", path.c_str());
            return;
        }
        fputs("{\"locations\":[\n", f);
        bool firstLocation = true;
        for (std::map<uint32_t, std::vector<EventChoice> >::const_iterator location = all.begin(); location != all.end(); ++location) {
            std::string name = MapNameForKey(location->first);
            if (name.empty()) name = "Location " + std::to_string(location->first);
            fprintf(f, "%s{\"key\":%u,\"name\":\"%s\",\"events\":[\n", firstLocation ? "" : ",\n", location->first, name.c_str());
            firstLocation = false;
            for (size_t i = 0; i < location->second.size(); ++i) {
                const EventChoice& event = location->second[i];
                std::string modeName = RaceModeName(event.mode, event.raceNameId);
                if (modeName.empty()) modeName = "Mode " + std::to_string(event.mode);
                fprintf(f, "%s{\"key\":%u,\"name\":\"%s %u\",\"mode\":%d,\"modeName\":\"%s\",\"laps\":%u,\"heats\":%u,\"maxLaps\":%u}",
                        i == 0 ? "" : ",\n", event.key, modeName.c_str(), static_cast<unsigned>(i + 1), event.mode, modeName.c_str(), event.laps, event.heats, event.maxLaps);
            }
            fputs("\n]}", f);
        }
        fputs("\n]}\n", f);
        fclose(f);
        Log::Event(Log::Tag::Client, "Wrote the event catalog to %s\n", path.c_str());
    }

    typedef void (__attribute__((thiscall)) *OnOptionFn)(void*, int, int);
    typedef void (__attribute__((thiscall)) *OnGameListFn)(void*, void*);
    typedef int (__attribute__((thiscall)) *ListCountFn)(void*);
    typedef void (__attribute__((thiscall)) *JoinSelectedFn)(void*);

    static void ForgetServerRoom() {
        EnterCriticalSection(&g_roomLock);
        g_serverRoomKnown = false;
        g_serverRoom.clear();
        LeaveCriticalSection(&g_roomLock);
    }

    void SetServerRoom(const std::string& gameName) {
        if (!g_on) return;
        EnterCriticalSection(&g_roomLock);
        if (!g_inviteMode) {
            g_serverRoomKnown = true;
            g_serverRoom = gameName;
        }
        LeaveCriticalSection(&g_roomLock);
        Log::Debug("[OpenProStreet] Quick match: the server's race day is [%s].\n", gameName.empty() ? "none yet" : gameName.c_str());
    }

    static std::string InviteErrorText(const std::string& err) {
        if (err == "roomFull") return "That race day is already full.";
        if (err == "roomClosed") return "That race day is locked or already racing.";
        if (err == "clientUpdateRequired") return "Update OpenProStreetClient to join.";
        return "That race day invite has expired or ended.";
    }

    void RequestInviteJoin(const std::string& secret) {
        if (secret.empty()) return;
        bool connected = ServerLink::IsConnected();
        EnterCriticalSection(&g_roomLock);
        g_inviteMode = true;
        g_inviteSecret = secret;
        g_inviteSent = connected;
        g_serverRoomKnown = false;
        g_serverRoom.clear();
        LeaveCriticalSection(&g_roomLock);

        InterlockedIncrement(&g_generation);
        g_listArrived = false;
        InterlockedExchange(&g_phase, PHASE_WAITING);

        Log::Event(Log::Tag::Discord, "Discord invite received; preparing to join invited race day.\n");
        if (connected) {
            ServerLink::RequestJoinInvite(secret);
        } else {
            Overlay::PushNotice("best", "Discord invite ready",
                                std::vector<std::string>{"Open LAN Play and log in to join the race day."});
        }
    }

    void OnServerConnected() {
        std::string secret;
        EnterCriticalSection(&g_roomLock);
        if (g_inviteMode && !g_inviteSecret.empty() && !g_inviteSent) {
            secret = g_inviteSecret;
            g_inviteSent = true;
        }
        LeaveCriticalSection(&g_roomLock);
        if (!secret.empty()) ServerLink::RequestJoinInvite(secret);
    }

    void SetInviteRoom(const std::string& gameName, const std::string& displayName, const std::string& error) {
        if (!error.empty() || gameName.empty()) {
            EnterCriticalSection(&g_roomLock);
            g_inviteMode = false;
            g_inviteSecret.clear();
            g_inviteSent = false;
            g_serverRoomKnown = false;
            g_serverRoom.clear();
            LeaveCriticalSection(&g_roomLock);
            InterlockedExchange(&g_phase, PHASE_IDLE);
            Overlay::PushNotice("not_counted", "Invite unavailable", std::vector<std::string>{InviteErrorText(error)});
            return;
        }

        EnterCriticalSection(&g_roomLock);
        g_inviteMode = true;
        g_serverRoomKnown = true;
        g_serverRoom = gameName;
        LeaveCriticalSection(&g_roomLock);

        const std::string label = displayName.empty() ? gameName : displayName;
        Overlay::PushNotice("best", "Discord invite: " + label,
                            std::vector<std::string>{"Open LAN Play (or refresh the game list) to join."});
        Log::Debug("[OpenProStreet] Discord invite resolved to room [%s].\n", gameName.c_str());
    }

    // A few seconds after the first list, a quick match that couldn't join leaves the list to the player.
    static DWORD WINAPI JoinTimeout(LPVOID parameter) {
        const LONG generation = static_cast<LONG>(reinterpret_cast<intptr_t>(parameter));
        Sleep(JOIN_TIMEOUT_MS);
        if (g_generation != generation) return 0;
        if (InterlockedCompareExchange(&g_phase, PHASE_IDLE, PHASE_WAITING) != PHASE_WAITING) return 0;
        bool wasInvite = false;
        EnterCriticalSection(&g_roomLock);
        wasInvite = g_inviteMode;
        g_inviteMode = false;
        g_inviteSecret.clear();
        g_inviteSent = false;
        LeaveCriticalSection(&g_roomLock);
        if (wasInvite) ServerLink::CancelInvite();
        else ServerLink::CancelQuickMatch();
        Log::Debug("[OpenProStreet] Match join timed out after %lu ms, leaving the list to the player.\n", JOIN_TIMEOUT_MS);
        Log::Summary(Log::Tag::Match, "didn't auto-join yet, pick a race day from the list\n");
        return 0;
    }

    // What the join button would do, once the list shows and nothing else is changing. The button
    // (0x7D2620) does nothing outside state 0x14. The list arrives in state 0x15, so pressing it from
    // the list listener, as versions before 8.2 did, never joined.
    static void TryJoin(void* manager, const char* from) {
        if (g_phase != PHASE_WAITING || !g_listArrived) return;
        if (LanMenu::State(manager) != STATE_LOBBY || !LanMenu::IsSettled(manager)) return;
        const char* lobby = *reinterpret_cast<const char**>(g_baseModule + LOBBY_SCREEN);
        if (!lobby) return;
        const char* entry = *reinterpret_cast<const char* const*>(lobby + LOBBY_SELECTED_ENTRY);
        if (!entry) return;

        char name[GAME_NAME_SIZE + 1] = {};
        memcpy(name, entry + ENTRY_GAME_NAME, GAME_NAME_SIZE);

        // The server lists its quick play race day first. When it also says which one that is, only
        // that one is joined, so a race day a player made is never picked by mistake.
        EnterCriticalSection(&g_roomLock);
        const bool known = g_serverRoomKnown;
        const std::string room = g_serverRoom;
        LeaveCriticalSection(&g_roomLock);
        if (known && (room.empty() || _stricmp(room.c_str(), name) != 0)) {
            Log::Debug("[OpenProStreet] Quick match: the first race day is [%s], the server's is [%s]. Not joining.\n",
                       name, room.empty() ? "none yet" : room.c_str());
            return;
        }

        if (InterlockedCompareExchange(&g_phase, PHASE_JOINING, PHASE_WAITING) != PHASE_WAITING) return;
        Log::Debug("[OpenProStreet] Quick match: pressing join for [%s] (%s).\n", name, from);
        reinterpret_cast<JoinSelectedFn>(g_baseModule + LAN_JOIN_SELECTED)(manager);
        if (LanMenu::State(manager) == STATE_JOINING) {
            InterlockedExchange(&g_phase, PHASE_IDLE);
            EnterCriticalSection(&g_roomLock);
            const bool joinedInvite = g_inviteMode;
            g_inviteMode = false;
            g_inviteSecret.clear();
            g_inviteSent = false;
            LeaveCriticalSection(&g_roomLock);
            Log::Event(joinedInvite ? Log::Tag::Discord : Log::Tag::Match,
                       joinedInvite ? "Joining invited race day %s\n" : "Joining quick play race day %s\n",
                       Log::Highlight(name).c_str());
        } else {
            InterlockedExchange(&g_phase, PHASE_WAITING);
            Log::Debug("[OpenProStreet] Quick match: the join button did nothing (state 0x%X).\n", LanMenu::State(manager));
        }
    }

    bool IsOn() {
        return g_on;
    }

    // The change back to the list after it arrived ends here (0x5B97F0), with the list filled.
    void OnStateEntered(void* manager) {
        if (g_phase != PHASE_WAITING) return;
        if (LanMenu::State(manager) != STATE_LOBBY || LanMenu::PreviousState(manager) != STATE_LOOKING) return;
        // 0x5B97F0 goes on after this call. With 3 it asks the screen showing to refresh, which
        // shouldn't happen to the "joining" popup, so that case waits for the screen to show.
        if (*reinterpret_cast<int*>(static_cast<char*>(manager) + LanMenu::POPUP) == POPUP_REFRESH) return;
        TryJoin(manager, "list shown");
    }

    // Before the game's own slot 74, which goes back to looking for games while the state is 0x14.
    void OnScreenShown(void* manager) {
        TryJoin(manager, "screen shown");
    }

    // "Schnelles Spiel" would log in to EA's servers, which are gone. The LAN menu option starts
    // the same kind of child flow from the same place, so quick match takes that path instead.
    static void __attribute__((thiscall)) OnOptionHook(void* manager, int option, int param) {
        const int state = *reinterpret_cast<int*>(reinterpret_cast<char*>(manager) + 4);
        if (state == MENU_STATE_MAIN) {
            if (g_dumpEvents && (option == OPTION_LAN_PLAY || option == OPTION_QUICK_RANKED || option == OPTION_QUICK_UNRANKED)) DumpEventCatalog();
            if (option == OPTION_QUICK_RANKED || option == OPTION_QUICK_UNRANKED) {
                const bool ranked = option == OPTION_QUICK_RANKED;
                Log::Debug("[OpenProStreet] %s quick match: using the OpenProStreet server instead of EA's.\n", ranked ? "Ranked" : "Unranked");
                Log::Summary(Log::Tag::Match, "%s quick match on the OpenProStreet server\n", ranked ? "ranked" : "unranked");
                InterlockedIncrement(&g_generation);
                g_listArrived = false;
                ForgetServerRoom();
                EnterCriticalSection(&g_roomLock);
                g_inviteMode = false;
                g_inviteSecret.clear();
                g_inviteSent = false;
                LeaveCriticalSection(&g_roomLock);
                ServerLink::CancelInvite();
                InterlockedExchange(&g_phase, PHASE_WAITING);
                std::string location, events;
                int difficulty = 0;
                PickRaceDay(location, difficulty, events);
                ServerLink::RequestQuickMatch(ranked, location, difficulty, events);
                option = OPTION_LAN_PLAY;
            } else if (option == OPTION_LAN_PLAY) {
                InterlockedIncrement(&g_generation);
                g_listArrived = false;
                EnterCriticalSection(&g_roomLock);
                const bool hasInvite = g_inviteMode && !g_inviteSecret.empty();
                LeaveCriticalSection(&g_roomLock);
                if (hasInvite) {
                    InterlockedExchange(&g_phase, PHASE_WAITING);
                    ServerLink::CancelQuickMatch();
                } else {
                    InterlockedExchange(&g_phase, PHASE_IDLE);
                    ServerLink::CancelQuickMatch();
                }
            }
        }
        reinterpret_cast<OnOptionFn>(g_origOnOption)(manager, option, param);
    }

    // Quick play lists the same games as LAN play. The server puts its quick play race day first,
    // and makes one when none is open. The quick match stays on with the server until the player
    // joins (the server ends it then) or it gives up, so later lists keep that race day first.
    static void __attribute__((thiscall)) OnGameListHook(void* listener, void* games) {
        reinterpret_cast<OnGameListFn>(g_origOnGameList)(listener, games);
        if (g_phase != PHASE_WAITING || !games || g_listArrived) return;
        g_listArrived = true;
        const int count = (**reinterpret_cast<ListCountFn**>(games))(games);
        Log::Debug("[OpenProStreet] Quick match: the list is in, %d race day(s). Joining the first once the list shows.\n", count);
        Log::Summary(Log::Tag::Match, "list is in, joining the quick play race day\n");
        HANDLE thread = CreateThread(nullptr, 0, JoinTimeout, reinterpret_cast<LPVOID>(static_cast<intptr_t>(g_generation)), 0, nullptr);
        if (thread) CloseHandle(thread);
    }

    void Init(uintptr_t baseModule) {
        g_baseModule = baseModule;
        InitializeCriticalSection(&g_roomLock);
        const std::string& settings = Log::SettingsPath();
        g_dumpEvents = !settings.empty() && GetPrivateProfileIntA("Debug", "DumpEvents", 0, settings.c_str()) != 0;
        const bool ok =
            Hooks::InstallDetour((void*)(baseModule + MAIN_MENU_ON_OPTION), (void*)OnOptionHook, &g_origOnOption) &&
            Hooks::InstallDetour((void*)(baseModule + LAN_ON_GAME_LIST), (void*)OnGameListHook, &g_origOnGameList);
        g_on = ok;
        if (ok) Log::Debug("[OpenProStreet] Quick match hooks installed.\n");
        else Log::Error("Could not install the quick match hooks.\n");
    }
}
