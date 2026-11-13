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
    // in" button (0x7E13E5) opens the log in screen with 0x5C4320(manager, *0xA894FC, 7, 1), from the
    // call at 0x7E13F2. That screen's OK button (0x7E1447) passes the typed name and password to
    // 0x7D2830, which only shows "logging in" (state 8, 0x5C43E0) and starts
    // netcodeLoginSystem::BeginLogin. The answer moves the manager to 0x14 when the login worked, or
    // to one of the error messages, 0x0A to 0x0D, when it didn't.
    //
    // With automatic login on, the "log in" button's call goes to LogInPressed instead, which calls
    // 0x7D2830 with the saved account. The player still presses "log in" themselves, so it runs from
    // the button, with the account screen showing, as the OK button's call does. Nothing waits for
    // a screen or presses anything: earlier versions pressed the buttons for the player, which
    // raced the screens' fades. The log in screen is never opened, and everything that reads it
    // (0x7D21FC, 0x7E1447) checks that it exists first.
    static const uintptr_t LOG_IN_BUTTON_CALL = 0x3E13F2; // 0x7E13F2: call 0x5C4320
    static const uintptr_t CHANGE_SCREEN = 0x1C4320;      // 0x5C4320
    static const uintptr_t LAN_LOGIN = 0x3D2830;          // 0x7D2830

    // The instructions before the call: push 1 / push 7 / push ecx / mov ecx,esi.
    static const uint8_t LOG_IN_BUTTON_ARGS[] = { 0x6A, 0x01, 0x6A, 0x07, 0x51, 0x8B, 0xCE };

    static const int STATE_ACCOUNT_SCREEN = 6;
    static const int STATE_LOG_IN_SCREEN = 7;
    static const int STATE_LOGGED_IN = 0x14;
    // 0x7D2870 turns the login system's error into a message: 0x0B for dbUnknownPlayer, 0x0C for
    // dbIncorrectPassword, 0x0D for dbPlayerAlreadyLoggedIn, and 0x0A for anything else.
    static const int STATE_LOGIN_ERROR_FIRST = 0x0A;
    static const int STATE_LOGIN_ERROR_LAST = 0x0D;
    static const int STATE_UNKNOWN_PLAYER = 0x0B;
    static const int STATE_WRONG_PASSWORD = 0x0C;

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

    static bool g_waiting = false; // a login this sent hasn't been answered yet
    static bool g_gaveUp = false;  // the saved account is wrong, so "log in" opens the log in screen until a login works

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

    // Replaces the "log in" button's screen change. Anything else it might be asked for goes on as before.
    static void __attribute__((thiscall)) LogInPressed(void* manager, const char* screen, int state, int popup)
    {
        if (g_on && !g_gaveUp && state == STATE_LOG_IN_SCREEN)
        {
            g_waiting = true;
            Log::Event(Log::Tag::Client, "Logging in as %s\n", Log::Highlight(g_username).c_str());
            reinterpret_cast<LanLoginFn>(g_baseModule + LAN_LOGIN)(manager, g_username.c_str(), g_password.c_str());
            return;
        }
        reinterpret_cast<ChangeScreenFn>(g_baseModule + CHANGE_SCREEN)(manager, screen, state, popup);
    }

    void OnStateEntered(void* manager)
    {
        if (!g_on) return;
        const int state = LanMenu::State(manager);
        if (state < STATE_ACCOUNT_SCREEN) g_waiting = false; // connecting again: that login is gone
        else if (state == STATE_LOGGED_IN)
        {
            if (g_waiting) Log::Debug("[OpenProStreet] Automatic login: logged in.\n");
            g_waiting = false;
            g_gaveUp = false;
        }
        else if (state >= STATE_LOGIN_ERROR_FIRST && state <= STATE_LOGIN_ERROR_LAST && g_waiting)
        {
            g_waiting = false;
            // Only a wrong name or password sends the player to the log in screen. Anything else,
            // like the server still holding an earlier login, is tried again on the next press.
            if (state == STATE_UNKNOWN_PLAYER || state == STATE_WRONG_PASSWORD)
            {
                g_gaveUp = true;
                Log::Warn("The saved name or password for %s is wrong. Press \"log in\" again to type them, or fix them in the launcher.\n",
                          g_username.c_str());
            }
            else
            {
                Log::Warn("Logging in as %s didn't work this time. Press \"log in\" to try again.\n", g_username.c_str());
            }
        }
    }

    // Points the "log in" button's call at LogInPressed, if it's the call v1.1 has there.
    static bool PatchLogInButton()
    {
        uint8_t* call = reinterpret_cast<uint8_t*>(g_baseModule + LOG_IN_BUTTON_CALL);
        if (memcmp(call - sizeof(LOG_IN_BUTTON_ARGS), LOG_IN_BUTTON_ARGS, sizeof(LOG_IN_BUTTON_ARGS)) != 0 || call[0] != 0xE8) return false;
        int32_t rel;
        memcpy(&rel, call + 1, 4);
        if (reinterpret_cast<uintptr_t>(call) + 5 + rel != g_baseModule + CHANGE_SCREEN) return false;

        DWORD oldProtect;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LogInPressed) - (reinterpret_cast<uintptr_t>(call) + 5));
        memcpy(call + 1, &hookRel, 4);
        VirtualProtect(call + 1, 4, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        return true;
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
        if (!PatchLogInButton())
        {
            Log::Warn("Automatic login is off: the account screen's \"log in\" button isn't where it's expected.\n");
            return;
        }
        g_on = true;
        Log::Event(Log::Tag::Client, "Automatic login as %s is on: press \"log in\" and the saved account is used\n",
                   Log::Highlight(g_username).c_str());
    }
}
