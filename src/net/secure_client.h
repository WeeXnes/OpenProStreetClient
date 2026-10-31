#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The client side of the encrypted channel to an OpenProStreet server. The protocol is described
// in the daemon's src/secure/secure_gateway.h. This class has no sockets and no Windows code, so
// the same code the game runs can be tested against a real server without the game.
//
// Not thread safe: the caller holds a lock around every call.
class SecureClient {
public:
    enum class State {
        Handshaking,
        Established,
        Plaintext, // the server doesn't speak this protocol, such as the 2007 servers
        Failed     // the server's identity changed, or a known server stopped answering
    };

    struct Callbacks {
        // Sends a datagram to the server.
        std::function<void(const uint8_t* data, int len)> send;
        // Pins the server's key. False means this address is known with a different key.
        std::function<bool(const uint8_t key[32])> acceptServerKey;
        // True if this address has been seen with a key before, so it must not fall back to plaintext.
        std::function<bool()> isKnownServer;
        // A message from the server on the client channel.
        std::function<void(const std::string& xml)> onMessage;
        std::function<void(const char* text)> log;
    };

    SecureClient(const uint8_t identitySecretKey[64], const uint8_t ephemeralSecret[32], Callbacks callbacks);
    ~SecureClient();

    // Starts the handshake. Nothing the game sends leaves until it finishes.
    void Begin(uint32_t nowMs);

    // Resends what was lost. Call this regularly.
    void Tick(uint32_t nowMs);

    // A packet from the game, on its way to the server.
    void SendGamePacket(const uint8_t* data, int len, uint32_t nowMs);

    // A packet from the network. Returns the length of the game packet written to out,
    // or 0 if the game shouldn't see it. out must hold at least len bytes.
    int OnNetworkPacket(const uint8_t* data, int len, uint8_t* out, uint32_t nowMs);

    // A message to the server on the client channel. Delivered in order, once.
    void SendChannelMessage(const std::string& xml, uint32_t nowMs);

    State GetState() const { return m_state; }

private:
    enum class Step { Hello, Finish, Done };

    struct OutgoingMessage {
        uint32_t             seq;
        std::vector<uint8_t> packet;
        uint32_t             lastSent;
    };

    void SendHello();
    void OnCookie(const uint8_t* data, int len, uint32_t nowMs);
    void OnServerHello(const uint8_t* data, int len, uint32_t nowMs);
    void OnServerFinish(const uint8_t* data, int len);
    void Fail(const char* reason);
    void FlushQueuedPackets(uint32_t nowMs);
    void SendSealed(const uint8_t* plain, int len);
    bool CheckReplay(uint64_t counter);
    void OnChannelPacket(const uint8_t* data, int len);

    Callbacks m_callbacks;
    State     m_state = State::Handshaking;
    Step      m_step = Step::Hello;

    uint8_t m_identitySecret[64];
    uint8_t m_identityPublic[32];
    uint8_t m_ephemeralSecret[32];
    uint8_t m_ephemeralPublic[32];
    uint8_t m_cookie[16];
    uint8_t m_finishPacket[116];
    uint8_t m_sendKey[32];
    uint8_t m_recvKey[32];

    uint32_t m_startedAt = 0;
    uint32_t m_lastHandshakeSend = 0;
    bool     m_serverAnswered = false;

    uint64_t m_sendCounter = 1;
    uint64_t m_highestReceived = 0;
    uint64_t m_seen[16];

    std::deque<std::vector<uint8_t>> m_queuedGamePackets;

    uint32_t                        m_nextMessageSeq = 0;
    uint32_t                        m_expectedMessageSeq = 0;
    std::deque<OutgoingMessage>     m_outgoingMessages;
    std::map<uint32_t, std::string> m_heldMessages;
};
