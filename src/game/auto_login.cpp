#include "game/auto_login.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <string>
#include <vector>

#include "game/lan_menu.h"
#include "log.h"

namespace AutoLogin
{
    // FELanStateManager in nfs.exe v1.1, as offsets from the image base.
    //
    // State 6 is the account screen: "create account" (state 0x0F) or "log in" (state 7). Its "log
    // in" button (0x7E13E5) changes to the log in screen with 0x5C4320(manager, *0xA894FC, 7, 1).
    // That screen sets 0xB32B5C while it exists, and its OK button (0x7E1447) calls 0x7D2830 with
    // the name and password fields: "logging in" (state 8), then netcodeLoginSystem::BeginLogin.
    // The login system's answer moves the manager to 0x14 when it worked, or to one of the error
    // messages, 0x0A to 0x0D, when it didn't.
    //
    // Version 9.7 called 0x7D2830 as state 6 was entered, before the account screen was loaded. That
    // started a second screen change inside the first one, and could leave the "logging in" popup
    // over the lobby, holding the input. So this presses the same two buttons the player would, each
    // once its screen is showing and nothing else is changing.
    static const uintptr_t CHANGE_SCREEN = 0x1C4320;   // 0x5C4320
    static const uintptr_t LOG_IN_SCREEN = 0x6894FC;   // 0xA894FC: the log in screen's package name
    static const uintptr_t LOG_IN_SCREEN_OBJECT = 0x732B5C; // 0xB32B5C
    static const uintptr_t LAN_LOGIN = 0x3D2830;       // 0x7D2830

    static const int STATE_ACCOUNT_SCREEN = 6;
    static const int STATE_LOG_IN_SCREEN = 7;
    static const int STATE_LOGGED_IN = 0x14;
    static const int STATE_LOGIN_ERROR_FIRST = 0x0A;
    static const int STATE_LOGIN_ERROR_LAST = 0x0D;

    // What the launcher's DPAPI-protected passwords start with. Anything else is the password itself.
    static const char* const PROTECTED_PREFIX = "dpapi:";
    static const char* const ENTROPY = "OpenProStreet login";

    typedef void (__attribute__((thiscall)) *ChangeScreenFn)(void*, const char*, int, int);
    typedef void (__attribute__((thiscall)) *LanLoginFn)(void*, const char*, const char*);

    static uintptr_t g_baseModule = 0;
    static bool g_on = false;

    // The game keeps the pointers it's given, so these stay allocated.
    static std::string g_username;
    static std::string g_password;

    static bool g_pressedLogIn = true;  // on this visit to the account screen; reset on arriving there
    static bool g_pressedOk = false;    // on this visit to the log in screen
    static bool g_waiting = false;      // a login this sent hasn't been answered yet
    static bool g_gaveUp = false;       // it failed, so the player logs in by hand until one works

    static std::vector<unsigned char> FromBase64(const std::string& text)
    {
        DWORD size = 0;
        if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return {};
        std::vector<unsigned char> bytes(size);
        if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, bytes.data(), &size, nullptr, nullptr)) return {};
        bytes.resize(size);
        return bytes;
    }

    // The launcher on Windows protects the password with DPAPI for the Windows user, as the ini
    // sits in the game folder where other users can read it. Only that user can turn it back.
    static bool Unprotect(const std::string& stored, std::string& password)
    {
        if (stored.compare(0, strlen(PROTECTED_PREFIX), PROTECTED_PREFIX) != 0)
        {
            password = stored;
            return true;
        }
        std::vector<unsigned char> sealed = FromBase64(stored.substr(strlen(PROTECTED_PREFIX)));
        if (sealed.empty()) return false;
        DATA_BLOB in = { static_cast<DWORD>(sealed.size()), sealed.data() };
        DATA_BLOB entropy = { static_cast<DWORD>(strlen(ENTROPY)), reinterpret_cast<BYTE*>(const_cast<char*>(ENTROPY)) };
        DATA_BLOB out = {};
        if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        password.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return true;
    }

    bool IsOn()
    {
        return g_on;
    }

    // Only notes where the menus are. Nothing is pressed from here: the state's screen isn't loaded yet.
    void OnStateEntered(void* manager)
    {
        if (!g_on) return;
        const int state = LanMenu::State(manager);
        // Only on arriving from connecting (states 0 to 5). A player who goes back from the log in
        // or create account screen wants the account screen, not to be sent on again.
        if (state == STATE_ACCOUNT_SCREEN)
        {
            if (LanMenu::PreviousState(manager) < STATE_ACCOUNT_SCREEN) g_pressedLogIn = false;
        }
        else if (state < STATE_ACCOUNT_SCREEN) g_waiting = false; // connecting again: that login is gone
        else if (state == STATE_LOG_IN_SCREEN) g_pressedOk = false;
        else if (state == STATE_LOGGED_IN)
        {
            if (g_waiting) Log::Debug("[OpenProStreet] Automatic login: logged in.\n");
            g_waiting = false;
            g_gaveUp = false;
        }
        else if (state >= STATE_LOGIN_ERROR_FIRST && state <= STATE_LOGIN_ERROR_LAST && g_waiting)
        {
            g_waiting = false;
            g_gaveUp = true;
            Log::Warn("Automatic login as %s didn't work. Log in by hand, or check the name and password in the launcher.\n",
                      g_username.c_str());
        }
    }

    void OnScreenShown(void* manager)
    {
        if (!g_on || g_gaveUp || g_waiting || !LanMenu::IsSettled(manager)) return;
        const int state = LanMenu::State(manager);

        if (state == STATE_ACCOUNT_SCREEN && !g_pressedLogIn)
        {
            const char* screen = *reinterpret_cast<const char**>(g_baseModule + LOG_IN_SCREEN);
            if (!screen) return;
            g_pressedLogIn = true;
            Log::Debug("[OpenProStreet] Automatic login: opening the log in screen.\n");
            reinterpret_cast<ChangeScreenFn>(g_baseModule + CHANGE_SCREEN)(manager, screen, STATE_LOG_IN_SCREEN, 1);
        }
        else if (state == STATE_LOG_IN_SCREEN && !g_pressedOk)
        {
            if (!*reinterpret_cast<void**>(g_baseModule + LOG_IN_SCREEN_OBJECT)) return;
            g_pressedOk = true;
            g_waiting = true;
            Log::Event(Log::Tag::Client, "Logging in automatically as %s\n", Log::Highlight(g_username).c_str());
            reinterpret_cast<LanLoginFn>(g_baseModule + LAN_LOGIN)(manager, g_username.c_str(), g_password.c_str());
        }
    }

    void Init(uintptr_t baseModule)
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty() || GetPrivateProfileIntA("Account", "AutoLogin", 0, settings.c_str()) == 0) return;

        char username[128] = {};
        char stored[1024] = {};
        GetPrivateProfileStringA("Account", "Username", "", username, sizeof(username), settings.c_str());
        GetPrivateProfileStringA("Account", "Password", "", stored, sizeof(stored), settings.c_str());
        g_username = username;
        if (g_username.empty() || stored[0] == '\0')
        {
            Log::Warn("Automatic login is on, but the name or password under [Account] is missing.\n");
            return;
        }
        if (!Unprotect(stored, g_password))
        {
            Log::Warn("Couldn't read the saved password. Save it again in the launcher, as the same Windows user who plays.\n");
            SecureZeroMemory(stored, sizeof(stored));
            return;
        }
        SecureZeroMemory(stored, sizeof(stored));

        g_baseModule = baseModule;
        g_on = true; // it acts through LanMenu's hooks, which warn if they can't be installed
        Log::Event(Log::Tag::Client, "Automatic login as %s is on\n", Log::Highlight(g_username).c_str());
    }
}
