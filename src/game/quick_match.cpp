#include "game/quick_match.h"

#include <cstdio>

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
                ServerLink::RequestQuickMatch(ranked);
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
