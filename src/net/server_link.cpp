#include "net/server_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <monocypher-ed25519.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

#include "net/secure_client.h"

extern const std::string version;

namespace ServerLink
{
    // Import slots in nfs.exe v1.1 (base + offset). The netcode socket calls through these at
    // 0x8270E2 (send) and 0x827110 (recv), and closes the socket through closesocket.
    static const uintptr_t IAT_CLOSESOCKET = 0x567584;
    static const uintptr_t IAT_SEND = 0x56758C;
    static const uintptr_t IAT_RECV = 0x567590;

    typedef int (WSAAPI *SendFn)(SOCKET, const char*, int, int);
    typedef int (WSAAPI *RecvFn)(SOCKET, char*, int, int);
    typedef int (WSAAPI *CloseFn)(SOCKET);

    static SendFn g_realSend = nullptr;
    static RecvFn g_realRecv = nullptr;
    static CloseFn g_realClose = nullptr;
    static bool g_hooked = false;
    static uintptr_t g_baseModule = 0;

    static CRITICAL_SECTION g_lock;
    static SOCKET g_socket = INVALID_SOCKET;
    static std::unique_ptr<SecureClient> g_client;
    static std::string g_serverAddress;
    static std::string g_dataDirectory;
    static uint8_t g_identity[64];
    static bool g_hasIdentity = false;
    static int g_nextTransaction = 1;
    static bool g_quickMatchPending = false;
    static std::vector<std::pair<std::string, std::string> > g_quickMatchParams;

    class Lock {
    public:
        Lock() { EnterCriticalSection(&g_lock); }
        ~Lock() { LeaveCriticalSection(&g_lock); }
    };

    static bool RandomBytes(void* buffer, size_t size) {
        return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
    }

    static bool LoadIdentity() {
        const std::string path = g_dataDirectory + "\\identity.key";
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            const size_t got = fread(g_identity, 1, sizeof(g_identity), f);
            fclose(f);
            if (got == sizeof(g_identity)) return true;
            printf("[OpenProStreet] %s is damaged. Encrypted login is off until it's restored or deleted.\n", path.c_str());
            return false;
        }

        uint8_t seed[32];
        uint8_t publicKey[32];
        if (!RandomBytes(seed, sizeof(seed))) return false;
        crypto_ed25519_key_pair(g_identity, publicKey, seed); // wipes the seed
        f = fopen(path.c_str(), "wb");
        if (!f || fwrite(g_identity, 1, sizeof(g_identity), f) != sizeof(g_identity)) {
            if (f) fclose(f);
            printf("[OpenProStreet] Could not save your identity to %s\n", path.c_str());
            return false;
        }
        fclose(f);
        printf("[OpenProStreet] Created your identity in %s. Back this file up: it's what logs you in to your account.\n", path.c_str());
        return true;
    }

    static std::string HexKey(const uint8_t key[32]) {
        char out[65];
        for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", key[i]);
        return std::string(out, 64);
    }

    // known_servers.txt: one "address key" line per server, like SSH's known_hosts.
    static std::string FindKnownServerKey() {
        std::string found;
        FILE* f = fopen((g_dataDirectory + "\\known_servers.txt").c_str(), "r");
        if (!f) return found;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::istringstream row(line);
            std::string address, key;
            if ((row >> address >> key) && address == g_serverAddress) found = key;
        }
        fclose(f);
        return found;
    }

    static bool AcceptServerKey(const uint8_t key[32]) {
        const std::string hex = HexKey(key);
        const std::string known = FindKnownServerKey();
        if (!known.empty()) return known == hex;
        FILE* f = fopen((g_dataDirectory + "\\known_servers.txt").c_str(), "a");
        if (f) {
            fprintf(f, "%s %s\n", g_serverAddress.c_str(), hex.c_str());
            fclose(f);
        }
        printf("[OpenProStreet] First connection to %s, remembering its identity.\n", g_serverAddress.c_str());
        return true;
    }

    static std::string BuildCommand(const char* service, const char* method, const std::vector<std::pair<std::string, std::string> >& params) {
        std::ostringstream xml;
        xml << "<service name=\"" << service << "\"><transaction id=\"" << g_nextTransaction++
            << "\" type=\"TRANSACTION_TYPE_REQUEST\"><method> " << method;
        for (size_t i = 0; i < params.size(); ++i) {
            xml << "<param><name> " << params[i].first << " </name><value> " << params[i].second << " </value></param>";
        }
        xml << "</method></transaction></service>";
        return xml.str();
    }

    static void OnServerMessage(const std::string& xml) {
        if (xml.find("<error>") != std::string::npos) {
            printf("[OpenProStreet] The server refused a request: %.300s\n", xml.c_str());
        }
    }

    static void SendCommand(const char* service, const char* method, const std::vector<std::pair<std::string, std::string> >& params) {
        if (g_client) g_client->SendChannelMessage(BuildCommand(service, method, params), GetTickCount());
    }

    static int WSAAPI SendHook(SOCKET s, const char* buffer, int length, int flags) {
        {
            Lock lock;
            if (s == g_socket && g_client) {
                g_client->SendGamePacket(reinterpret_cast<const uint8_t*>(buffer), length, GetTickCount());
                return length;
            }
        }
        return g_realSend(s, buffer, length, flags);
    }

    // The game reads this socket with a blocking recv on its own thread, so waiting here for the
    // next game packet changes nothing for it. The lock isn't held while waiting.
    static int WSAAPI RecvHook(SOCKET s, char* buffer, int length, int flags) {
        for (;;) {
            bool ours;
            {
                Lock lock;
                ours = s == g_socket && g_client;
            }
            if (!ours) return g_realRecv(s, buffer, length, flags);

            char network[2048];
            const int received = g_realRecv(s, network, sizeof(network), flags);
            if (received <= 0) return received;

            uint8_t plain[2048];
            int size = 0;
            {
                Lock lock;
                if (s == g_socket && g_client) {
                    size = g_client->OnNetworkPacket(reinterpret_cast<const uint8_t*>(network), received, plain, GetTickCount());
                }
            }
            if (size > 0) {
                if (size > length) size = length;
                memcpy(buffer, plain, static_cast<size_t>(size));
                return size;
            }
        }
    }

    static int WSAAPI CloseHook(SOCKET s) {
        {
            Lock lock;
            if (s == g_socket) {
                g_socket = INVALID_SOCKET;
                g_client.reset();
            }
        }
        return g_realClose(s);
    }

    static DWORD WINAPI LinkThread(LPVOID) {
        for (;;) {
            {
                Lock lock;
                if (g_client) g_client->Tick(GetTickCount());
            }
            Sleep(50);
        }
    }

    static bool SlotHolds(uintptr_t slot, void* function) {
        return function && !IsBadReadPtr(reinterpret_cast<void*>(slot), sizeof(void*)) && *reinterpret_cast<void**>(slot) == function;
    }

    static void PatchSlot(uintptr_t slot, void* hook) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtect)) return;
        *reinterpret_cast<void**>(slot) = hook;
        VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), oldProtect, &oldProtect);
    }

    // Replaces all three slots or none, and only if they hold the expected functions, so a
    // different build of the game is left alone instead of having other imports overwritten.
    static bool InstallSocketHooks() {
        if (g_hooked) return true;
        HMODULE winsock = GetModuleHandleA("ws2_32.dll");
        if (!winsock) return false;
        void* realSend = reinterpret_cast<void*>(GetProcAddress(winsock, "send"));
        void* realRecv = reinterpret_cast<void*>(GetProcAddress(winsock, "recv"));
        void* realClose = reinterpret_cast<void*>(GetProcAddress(winsock, "closesocket"));
        if (!SlotHolds(g_baseModule + IAT_SEND, realSend) || !SlotHolds(g_baseModule + IAT_RECV, realRecv) ||
            !SlotHolds(g_baseModule + IAT_CLOSESOCKET, realClose)) {
            return false;
        }
        g_realSend = reinterpret_cast<SendFn>(realSend);
        g_realRecv = reinterpret_cast<RecvFn>(realRecv);
        g_realClose = reinterpret_cast<CloseFn>(realClose);
        PatchSlot(g_baseModule + IAT_SEND, reinterpret_cast<void*>(SendHook));
        PatchSlot(g_baseModule + IAT_RECV, reinterpret_cast<void*>(RecvHook));
        PatchSlot(g_baseModule + IAT_CLOSESOCKET, reinterpret_cast<void*>(CloseHook));
        g_hooked = true;

        HANDLE thread = CreateThread(nullptr, 0, LinkThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        printf("[OpenProStreet] Server link ready.\n");
        return true;
    }

    void Init(uintptr_t baseModule) {
        InitializeCriticalSection(&g_lock);

        const char* appData = getenv("LOCALAPPDATA");
        g_dataDirectory = std::string(appData ? appData : ".") + "\\OpenProStreet";
        CreateDirectoryA(g_dataDirectory.c_str(), nullptr);
        g_hasIdentity = LoadIdentity();
        g_baseModule = baseModule;
        if (g_hasIdentity) InstallSocketHooks();
    }

    void OnDataSocketConnected(uintptr_t socketHandle) {
        if (!g_hasIdentity) return;
        if (!InstallSocketHooks()) {
            printf("[OpenProStreet] Could not hook the game's sockets (is this the v1.1 executable?). Encrypted login is off.\n");
            return;
        }
        const SOCKET s = static_cast<SOCKET>(socketHandle);

        sockaddr_storage peer;
        int peerSize = sizeof(peer);
        char address[128] = {0};
        DWORD addressSize = sizeof(address);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0 ||
            WSAAddressToStringA(reinterpret_cast<sockaddr*>(&peer), peerSize, nullptr, address, &addressSize) != 0) {
            printf("[OpenProStreet] Could not read the server's address. Encrypted login is off for this connection.\n");
            return;
        }

        uint8_t ephemeral[32];
        if (!RandomBytes(ephemeral, sizeof(ephemeral))) return;

        Lock lock;
        g_socket = s;
        g_serverAddress = address;

        SecureClient::Callbacks callbacks;
        callbacks.send = [s](const uint8_t* data, int len) { g_realSend(s, reinterpret_cast<const char*>(data), len, 0); };
        callbacks.acceptServerKey = [](const uint8_t key[32]) { return AcceptServerKey(key); };
        callbacks.isKnownServer = []() { return !FindKnownServerKey().empty(); };
        callbacks.onMessage = [](const std::string& xml) { OnServerMessage(xml); };
        callbacks.log = [](const char* text) { printf("[OpenProStreet] %s\n", text); };

        g_client.reset(new SecureClient(g_identity, ephemeral, callbacks));
        g_client->Begin(GetTickCount());
        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("version"), version));
        SendCommand("client", "hello", params);
        if (g_quickMatchPending) SendCommand("matchmaking", "quickMatch", g_quickMatchParams);
        printf("[OpenProStreet] Connecting to %s\n", address);
    }

    bool IsConnected() {
        if (!g_hooked) return false;
        Lock lock;
        return g_client && g_client->GetState() == SecureClient::State::Established;
    }

    static std::string FormatFloat(float value) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.3f", value);
        return buffer;
    }

    void SubmitRaceResult(const RaceReport& report) {
        if (!g_hooked) return;

        std::string laps;
        for (size_t i = 0; i < report.lapTimes.size(); ++i) {
            if (i != 0) laps += ",";
            laps += FormatFloat(report.lapTimes[i]);
        }

        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("track_key"), report.trackKey));
        params.push_back(std::make_pair(std::string("track_name"), report.trackName));
        params.push_back(std::make_pair(std::string("race_mode"), report.raceMode));
        params.push_back(std::make_pair(std::string("car_key"), report.carKey));
        params.push_back(std::make_pair(std::string("position"), std::to_string(report.position)));
        params.push_back(std::make_pair(std::string("race_time"), FormatFloat(report.raceTime)));
        params.push_back(std::make_pair(std::string("best_lap"), FormatFloat(report.bestLap)));
        params.push_back(std::make_pair(std::string("score"), FormatFloat(report.score)));
        params.push_back(std::make_pair(std::string("laps"), std::to_string(report.lapsCompleted)));
        params.push_back(std::make_pair(std::string("lap_times"), laps));

        Lock lock;
        if (g_client && g_client->GetState() == SecureClient::State::Established) {
            SendCommand("results", "submitResult", params);
        }
    }

    void RequestQuickMatch(bool ranked, const std::string& location, int difficulty, const std::string& events) {
        if (!g_hasIdentity) return;
        Lock lock;
        g_quickMatchPending = true;
        g_quickMatchParams.clear();
        g_quickMatchParams.push_back(std::make_pair(std::string("ranked"), std::string(ranked ? "1" : "0")));
        if (!events.empty()) {
            g_quickMatchParams.push_back(std::make_pair(std::string("location"), location));
            g_quickMatchParams.push_back(std::make_pair(std::string("difficulty"), std::to_string(difficulty)));
            g_quickMatchParams.push_back(std::make_pair(std::string("events"), events));
        }
        SendCommand("matchmaking", "quickMatch", g_quickMatchParams);
    }

    void CancelQuickMatch() {
        if (!g_hasIdentity) return;
        Lock lock;
        g_quickMatchPending = false;
        SendCommand("matchmaking", "cancelQuickMatch", std::vector<std::pair<std::string, std::string> >());
    }
}
