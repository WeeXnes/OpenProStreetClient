// Tests the plugin's SecureClient against a real server without the game: a local UDP relay that
// does what the plugin's socket hooks do. A simulated game client talks plaintext to this program,
// and each client gets its own encrypted connection to the server.
//
//   secure_proxy <listen port> <server port>
//
// Point OpenProStreet's tests/race_test.js at it with PROXY_PORT=<listen port>.

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
#include <string>
#include <vector>

#include "net/secure_client.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

struct Peer {
    sockaddr_in gameAddress;
    SOCKET server;
    std::unique_ptr<SecureClient> client;
    bool announced;
};

static void RandomBytes(void* buffer, size_t size) {
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

static const char* StateName(SecureClient::State state) {
    switch (state) {
        case SecureClient::State::Handshaking: return "handshaking";
        case SecureClient::State::Established: return "encrypted";
        case SecureClient::State::Plaintext:   return "plaintext";
        default:                               return "failed";
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: secure_proxy <listen port> <server port>\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(static_cast<u_short>(atoi(argv[1])));
    if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        printf("could not bind\n");
        return 1;
    }
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddress.sin_port = htons(static_cast<u_short>(atoi(argv[2])));

    std::map<std::string, std::unique_ptr<Peer>> peers;
    const bool knownServer = argc > 3 && strcmp(argv[3], "known") == 0;
    uint32_t lastTick = GetTickCount();
    std::string pinned;

    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        for (auto& entry : peers) FD_SET(entry.second->server, &readable);
        timeval timeout{0, 5000};
        select(0, &readable, nullptr, nullptr, &timeout);

        if (FD_ISSET(listener, &readable)) {
            uint8_t data[2048];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int received = recvfrom(listener, reinterpret_cast<char*>(data), sizeof(data), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received > 0) {
                const std::string key = std::to_string(from.sin_addr.s_addr) + ":" + std::to_string(from.sin_port);
                auto found = peers.find(key);
                if (found == peers.end()) {
                    std::unique_ptr<Peer> peer(new Peer());
                    peer->gameAddress = from;
                    peer->announced = false;
                    peer->server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    connect(peer->server, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress));

                    uint8_t seed[32], secret[64], publicKey[32], ephemeral[32];
                    RandomBytes(seed, sizeof(seed));
                    RandomBytes(ephemeral, sizeof(ephemeral));
                    crypto_ed25519_key_pair(secret, publicKey, seed);

                    const SOCKET server = peer->server;
                    SecureClient::Callbacks callbacks;
                    callbacks.send = [server](const uint8_t* bytes, int len) { send(server, reinterpret_cast<const char*>(bytes), len, 0); };
                    callbacks.acceptServerKey = [&pinned](const uint8_t bytes[32]) {
                        const std::string candidate(reinterpret_cast<const char*>(bytes), 32);
                        if (pinned.empty()) pinned = candidate;
                        return pinned == candidate;
                    };
                    callbacks.isKnownServer = [&pinned, knownServer]() { return knownServer || !pinned.empty(); };
                    callbacks.onMessage = [](const std::string& xml) { printf("server message: %.200s\n", xml.c_str()); };
                    callbacks.log = [](const char* text) { printf("client: %s\n", text); };
                    peer->client.reset(new SecureClient(secret, ephemeral, callbacks));
                    peer->client->Begin(GetTickCount());
                    peer->client->SendChannelMessage(
                        "<service name=\"client\"><transaction id=\"1\" type=\"TRANSACTION_TYPE_REQUEST\"><method> hello"
                        "<param><name> version </name><value> proxy </value></param></method></transaction></service>",
                        GetTickCount());
                    found = peers.emplace(key, std::move(peer)).first;
                }
                found->second->client->SendGamePacket(data, received, GetTickCount());
            }
        }

        for (auto& entry : peers) {
            Peer& peer = *entry.second;
            if (!FD_ISSET(peer.server, &readable)) continue;
            uint8_t data[2048];
            const int received = recv(peer.server, reinterpret_cast<char*>(data), sizeof(data), 0);
            if (received <= 0) continue;
            uint8_t plain[2048];
            const int size = peer.client->OnNetworkPacket(data, received, plain, GetTickCount());
            if (size > 0) {
                sendto(listener, reinterpret_cast<const char*>(plain), size, 0, reinterpret_cast<const sockaddr*>(&peer.gameAddress), sizeof(peer.gameAddress));
            }
        }

        const uint32_t now = GetTickCount();
        if (now - lastTick >= 20) {
            lastTick = now;
            for (auto& entry : peers) {
                Peer& peer = *entry.second;
                peer.client->Tick(now);
                if (!peer.announced && peer.client->GetState() != SecureClient::State::Handshaking) {
                    peer.announced = true;
                    printf("peer %s is %s\n", entry.first.c_str(), StateName(peer.client->GetState()));
                }
            }
        }
    }
}
