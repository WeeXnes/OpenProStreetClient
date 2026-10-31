#include "game/quick_match.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "game/hooks.h"
#include "net/server_link.h"

namespace QuickMatch
{
    // nfs.exe v1.1, as offsets from the image base.
    static const uintptr_t MAIN_MENU_ON_OPTION = 0x1CA5E0; // 0x5CA5E0: FEOnlineMainStateManager's menu handler
    static const uintptr_t LAN_ON_GAME_LIST = 0x3E15C0;    // 0x7E15C0: FELanStateManager's game list listener
    static const uintptr_t LAN_JOIN_SELECTED = 0x3D2620;   // 0x7D2620: FELanStateManager's join button

    static const int MENU_STATE_MAIN = 2;       // FEOnlineMainStateManager +0x04 while its menu shows
    static const int OPTION_LAN_PLAY = 0x1F;
    static const int OPTION_QUICK_RANKED = 0x25;
    static const int OPTION_QUICK_UNRANKED = 0x26;
    static const int LISTENER_TO_MANAGER = 0xCC; // the game list listener's offset in FELanStateManager

    static void* g_origOnOption = nullptr;
    static void* g_origOnGameList = nullptr;
    static uintptr_t g_baseModule = 0;

    // Set from the menu until the lobby joins a room.
    static volatile bool g_waitingForRoom = false;

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
            printf("[OpenProStreet] Quick match: the game has no events to pick from yet.\n");
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
        printf("[OpenProStreet] Quick match: picked %u event(s) at location %s, difficulty %d.\n", static_cast<unsigned>(events.size()), location.c_str(), difficulty);
    }

    typedef void (__attribute__((thiscall)) *OnOptionFn)(void*, int, int);
    typedef void (__attribute__((thiscall)) *OnGameListFn)(void*, void*);
    typedef int (__attribute__((thiscall)) *ListCountFn)(void*);
    typedef void (__attribute__((thiscall)) *JoinSelectedFn)(void*);

    // "Schnelles Spiel" would log in to EA's servers, which are gone. The LAN menu option starts
    // the same kind of child flow from the same place, so quick match takes that path instead.
    static void __attribute__((thiscall)) OnOptionHook(void* manager, int option, int param) {
        const int state = *reinterpret_cast<int*>(reinterpret_cast<char*>(manager) + 4);
        if (state == MENU_STATE_MAIN) {
            if (option == OPTION_QUICK_RANKED || option == OPTION_QUICK_UNRANKED) {
                const bool ranked = option == OPTION_QUICK_RANKED;
                printf("[OpenProStreet] %s quick match: using the OpenProStreet server instead of EA's.\n", ranked ? "Ranked" : "Unranked");
                g_waitingForRoom = true;
                std::string location, events;
                int difficulty = 0;
                PickRaceDay(location, difficulty, events);
                ServerLink::RequestQuickMatch(ranked, location, difficulty, events);
                option = OPTION_LAN_PLAY;
            } else if (option == OPTION_LAN_PLAY) {
                g_waitingForRoom = false;
                ServerLink::CancelQuickMatch();
            }
        }
        reinterpret_cast<OnOptionFn>(g_origOnOption)(manager, option, param);
    }

    // During a quick match the server only lists rooms the player can join, best first, and the
    // game selects the first entry. So this is what pressing "join" in the lobby would do. With no
    // room, the lobby stays open: the player can wait for the next list, or create a race day.
    static void __attribute__((thiscall)) OnGameListHook(void* listener, void* games) {
        reinterpret_cast<OnGameListFn>(g_origOnGameList)(listener, games);
        if (!g_waitingForRoom || !games) return;

        ListCountFn count = **reinterpret_cast<ListCountFn**>(games);
        if (count(games) <= 0) {
            printf("[OpenProStreet] Quick match: no open rooms yet.\n");
            return;
        }
        g_waitingForRoom = false;
        printf("[OpenProStreet] Quick match: joining the best room.\n");
        void* manager = reinterpret_cast<char*>(listener) - LISTENER_TO_MANAGER;
        reinterpret_cast<JoinSelectedFn>(g_baseModule + LAN_JOIN_SELECTED)(manager);
    }

    void Init(uintptr_t baseModule) {
        g_baseModule = baseModule;
        const bool ok =
            Hooks::InstallDetour((void*)(baseModule + MAIN_MENU_ON_OPTION), (void*)OnOptionHook, &g_origOnOption) &&
            Hooks::InstallDetour((void*)(baseModule + LAN_ON_GAME_LIST), (void*)OnGameListHook, &g_origOnGameList);
        printf("[OpenProStreet] %s\n", ok ? "Quick match hooks installed." : "Could not install the quick match hooks.");
    }
}
