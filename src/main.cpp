#include "discord/discord_rpc.h"
#include "game/lanfix.h"
#include "game/race_results.h"
#include "game/tweaks.h"
#include "game/decals.h"
#include "game/quick_match.h"
#include "net/server_link.h"
#include <cstdio>
#include <discord.h>
#include <string>
#include <windows.h>

discord::Core *discordCore = nullptr;

const std::string version = "7.0";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    printf("[OpenProStreet] ASI attached. "
           "(https://code.weexnes.dev/OpenProStreetClient)\n");
    char procname[MAX_PATH];
    GetModuleFileNameA(NULL, procname, MAX_PATH);
    if (strstr(procname, "nfs.exe") == NULL) {
      printf("[OpenProStreet] Not nfs.exe, exiting.\n");
      return TRUE;
    }

    printf("[OpenProStreet] nfs.exe detected.\n");

    DisableThreadLibraryCalls(hModule);

    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);

    ServerLink::Init(baseModule);
    LANFix::Init();
    QuickMatch::Init(baseModule);
    Tweaks::Init(baseModule);
    Decals::Init(baseModule);
    RaceResults::Init(baseModule);

    HANDLE hThread = CreateThread(NULL, 0, RpcThread, NULL, 0, NULL);
    if (hThread) {
      printf("[OpenProStreet] RPC thread created successfully.\n");
      CloseHandle(hThread);
    } else {
      printf("[OpenProStreet] Failed to create RPC thread.\n");
    }
  }

  return TRUE;
}
