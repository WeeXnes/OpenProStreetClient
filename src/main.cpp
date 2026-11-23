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
#include "game/camera_orbit.h"
#include "game/graphics.h"
#include "game/hooks.h"
#include "game/reshifted.h"
#include "game/textures.h"
#include "game/lanfix.h"
#include "game/race_results.h"
#include "game/tweaks.h"
#include "game/vault_fixes.h"
#include "game/quick_match.h"
#include "net/server_link.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <discord.h>
#include <shellapi.h>
#include <string>
#include <windows.h>

discord::Core *discordCore = nullptr;
static HMODULE g_selfModule = nullptr;
static std::string g_gameDir;
static void* g_origCrtStartup = nullptr;

// Unpacked nfs.exe v1.1 __tmainCRTStartup (jumped to from AddressOfEntryPoint 0x828C25);
// starts with `push 0x10; push 0xA42D70` (7 bytes).
static const uintptr_t CRT_STARTUP = 0x4289CD;

static void EnsureGameWorkingDirectory() {
  if (!g_gameDir.empty()) {
    SetCurrentDirectoryA(g_gameDir.c_str());
  }
}

// Ultimate ASI Loader saves GetCurrentDirectoryW before loading plugins and restores it via
// SetCurrentDirectoryW after each LoadLibraryW returns, undoing DllMain's SetCurrentDirectoryA.
// Re-apply right as __tmainCRTStartup starts (after LoadEverything finishes, before WinMain/d3d9).
static int __cdecl CrtStartupHook() {
  EnsureGameWorkingDirectory();
  return reinterpret_cast<int(__cdecl*)()>(g_origCrtStartup)();
}

const std::string baseVersion = "10.24";
#ifdef OPENPROSTREET_VERSION_SUFFIX
const std::string version = baseVersion + OPENPROSTREET_VERSION_SUFFIX;
#else
const std::string version = baseVersion;
#endif

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

// Exported for rundll32.exe so Discord's "Launch Game" URI launches nfs.exe with WorkingDirectory = <game>.
extern "C" __declspec(dllexport) void CALLBACK LaunchGame(HWND, HINSTANCE, LPSTR, int) {
  char asiPath[MAX_PATH] = {};
  if (!g_selfModule || GetModuleFileNameA(g_selfModule, asiPath, MAX_PATH) == 0) return;
  std::string dir(asiPath);
  const size_t fileSlash = dir.find_last_of("\\/");
  if (fileSlash == std::string::npos) return;
  dir.resize(fileSlash);
  const size_t parentSlash = dir.find_last_of("\\/");
  if (parentSlash != std::string::npos) {
    const std::string leaf = dir.substr(parentSlash + 1);
    if (_stricmp(leaf.c_str(), "scripts") == 0) dir.resize(parentSlash);
  }
  const std::string exe = dir + "\\nfs.exe";
  if (reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL)) <= 32) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
  }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    g_selfModule = hModule;
    char procname[MAX_PATH];
    GetModuleFileNameA(NULL, procname, MAX_PATH);
    if (strstr(procname, "nfs.exe") == NULL) {
      return TRUE;
    }

    // Discord's Launch Game URL handler can spawn nfs.exe with Discord's working directory;
    // ProStreet opens game assets via relative paths, so anchor cwd to the game folder.
    const std::string exeStr(procname);
    const size_t slash = exeStr.find_last_of("\\/");
    if (slash != std::string::npos) {
      g_gameDir = exeStr.substr(0, slash);
      EnsureGameWorkingDirectory();
    }

    Log::Init(hModule);
    Log::Debug("[OpenProStreet] ASI attached. "
           "(https://code.weexnes.dev/OpenProStreet/client)\n");

    Log::Debug("[OpenProStreet] nfs.exe detected.\n");
    Log::Banner("OpenProStreet Client " + version,
                {{"source", "code.weexnes.dev/OpenProStreet/client"},
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

    Hooks::InstallDetour(reinterpret_cast<void*>(baseModule + CRT_STARTUP),
                         reinterpret_cast<void*>(CrtStartupHook), &g_origCrtStartup, 7);

    ServerLink::Init(baseModule);
    LANFix::Init();
    QuickMatch::Init(baseModule);
    Tweaks::SetVersion(version.c_str());
    Tweaks::Init(baseModule);
    VaultFixes::Init(baseModule);
    DisplayTest::Init(baseModule);
    Overlay::Init(baseModule, version);
    Atmosphere::Init(baseModule);
    Graphics::Init(baseModule);
    Textures::Init(baseModule);
    Borderless::Init(baseModule);
    RaceResults::Init(baseModule);
    CarLock::Init(baseModule);
    InputFocus::Init(baseModule);
    EventVote::Init();
    AutoLogin::Init(baseModule);
    LanMenu::Init(baseModule);
    CameraOrbit::Init(baseModule);
    ReShifted::Init(baseModule);

    HANDLE hThread = CreateThread(NULL, 0, RpcThread, hModule, 0, NULL);
    if (hThread) {
      Log::Debug("[OpenProStreet] RPC thread created successfully.\n");
      CloseHandle(hThread);
    } else {
      Log::Warn("Failed to create RPC thread.\n");
    }
  }

  return TRUE;
}
