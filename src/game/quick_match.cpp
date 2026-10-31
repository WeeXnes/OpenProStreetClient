#include "game/quick_match.h"

#include <cstdio>

#include "game/hooks.h"
#include "net/server_link.h"

namespace QuickMatch
{
    // DALOnlineMatchmakingCommands::*::Execute, vtable slot 2 of each command object (nfs.exe v1.1).
    static const uintptr_t START_QUICK_MATCH = 0x144730;           // 0x544730
    static const uintptr_t START_QUICK_MATCH_FREESTYLE = 0x144770; // 0x544770
    static const uintptr_t CONTINUE_SEARCH_FREESTYLE = 0x1447B0;   // 0x5447B0
    static const uintptr_t START_CUSTOM_HUB = 0x1447E0;            // 0x5447E0
    static const uintptr_t CANCEL_QUICK_MATCH = 0x144840;          // 0x544840

    static void* g_origStart = nullptr;
    static void* g_origStartFreestyle = nullptr;
    static void* g_origContinue = nullptr;
    static void* g_origCustom = nullptr;
    static void* g_origCancel = nullptr;

    typedef void (__attribute__((thiscall)) *CommandFn)(void*);

    static void __attribute__((thiscall)) StartHook(void* self) {
        ServerLink::RequestQuickMatch();
        reinterpret_cast<CommandFn>(g_origStart)(self);
    }

    static void __attribute__((thiscall)) StartFreestyleHook(void* self) {
        ServerLink::RequestQuickMatch();
        reinterpret_cast<CommandFn>(g_origStartFreestyle)(self);
    }

    static void __attribute__((thiscall)) ContinueHook(void* self) {
        ServerLink::RequestQuickMatch();
        reinterpret_cast<CommandFn>(g_origContinue)(self);
    }

    // A player hosting their own hub must get an empty room, not somebody else's.
    static void __attribute__((thiscall)) CustomHook(void* self) {
        ServerLink::CancelQuickMatch();
        reinterpret_cast<CommandFn>(g_origCustom)(self);
    }

    static void __attribute__((thiscall)) CancelHook(void* self) {
        ServerLink::CancelQuickMatch();
        reinterpret_cast<CommandFn>(g_origCancel)(self);
    }

    void Init(uintptr_t baseModule) {
        const bool ok =
            Hooks::InstallDetour((void*)(baseModule + START_QUICK_MATCH), (void*)StartHook, &g_origStart) &&
            Hooks::InstallDetour((void*)(baseModule + START_QUICK_MATCH_FREESTYLE), (void*)StartFreestyleHook, &g_origStartFreestyle) &&
            Hooks::InstallDetour((void*)(baseModule + CONTINUE_SEARCH_FREESTYLE), (void*)ContinueHook, &g_origContinue) &&
            Hooks::InstallDetour((void*)(baseModule + START_CUSTOM_HUB), (void*)CustomHook, &g_origCustom) &&
            Hooks::InstallDetour((void*)(baseModule + CANCEL_QUICK_MATCH), (void*)CancelHook, &g_origCancel);
        printf("[OpenProStreet] %s\n", ok ? "Quick match hooks installed." : "Could not install the quick match hooks.");
    }
}
