#include "discord/discord_rpc.h"
#include "game/borderless.h"
#include "game/car_lock.h"
#include "game/input_focus.h"
#include "game/lanfix.h"
#include "game/race_results.h"
#include "game/tweaks.h"
#include "game/quick_match.h"
#include "net/server_link.h"
#include "log.h"
#include <cstdio>
#include <discord.h>
#include <string>
#include <windows.h>

discord::Core *discordCore = nullptr;

const std::string version = "8.1";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    Log::Init(hModule);
    Log::Debug("[OpenProStreet] ASI attached. "
           "(https://code.weexnes.dev/OpenProStreetClient)\n");
    char procname[MAX_PATH];
    GetModuleFileNameA(NULL, procname, MAX_PATH);
    if (strstr(procname, "nfs.exe") == NULL) {
      Log::Debug("[OpenProStreet] Not nfs.exe, exiting.\n");
      return TRUE;
    }

    Log::Debug("[OpenProStreet] nfs.exe detected.\n");
    Log::Banner("OpenProStreet Client " + version,
                {{"source", "code.weexnes.dev/OpenProStreetClient"},
                 {"log", "short. Set Verbose=1 under [Log] in scripts\\OpenProStreetClient.ini for detail"}});

    DisableThreadLibraryCalls(hModule);

    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);

    ServerLink::Init(baseModule);
    LANFix::Init();
    QuickMatch::Init(baseModule);
    Tweaks::Init(baseModule);
    Borderless::Init(baseModule);
    RaceResults::Init(baseModule);
    CarLock::Init(baseModule);
    InputFocus::Init(baseModule);

    HANDLE hThread = CreateThread(NULL, 0, RpcThread, NULL, 0, NULL);
    if (hThread) {
      Log::Debug("[OpenProStreet] RPC thread created successfully.\n");
      CloseHandle(hThread);
    } else {
      Log::Warn("Failed to create RPC thread.\n");
    }
  }

  return TRUE;
}
