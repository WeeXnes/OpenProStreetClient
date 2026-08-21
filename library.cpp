#include "library.h"
#include "discord_rpc.h"
#include "lanfix.h"
#include "race_results.h"
#include "tweaks.h"
#include <cstdio>
#include <discord.h>
#include <string>
#include <windows.h>

discord::Core *discordCore = nullptr;

const std::string version = "4.2";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    printf("[ProStreetRPC] ASI attached. "
           "(https://code.weexnes.dev/ProStreetRPC)\n");
    char procname[MAX_PATH];
    GetModuleFileNameA(NULL, procname, MAX_PATH);
    if (strstr(procname, "nfs.exe") == NULL) {
      printf("[ProStreetRPC] Not nfs.exe, exiting.\n");
      return TRUE;
    }

    printf("[ProStreetRPC] nfs.exe detected.\n");

    DisableThreadLibraryCalls(hModule);

    uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);

    LANFix::Init();
    Tweaks::Init(baseModule);
    RaceResults::Init(baseModule);

    discord::Result result = discord::Core::Create(
        656565390411038731, DiscordCreateFlags_NoRequireDiscord, &discordCore);
    if (result != discord::Result::Ok) {
      printf("[ProStreetRPC] Failed to create Discord core: %d\n", (int)result);
      return FALSE;
    }
    printf("[ProStreetRPC] Discord core created successfully.\n");

    HANDLE hThread = CreateThread(NULL, 0, RpcThread, NULL, 0, NULL);
    if (hThread) {
      printf("[ProStreetRPC] RPC thread created successfully.\n");
      CloseHandle(hThread);
    } else {
      printf("[ProStreetRPC] Failed to create RPC thread.\n");
    }
  }

  return TRUE;
}
