#include "net/server_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <monocypher-ed25519.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

#include "net/secure_client.h"
#include "game/atmosphere.h"
#include "game/car_lock.h"
#include "game/overlay.h"
#include "game/quick_match.h"
#include "game/event_vote.h"
#include "log.h"

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
    static CRITICAL_SECTION g_pingLock;
    static bool g_pingLockReady = false;
    static constexpr DWORD PING_INTERVAL_MS = 2000;
    static constexpr float MAX_CAR_LEAD_SECONDS = 0.120f;
    static uint32_t g_pingToken = 0;
    static uint64_t g_pendingPingSentUs = 0;
    static DWORD g_lastPingSentTick = 0;
    static bool g_pingSendNow = false;
    static double g_rttSmoothedMs = -1.0;
    static std::map<uint32_t, float> g_carLeadSeconds;

    static uint64_t NowMicros() {
        static LARGE_INTEGER freq = {};
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return freq.QuadPart ? static_cast<uint64_t>((now.QuadPart * 1000000ULL) / freq.QuadPart) : 0;
    }

    static void ResetLatencyState() {
        if (!g_pingLockReady) return;
        EnterCriticalSection(&g_pingLock);
        g_pingToken = 0;
        g_pendingPingSentUs = 0;
        g_lastPingSentTick = 0;
        g_pingSendNow = false;
        g_rttSmoothedMs = -1.0;
        g_carLeadSeconds.clear();
        LeaveCriticalSection(&g_pingLock);
    }

    static SOCKET g_socket = INVALID_SOCKET;
    static std::unique_ptr<SecureClient> g_client;
    static bool g_helloRejected = false;
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
            Log::Error("%s is damaged. Encrypted login is off until it's restored or deleted.\n", path.c_str());
            return false;
        }

        uint8_t seed[32];
        uint8_t publicKey[32];
        if (!RandomBytes(seed, sizeof(seed))) return false;
        crypto_ed25519_key_pair(g_identity, publicKey, seed); // wipes the seed
        f = fopen(path.c_str(), "wb");
        if (!f || fwrite(g_identity, 1, sizeof(g_identity), f) != sizeof(g_identity)) {
            if (f) fclose(f);
            Log::Error("Could not save your identity to %s\n", path.c_str());
            return false;
        }
        fclose(f);
        Log::Event(Log::Tag::Client, "Created your identity in %s. Back this file up: it's what logs you in to your account.\n", path.c_str());
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

    // A parameter as the server writes it: "<name> NAME </name><value> VALUE </value>".
    static std::string ParamValue(const std::string& xml, const std::string& name) {
        const std::string start = "<name> " + name + " </name><value>";
        const size_t at = xml.find(start);
        if (at == std::string::npos) return "";
        const size_t from = at + start.size();
        const size_t to = xml.find("</value>", from);
        if (to == std::string::npos) return "";
        std::string value = xml.substr(from, to - from);
        const size_t first = value.find_first_not_of(' ');
        const size_t last = value.find_last_not_of(' ');
        return first == std::string::npos ? "" : value.substr(first, last - first + 1);
    }

    static std::string ErrorValue(const std::string& xml) {
        const std::string open = "<error>";
        const size_t pos = xml.find(open);
        if (pos == std::string::npos) return "";
        const size_t start = pos + open.size();
        const size_t end = xml.find("</error>", start);
        if (end == std::string::npos) return "";
        const std::string value = xml.substr(start, end - start);
        const size_t first = value.find_first_not_of(' ');
        const size_t last = value.find_last_not_of(' ');
        return first == std::string::npos ? "" : value.substr(first, last - first + 1);
    }

    static void OnServerMessage(const std::string& xml) {
        if (xml.find("<error>") != std::string::npos) {
            const std::string err = ErrorValue(xml);
            if (xml.find("<method> hello") != std::string::npos && err == "clientUpdateRequired") {
                const std::string required = ParamValue(xml, "required");
                g_helloRejected = true;
                Log::Error("Connection refused: client v%s is outdated (server requires v%s).\n",
                           Log::Highlight(version).c_str(),
                           Log::Highlight(required.empty() ? "latest" : required).c_str());
                std::vector<std::string> lines;
                lines.push_back(required.empty()
                                    ? ("Your client is v" + version + " (update required).")
                                    : ("Your client is v" + version + " (need v" + required + ")."));
                lines.push_back("Connection refused. Update to the latest version.");
                Overlay::OnConnectionRefused("Client update required", lines);
                return;
            }
            Log::Warn("The server refused a request: %s\n", Log::Highlight(xml.substr(0, 300)).c_str());
            return;
        }
        // The server answered the client's hello: the encrypted connection works.
        if (xml.find("<method> hello") != std::string::npos && xml.find("TRANSACTION_TYPE_REPLY") != std::string::npos) {
            g_helloRejected = false;
            Overlay::OnConnected();
        }
        // Sent on joining a room: the race day's allowed cars, or nothing for every car.
        if (xml.find("<method> allowedCars") != std::string::npos) {
            CarLock::SetAllowedCars(ParamValue(xml, "cars"));
        }
        // Who is in the room, which car each picked, and their lobby state.
        if (xml.find("<method> lobbyCars") != std::string::npos) {
            std::vector<Overlay::LobbyPlayer> players;
            const int count = atoi(ParamValue(xml, "cars").c_str());
            const std::string statuses = ParamValue(xml, "st");
            for (int i = 0; i < count && i < 8; ++i) {
                const std::string packed = ParamValue(xml, "c" + std::to_string(i));
                const size_t bar = packed.find('|');
                if (bar == std::string::npos) continue;
                Overlay::LobbyPlayer p;
                p.name = packed.substr(0, bar);
                p.car = packed.substr(bar + 1);
                const char code = static_cast<size_t>(i) < statuses.size() ? statuses[static_cast<size_t>(i)] : '\0';
                if (code == 'b') p.status = "blocked";
                else if (code == 'g') p.status = "racing";
                else if (code == 'p') p.status = "picking";
                else if (code == 'a') p.status = "menus";
                else if (code == 'r') p.status = "ready";
                else p.status = p.car.empty() ? "picking" : "ready";
                players.push_back(p);
            }
            const int ev = atoi(ParamValue(xml, "ev").c_str());
            const int evs = atoi(ParamValue(xml, "evs").c_str());
            Overlay::SetLobbyState(ParamValue(xml, "room"), ParamValue(xml, "atm"), ev, evs, players);
            if (count == 0) {
                EventVote::Reset();
                Overlay::ResetRoomState();
            }
        }
        if (xml.find("<method> lobbyTimer") != std::string::npos) {
            Overlay::LobbyTimerState t;
            const std::string sec = ParamValue(xml, "sec");
            t.seconds = sec.empty() ? -1 : atoi(sec.c_str());
            t.paused = atoi(ParamValue(xml, "paused").c_str()) != 0;
            t.reason = ParamValue(xml, "why");
            t.who = ParamValue(xml, "who");
            t.ready = atoi(ParamValue(xml, "ready").c_str());
            t.total = atoi(ParamValue(xml, "total").c_str());
            t.allReadyIn = atoi(ParamValue(xml, "skip").c_str());
            Overlay::SetLobbyTimer(t);
        }
        // The room's race day events and the next one, for the event vote window.
        if (xml.find("<method> raceDayEvents") != std::string::npos) {
            std::vector<std::string> names;
            const int count = atoi(ParamValue(xml, "events").c_str());
            for (int i = 0; i < count && i < 8; ++i) names.push_back(ParamValue(xml, "e" + std::to_string(i)));
            EventVote::SetEvents(names, atoi(ParamValue(xml, "current").c_str()));
        }
        // An event vote in the room: started, update, ended, or refused (to the caller).
        if (xml.find("<method> eventVote") != std::string::npos) {
            EventVote::OnVote(ParamValue(xml, "type"), ParamValue(xml, "caller"), atoi(ParamValue(xml, "index").c_str()),
                              ParamValue(xml, "event"), atoi(ParamValue(xml, "yes").c_str()), atoi(ParamValue(xml, "no").c_str()),
                              atoi(ParamValue(xml, "total").c_str()), atoi(ParamValue(xml, "seconds").c_str()),
                              ParamValue(xml, "passed") == "1", ParamValue(xml, "reason"));
        }
        // Sent on joining and leaving a room: the race day's time of day, or nothing for the player's own.
        if (xml.find("<method> atmosphere") != std::string::npos) {
            Atmosphere::SetFromRoom(ParamValue(xml, "look"));
        }
        // During a quick match: the race day the server picked or prepared, or nothing yet.
        if (xml.find("<method> quickMatchRoom") != std::string::npos) {
            QuickMatch::SetServerRoom(ParamValue(xml, "gamename"));
        }
        // What a race meant for the player: a notification in the overlay.
        if (xml.find("<method> notice") != std::string::npos) {
            std::vector<std::string> lines;
            const int count = atoi(ParamValue(xml, "lines").c_str());
            for (int i = 0; i < count && i < 12; ++i) lines.push_back(ParamValue(xml, "line" + std::to_string(i)));
            const std::string title = ParamValue(xml, "title");
            if (title == "Client update required") {
                if (!g_helloRejected) Overlay::OnConnectionRefused(title, lines);
            } else {
                // From server 1.27: the XP the race earned, for the level bar.
                Overlay::XpGain xp;
                xp.gained = atoi(ParamValue(xml, "xp").c_str());
                xp.total = _atoi64(ParamValue(xml, "xpTotal").c_str());
                xp.levelBefore = atoi(ParamValue(xml, "level").c_str());
                const std::string starts = ParamValue(xml, "levelStarts");
                for (size_t at = 0; at < starts.size() && xp.levelStarts.size() < 100;) {
                    const size_t comma = starts.find(',', at);
                    xp.levelStarts.push_back(_atoi64(starts.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
                xp.family = ParamValue(xml, "family");
                xp.pool = _atoi64(ParamValue(xml, "pool").c_str());
                xp.poolRank = atoi(ParamValue(xml, "poolRank").c_str());
                xp.team = ParamValue(xml, "team");
                xp.joinedTeam = ParamValue(xml, "joined") == "1";
                xp.crowned = ParamValue(xml, "crowned") == "1";
                if (xp.gained <= 0 || xp.levelBefore <= 0 || xp.levelStarts.size() < 2) xp = Overlay::XpGain();
                Overlay::PushNotice(ParamValue(xml, "kind"), title, lines, xp);
                // The server just counted a result, so the card's times may have changed.
                Overlay::RefreshStanding();
            }
        }
        // Where the player stands on the event they're racing.
        if (xml.find("<method> standing") != std::string::npos) {
            std::vector<Overlay::Board> boards;
            const int count = atoi(ParamValue(xml, "boards").c_str());
            for (int i = 0; i < count && i < 8; ++i) {
                // One board per parameter, to fit the channel's size limit:
                // metric|laps|record|record holder|best|place|players|beat|beat holder
                const std::string packed = ParamValue(xml, "b" + std::to_string(i));
                std::vector<std::string> f;
                size_t start = 0;
                while (true) {
                    const size_t bar = packed.find('|', start);
                    f.push_back(packed.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                    if (bar == std::string::npos) break;
                    start = bar + 1;
                }
                if (f.size() < 9) continue;
                Overlay::Board b;
                b.metric = f[0];
                b.laps = atoi(f[1].c_str());
                b.record = atof(f[2].c_str());
                b.recordHolder = f[3];
                b.best = atof(f[4].c_str());
                b.place = atoi(f[5].c_str());
                b.players = atoi(f[6].c_str());
                b.beat = atof(f[7].c_str());
                b.beatHolder = f[8];
                boards.push_back(b);
            }
            Overlay::SetStanding(ParamValue(xml, "track_key"), ParamValue(xml, "race_mode"), boards);
        }
        if ((xml.find("<method> ping<") != std::string::npos || xml.find("<method> ping ") != std::string::npos) &&
            xml.find("TRANSACTION_TYPE_REPLY") != std::string::npos && g_pingLockReady) {
            const uint32_t echoed = static_cast<uint32_t>(strtoul(ParamValue(xml, "t").c_str(), nullptr, 10));
            EnterCriticalSection(&g_pingLock);
            if (echoed != 0 && echoed == g_pingToken && g_pendingPingSentUs != 0) {
                const double sampleMs = static_cast<double>(NowMicros() - g_pendingPingSentUs) / 1000.0;
                g_pendingPingSentUs = 0;
                if (sampleMs >= 0.0 && sampleMs <= 1000.0) {
                    const bool firstSample = g_rttSmoothedMs < 0.0;
                    g_rttSmoothedMs = firstSample ? sampleMs : (g_rttSmoothedMs * 0.75 + sampleMs * 0.25);
                    if (firstSample) g_pingSendNow = true;
                }
            }
            LeaveCriticalSection(&g_pingLock);
        }
        if ((xml.find("<method> carPings<") != std::string::npos || xml.find("<method> carPings ") != std::string::npos) &&
            g_pingLockReady) {
            const int count = atoi(ParamValue(xml, "n").c_str());
            std::map<uint32_t, float> updated;
            for (int i = 0; i < count && i < 16; ++i) {
                const std::string entry = ParamValue(xml, "p" + std::to_string(i));
                const size_t bar = entry.find('|');
                if (bar == std::string::npos) continue;
                const uint32_t carId = static_cast<uint32_t>(strtoul(entry.substr(0, bar).c_str(), nullptr, 10));
                const int leadMs = atoi(entry.substr(bar + 1).c_str());
                if (carId == 0 || leadMs <= 0) continue;
                float leadSec = static_cast<float>(leadMs) * 0.001f;
                if (leadSec > MAX_CAR_LEAD_SECONDS) leadSec = MAX_CAR_LEAD_SECONDS;
                updated[carId] = leadSec;
            }
            EnterCriticalSection(&g_pingLock);
            g_carLeadSeconds.swap(updated);
            LeaveCriticalSection(&g_pingLock);
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
        bool closedOurs = false;
        {
            Lock lock;
            if (s == g_socket) {
                g_socket = INVALID_SOCKET;
                g_client.reset();
                g_helloRejected = false;
                closedOurs = true;
            }
        }
        if (closedOurs) {
            ResetLatencyState();
            EventVote::Reset();
            Overlay::ResetRoomState();
        }
        return g_realClose(s);
    }

    static DWORD WINAPI LinkThread(LPVOID) {
        for (;;) {
            {
                Lock lock;
                if (g_client) {
                    const DWORD nowTick = GetTickCount();
                    g_client->Tick(nowTick);
                    bool sendPing = false;
                    int reportedRtt = -1;
                    uint32_t token = 0;
                    if (g_pingLockReady && g_client->GetState() == SecureClient::State::Established) {
                        EnterCriticalSection(&g_pingLock);
                        if (g_pingSendNow || g_lastPingSentTick == 0 ||
                            static_cast<DWORD>(nowTick - g_lastPingSentTick) >= PING_INTERVAL_MS) {
                            g_pingSendNow = false;
                            g_lastPingSentTick = nowTick;
                            token = ++g_pingToken;
                            if (token == 0) token = ++g_pingToken;
                            g_pendingPingSentUs = NowMicros();
                            if (g_rttSmoothedMs >= 0.0) reportedRtt = static_cast<int>(g_rttSmoothedMs + 0.5);
                            sendPing = true;
                        }
                        LeaveCriticalSection(&g_pingLock);
                    }
                    if (sendPing) {
                        std::vector<std::pair<std::string, std::string> > params;
                        params.push_back(std::make_pair(std::string("t"), std::to_string(token)));
                        if (reportedRtt >= 0) params.push_back(std::make_pair(std::string("rtt"), std::to_string(reportedRtt)));
                        SendCommand("client", "ping", params);
                    }
                }
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
        Log::Debug("[OpenProStreet] Server link ready.\n");
        return true;
    }

    void Init(uintptr_t baseModule) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_pingLock);
        g_pingLockReady = true;

        const char* appData = getenv("LOCALAPPDATA");
        g_dataDirectory = std::string(appData ? appData : ".") + "\\OpenProStreet";
        CreateDirectoryA(g_dataDirectory.c_str(), nullptr);
        DeleteFileA((g_dataDirectory + "\\known_servers.txt").c_str()); // left by versions before 7.4
        g_hasIdentity = LoadIdentity();
        g_baseModule = baseModule;
        if (g_hasIdentity) InstallSocketHooks();
    }

    void OnDataSocketConnected(uintptr_t socketHandle) {
        if (!g_hasIdentity) return;
        if (!InstallSocketHooks()) {
            Log::Error("Could not hook the game's sockets (is this the v1.1 executable?). Encrypted login is off.\n");
            return;
        }
        const SOCKET s = static_cast<SOCKET>(socketHandle);

        sockaddr_storage peer;
        int peerSize = sizeof(peer);
        char address[128] = {0};
        DWORD addressSize = sizeof(address);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0 ||
            WSAAddressToStringA(reinterpret_cast<sockaddr*>(&peer), peerSize, nullptr, address, &addressSize) != 0) {
            Log::Warn("Could not read the server's address. Encrypted login is off for this connection.\n");
            return;
        }

        uint8_t ephemeral[32];
        if (!RandomBytes(ephemeral, sizeof(ephemeral))) return;

        Lock lock;
        g_socket = s;
        g_helloRejected = false;

        SecureClient::Callbacks callbacks;
        callbacks.send = [s](const uint8_t* data, int len) { g_realSend(s, reinterpret_cast<const char*>(data), len, 0); };
        callbacks.onMessage = [](const std::string& xml) { OnServerMessage(xml); };
        callbacks.log = [](const char* text) { Log::Event(Log::Tag::Server, "%s\n", text); };

        CarLock::SetAllowedCars(""); // a new connection starts outside any room
        EventVote::Reset();
        ResetLatencyState();
        g_client.reset(new SecureClient(g_identity, ephemeral, callbacks));
        g_client->Begin(GetTickCount());
        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("version"), version));
        SendCommand("client", "hello", params);
        if (g_quickMatchPending) SendCommand("matchmaking", "quickMatch", g_quickMatchParams);
        Log::Event(Log::Tag::Server, "Connecting to %s\n", Log::Highlight(address).c_str());
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
        if (!report.gameFiles.empty()) params.push_back(std::make_pair(std::string("game_files"), report.gameFiles));
        if (!report.speedLimit.empty()) params.push_back(std::make_pair(std::string("speed_limit"), report.speedLimit));

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

    void RequestStanding(const std::string& trackKey, const std::string& raceMode, const std::string& speedLimit) {
        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("track_key"), trackKey));
        params.push_back(std::make_pair(std::string("race_mode"), raceMode));
        params.push_back(std::make_pair(std::string("laps"), std::string("0"))); // the room's event decides
        if (!speedLimit.empty()) params.push_back(std::make_pair(std::string("speed_limit"), speedLimit));
        Lock lock;
        if (g_client && g_client->GetState() == SecureClient::State::Established) {
            SendCommand("results", "standing", params);
        }
    }

    void CallVote(int eventIndex) {
        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("index"), std::to_string(eventIndex)));
        Lock lock;
        if (g_client && g_client->GetState() == SecureClient::State::Established) SendCommand("client", "callVote", params);
    }

    void CastVote(bool yes) {
        std::vector<std::pair<std::string, std::string> > params;
        params.push_back(std::make_pair(std::string("yes"), std::string(yes ? "1" : "0")));
        Lock lock;
        if (g_client && g_client->GetState() == SecureClient::State::Established) SendCommand("client", "castVote", params);
    }

    float GetCarLeadSeconds(uint32_t carId) {
        if (!g_pingLockReady || carId == 0) return 0.0f;
        float lead = 0.0f;
        EnterCriticalSection(&g_pingLock);
        const std::map<uint32_t, float>::const_iterator it = g_carLeadSeconds.find(carId);
        if (it != g_carLeadSeconds.end()) lead = it->second;
        LeaveCriticalSection(&g_pingLock);
        return lead;
    }
}
