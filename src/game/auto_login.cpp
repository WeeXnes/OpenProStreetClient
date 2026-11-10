#include "game/auto_login.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace AutoLogin
{
    // FELanStateManager in nfs.exe v1.1, as offsets from the image base.
    //
    // Changing state stores the new one at +0x04 and calls vtable slot 80 (0x9A2BE4, 0x7D2180),
    // which sets up the state's screen. State 6 is the account screen: "create account" (state
    // 0x0F) or "log in" (state 7). On the log in screen, its button calls 0x7D2830 with the name and
    // password fields; that shows "logging in" (state 8) and starts netcodeLoginSystem::BeginLogin.
    // The login system's answer moves the manager to 0x14 when it worked, or to one of the error
    // messages, 0x0A to 0x0D, when it didn't.
    static const uintptr_t STATE_ENTERED_SLOT = 0x5A2BE4; // 0x9A2BE4
    static const uintptr_t STATE_ENTERED = 0x3D2180;      // 0x7D2180
    static const uintptr_t LAN_LOGIN = 0x3D2830;          // 0x7D2830

    static const int STATE_ACCOUNT_SCREEN = 6;
    static const int STATE_LOGGED_IN = 0x14;
    static const int STATE_LOGIN_ERROR_FIRST = 0x0A;
    static const int STATE_LOGIN_ERROR_LAST = 0x0D;

    // What the launcher's DPAPI-protected passwords start with. Anything else is the password itself.
    static const char* const PROTECTED_PREFIX = "dpapi:";
    static const char* const ENTROPY = "OpenProStreet login";

    typedef void (__attribute__((thiscall)) *StateEnteredFn)(void*);
    typedef void (__attribute__((thiscall)) *LanLoginFn)(void*, const char*, const char*);

    static StateEnteredFn g_stateEntered = nullptr;
    static LanLoginFn g_lanLogin = nullptr;

    // The game keeps the pointers it's given, so these stay allocated.
    static std::string g_username;
    static std::string g_password;

    static bool g_waiting = false; // a login this sent hasn't been answered yet
    static bool g_gaveUp = false;  // it failed, so the player logs in by hand until one works

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

    static void __attribute__((thiscall)) StateEnteredHook(void* manager)
    {
        g_stateEntered(manager);

        const int state = *reinterpret_cast<int*>(static_cast<char*>(manager) + 4);
        if (state == STATE_LOGGED_IN)
        {
            g_waiting = false;
            g_gaveUp = false;
            return;
        }
        if (state >= STATE_LOGIN_ERROR_FIRST && state <= STATE_LOGIN_ERROR_LAST)
        {
            if (g_waiting)
            {
                g_waiting = false;
                g_gaveUp = true;
                Log::Warn("Automatic login as %s didn't work. Log in by hand, or check the name and password in the launcher.\n",
                          g_username.c_str());
            }
            return;
        }
        if (state != STATE_ACCOUNT_SCREEN || g_gaveUp || g_waiting) return;

        g_waiting = true;
        Log::Event(Log::Tag::Client, "Logging in automatically as %s\n", Log::Highlight(g_username).c_str());
        g_lanLogin(manager, g_username.c_str(), g_password.c_str());
    }

    static bool PatchPointer(uintptr_t slot, uintptr_t expected, uintptr_t value)
    {
        uintptr_t* p = reinterpret_cast<uintptr_t*>(slot);
        if (IsBadReadPtr(p, sizeof(*p)) || *p != expected) return false;
        DWORD oldProtect;
        if (!VirtualProtect(p, sizeof(*p), PAGE_READWRITE, &oldProtect)) return false;
        *p = value;
        VirtualProtect(p, sizeof(*p), oldProtect, &oldProtect);
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

        g_lanLogin = reinterpret_cast<LanLoginFn>(baseModule + LAN_LOGIN);
        g_stateEntered = reinterpret_cast<StateEnteredFn>(baseModule + STATE_ENTERED);
        if (!PatchPointer(baseModule + STATE_ENTERED_SLOT, baseModule + STATE_ENTERED, reinterpret_cast<uintptr_t>(StateEnteredHook)))
        {
            Log::Warn("Automatic login is off: the LAN menu isn't what the client expects.\n");
            return;
        }
        Log::Event(Log::Tag::Client, "Automatic login as %s is on\n", Log::Highlight(g_username).c_str());
    }
}
