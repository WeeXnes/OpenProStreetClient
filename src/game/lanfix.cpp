#include "game/lanfix.h"
#include "game/hooks.h"
#include "net/server_link.h"
#include "log.h"
#include <windows.h>
#include <string>
#include <vector>
#include <regex>
#include <cstdio>

namespace LANFix
{
    uintptr_t ptrDataConnection = 0x827190;
    uint32_t AddrFamily = 2;

    // netcodeConnectionSystemImpl in nfs.exe v1.1, as offsets from the image base.
    static const uintptr_t CONNECT_TO_SERVER = 0x41FCB0; // 0x81FCB0: connects to an address and port
    static const uintptr_t FIND_LAN_SERVER = 0x41FF00;   // 0x81FF00: broadcasts for a server on the LAN

    static const char* const DEFAULT_ADDRESS = "weexnes.dev";
    static const int DEFAULT_PORT = 10104;

    typedef void (__attribute__((thiscall)) *ConnectToServerFn)(void*, const char*, const char*);
    typedef void (__attribute__((thiscall)) *FindLanServerFn)(void*);

    // The game keeps pointers to these until the connection is made, so they stay allocated.
    static std::string g_serverAddress = DEFAULT_ADDRESS;
    static std::string g_serverPort = std::to_string(DEFAULT_PORT);
    static uintptr_t g_baseModule = 0;
    static void* g_origFindLanServer = nullptr;

    uint32_t CheckAddressFamily(const char* addr)
    {
        if (!addr || IsBadReadPtr((void*)addr, 1)) {
            return 2;
        }
        try {
            std::string strAddr(addr);
            if (std::regex_match(strAddr, std::regex(R"(([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|:((:[0-9a-fA-F]{1,4}){1,7}|:)|fe80:(:[0-9a-fA-F]{0,4}){0,4}%[0-9a-zA-Z]{1,}|::(ffff(:0{1,4}){0,1}:){0,1}((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.)\{3,3\}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])|([0-9a-fA-F]{1,4}:){1,4}:((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.)\{3,3\}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9]))")))
            {
                return 23;
            }
        } catch (...) {

        }

        return 2;
    }

    bool __attribute__((thiscall)) DataConnectionHook(void* that, char* addr, void* unk)
    {
        AddrFamily = CheckAddressFamily(addr);
        Log::Debug("[OpenProStreet] LANFix DataConnectionHook: addr='%s' -> AddrFamily=%u (%s)\n", addr ? addr : "NULL", AddrFamily, AddrFamily == 23 ? "AF_INET6 (IPv6)" : "AF_INET (IPv4)");
        const bool connected = reinterpret_cast<bool(__attribute__((thiscall))*)(void*, char*, void*)>(ptrDataConnection)(that, addr, unk);
        if (connected) {
            // netcodeWinsockPlatUDPSocket +0x04 is the socket (0x8271DA).
            ServerLink::OnDataSocketConnected(*reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(that) + 4));
        }
        return connected;
    }

    // Opening the LAN menu, the game broadcasts for a server, and only shows the address screen if
    // none answers. A broadcast can't reach a server over the internet or a VPN. With an address in
    // the settings, this connects there instead, so the game carries on as if the search found it.
    //
    // This is netcodeConnectionSystemImpl::BeginConnect, which the game calls again for each later
    // step of connecting. Only the first call, in state 0 (+0x04), starts a search. Later ones move
    // the handshake on (0x81FFDB), and connecting again there fails with "ConnectionNotReady".
    static void __attribute__((thiscall)) FindLanServerHook(void* connectionSystem)
    {
        const int state = *reinterpret_cast<int*>(static_cast<char*>(connectionSystem) + 4);
        if (g_serverAddress.empty() || state != 0) {
            reinterpret_cast<FindLanServerFn>(g_origFindLanServer)(connectionSystem);
            return;
        }
        Log::Event(Log::Tag::Server, "Connecting to %s:%s\n", Log::Highlight(g_serverAddress).c_str(), g_serverPort.c_str());
        reinterpret_cast<ConnectToServerFn>(g_baseModule + CONNECT_TO_SERVER)(connectionSystem, g_serverAddress.c_str(), g_serverPort.c_str());
    }

    static void LoadServerSettings()
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty()) return;

        // An Address line left empty is different from no line: it turns the default off.
        char address[256] = {};
        GetPrivateProfileStringA("Server", "Address", DEFAULT_ADDRESS, address, sizeof(address), settings.c_str());
        g_serverAddress = address;

        int port = GetPrivateProfileIntA("Server", "Port", DEFAULT_PORT, settings.c_str());
        if (port < 1 || port > 65535) {
            Log::Warn("Port %d under [Server] isn't valid, using %d.\n", port, DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
        g_serverPort = std::to_string(port);
    }

    uintptr_t FindPattern(uintptr_t startAddr, size_t searchSize, const std::string& pattern)
    {
        std::vector<int> bytes;
        for (size_t i = 0; i < pattern.length(); ) {
            if (pattern[i] == ' ') { i++; continue; }
            if (pattern[i] == '?') {
                bytes.push_back(-1);
                i += (i + 1 < pattern.length() && pattern[i + 1] == '?') ? 2 : 1;
            } else {
                bytes.push_back(std::stoi(pattern.substr(i, 2), nullptr, 16));
                i += 2;
            }
        }

        uintptr_t currAddr = startAddr;
        uintptr_t endAddr = startAddr + searchSize;
        MEMORY_BASIC_INFORMATION mbi;

        while (currAddr < endAddr && VirtualQuery((void*)currAddr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE))) {
                uint8_t* p = (uint8_t*)currAddr;
                size_t regionSize = mbi.RegionSize - (currAddr - (uintptr_t)mbi.BaseAddress);
                if (currAddr + regionSize > endAddr) regionSize = endAddr - currAddr;

                for (size_t i = 0; i + bytes.size() <= regionSize; ++i) {
                    bool found = true;
                    for (size_t j = 0; j < bytes.size(); ++j) {
                        if (bytes[j] != -1 && p[i + j] != (uint8_t)bytes[j]) {
                            found = false;
                            break;
                        }
                    }
                    if (found) return (uintptr_t)(p + i);
                }
            }
            currAddr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
        return 0;
    }

    void PatchCall(uintptr_t callAddr, uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)callAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(uint8_t*)callAddr = 0xE8;
        *(int32_t*)(callAddr + 1) = (int32_t)(targetAddr - (callAddr + 5));
        VirtualProtect((void*)callAddr, 5, oldProtect, &oldProtect);
    }

    void InstallHook1(uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)targetAddr, 12, PAGE_EXECUTE_READWRITE, &oldProtect);

        uint8_t* p = (uint8_t*)targetAddr;
        p[0] = 0xA1;
        *(uint32_t*)(p + 1) = (uint32_t)&AddrFamily;
        p[5] = 0x89;
        p[6] = 0x44;
        p[7] = 0x24;
        p[8] = 0x10;
        p[9] = 0x90;
        p[10] = 0x90;
        p[11] = 0x90;

        VirtualProtect((void*)targetAddr, 12, oldProtect, &oldProtect);
    }

    void InstallHook2(uintptr_t targetAddr)
    {
        DWORD oldProtect;
        VirtualProtect((void*)targetAddr, 40, PAGE_EXECUTE_READWRITE, &oldProtect);

        uint8_t code[40] = {
            0x50,
            0x33, 0xC0,
            0x89, 0x44, 0x24, 0x14,
            0x89, 0x44, 0x24, 0x24,
            0x89, 0x44, 0x24, 0x28,
            0x89, 0x44, 0x24, 0x2C,
            0x89, 0x44, 0x24, 0x30,
            0x6A, 0x02,
            0x58,
            0x89, 0x44, 0x24, 0x1C,
            0x6A, 0x11,
            0x58,
            0x89, 0x44, 0x24, 0x20,
            0x58,
            0x90, 0x90
        };
        memcpy((void*)targetAddr, code, sizeof(code));

        VirtualProtect((void*)targetAddr, 40, oldProtect, &oldProtect);
    }

    void Init()
    {
        uintptr_t baseModule = (uintptr_t)GetModuleHandle(NULL);
        if (!baseModule) return;

        Log::Debug("[OpenProStreet] Initializing Win11 LAN Fix...\n");

        ptrDataConnection = FindPattern(baseModule, 0x800000, "83 EC 20 33 C0 56 6A 11 6A 02 89 44 24 10");
        if (!ptrDataConnection) {
            ptrDataConnection = baseModule + 0x427190;
        }

        uintptr_t loc_823C54 = FindPattern(baseModule, 0x800000, "C7 44 24 1C FF FF FF FF 89 46 04 E8 ? ? ? ? 84 C0 0F 84");
        if (loc_823C54) {
            loc_823C54 += 0xB;
        } else {
            loc_823C54 = baseModule + 0x423C54;
        }

        uintptr_t loc_82719A = ptrDataConnection + 0xA;
        uintptr_t loc_8271A9 = ptrDataConnection + 0x19;

        InstallHook1(loc_82719A);
        InstallHook2(loc_8271A9);
        PatchCall(loc_823C54, (uintptr_t)DataConnectionHook);

        Log::Debug("[OpenProStreet] Win11 LAN Fix applied successfully! (ptrDataConnection: 0x%p, hook Call: 0x%p)\n", (void*)ptrDataConnection, (void*)loc_823C54);
        Log::Summary(Log::Tag::Client, "Windows 11 LAN fix active\n");

        g_baseModule = baseModule;
        LoadServerSettings();
        if (!Hooks::InstallDetour((void*)(baseModule + FIND_LAN_SERVER), (void*)FindLanServerHook, &g_origFindLanServer)) {
            Log::Error("Could not install the server address hook.\n");
        } else if (g_serverAddress.empty()) {
            Log::Event(Log::Tag::Client, "No server set: searching the LAN, then asking for an address\n");
        } else {
            Log::Event(Log::Tag::Client, "Server set to %s:%s, skipping the address screen\n", Log::Highlight(g_serverAddress).c_str(), g_serverPort.c_str());
        }
    }
}
