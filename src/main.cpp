#include "discord/discord_rpc.h"
#include "game/auto_login.h"
#include "game/borderless.h"
#include "game/car_lock.h"
#include "game/input_focus.h"
#include "game/event_vote.h"
#include "game/display_test.h"
#include "game/lan_menu.h"
#include "game/overlay.h"
#include "game/atmosphere.h"
#include "game/lanfix.h"
#include "game/race_results.h"
#include "game/tweaks.h"
#include "game/vault_fixes.h"
#include "game/quick_match.h"
#include "net/server_link.h"
#include "log.h"
#include <cstdio>
#include <discord.h>
#include <string>
#include <windows.h>

discord::Core *discordCore = nullptr;

const std::string version = "10.6";

// Every hook patches nfs.exe v1.1 at fixed addresses, so the client only runs on that layout. The
// game's version check pushes 1, 1 here (MultiFix reads the same bytes). A packed exe, such as the
// "Vitality" 1.1 No-CD, still has its code compressed when plugins load, so this reads as zero.
static const uintptr_t VERSION_MARKER = 0x2CC3A6;
static const uint32_t VERSION_MARKER_V11 = 0x016A016A;

static bool IsSupportedGame(uintptr_t baseModule) {
  const void* marker = reinterpret_cast<const void*>(baseModule + VERSION_MARKER);
  return !IsBadReadPtr(marker, sizeof(uint32_t)) &&
         *reinterpret_cast<const uint32_t*>(marker) == VERSION_MARKER_V11;
}

// A message box can't open while the loader holds its lock, so it gets its own thread.
static DWORD WINAPI UnsupportedGameNotice(LPVOID) {
  MessageBoxA(nullptr,
              "This nfs.exe isn't supported, so the OpenProStreet client is turned off.\n\n"
              "The client needs the unpacked version 1.1 of nfs.exe. This one is either another "
              "version or packed (like the \"Vitality\" 1.1 No-CD), so the game's code isn't "
              "where the client expects it. XNFSPS MultiFix doesn't work with it either, which is "
              "why settings like the resolution don't stick.\n\n"
              "See https://ops.weexnes.dev/guide for what you need.",
              "OpenProStreet", MB_OK | MB_ICONWARNING | MB_TOPMOST);
  return 0;
}

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

    if (!IsSupportedGame(baseModule)) {
      Log::Error("This nfs.exe isn't the unpacked version 1.1 the client is made for. "
                 "The client is turned off and changes nothing in the game.\n");
      HANDLE notice = CreateThread(NULL, 0, UnsupportedGameNotice, NULL, 0, NULL);
      if (notice) CloseHandle(notice);
      return TRUE;
    }

    ServerLink::Init(baseModule);
    LANFix::Init();
    QuickMatch::Init(baseModule);
    Tweaks::SetVersion(version.c_str());
    Tweaks::Init(baseModule);
    VaultFixes::Init(baseModule);
    DisplayTest::Init(baseModule);
    Overlay::Init(baseModule, version);
    Atmosphere::Init(baseModule);
    Borderless::Init(baseModule);
    RaceResults::Init(baseModule);
    CarLock::Init(baseModule);
    InputFocus::Init(baseModule);
    EventVote::Init();
    AutoLogin::Init(baseModule);
    LanMenu::Init(baseModule);

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
