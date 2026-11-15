#include "net/secure_client.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <cstring>

static const uint8_t PACKET_HANDSHAKE = 'H';
static const uint8_t PACKET_SEALED = 'S';
static const uint8_t PACKET_CLIENT = 'P';
static const uint8_t CLIENT_KIND_MESSAGE = 1;
static const uint8_t CLIENT_KIND_ACK = 2;
static const uint8_t PROTOCOL_VERSION = 1;
static const int SEALED_OVERHEAD = 28;
static const int HELLO_SIZE = 52;
static const int HELLO_REPLY_SIZE = 132;
static const int FINISH_SIZE = 116;
static const int FINISH_REPLY_SIZE = 21;
static const int CLIENT_HEADER_SIZE = 8;
static const int MAX_DATAGRAM = 1500;
static const size_t MAX_MESSAGE_SIZE = 1200;
static const size_t MAX_QUEUED_GAME_PACKETS = 32;
static const size_t MAX_OUTGOING_MESSAGES = 32;
static const size_t MAX_HELD_MESSAGES = 32;
static const uint32_t HANDSHAKE_RESEND_MS = 300;
static const uint32_t MESSAGE_RESEND_MS = 250;
static const uint32_t SILENT_SERVER_MS = 2000;  // the game's connection attempt lasts longer than this
static const uint32_t HANDSHAKE_LIMIT_MS = 6000;
static const uint64_t REPLAY_WINDOW = 1024;

static void WriteLe32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

static uint32_t ReadLe32(const uint8_t* in) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

static void WriteLe64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

static uint64_t ReadLe64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return value;
}

static void AeadLock(const uint8_t key[32], uint64_t counter, const uint8_t* ad, size_t adSize,
                     const uint8_t* plain, size_t size, uint8_t* cipher, uint8_t mac[16]) {
    uint8_t nonce[12] = {0};
    WriteLe64(nonce + 4, counter);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, cipher, mac, ad, adSize, plain, size);
    crypto_wipe(&ctx, sizeof(ctx));
}

static bool AeadUnlock(const uint8_t key[32], uint64_t counter, const uint8_t* ad, size_t adSize,
                       const uint8_t* cipher, size_t size, const uint8_t mac[16], uint8_t* plain) {
    uint8_t nonce[12] = {0};
    WriteLe64(nonce + 4, counter);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    const int result = crypto_aead_read(&ctx, plain, mac, ad, adSize, cipher, size);
    crypto_wipe(&ctx, sizeof(ctx));
    return result == 0;
}

SecureClient::SecureClient(const uint8_t identitySecretKey[64], const uint8_t ephemeralSecret[32], Callbacks callbacks)
    : m_callbacks(std::move(callbacks)) {
    memcpy(m_identitySecret, identitySecretKey, 64);
    memcpy(m_identityPublic, identitySecretKey + 32, 32); // an Ed25519 secret key is the seed, then the public key
    memcpy(m_ephemeralSecret, ephemeralSecret, 32);
    crypto_x25519_public_key(m_ephemeralPublic, m_ephemeralSecret);
    memset(m_cookie, 0, sizeof(m_cookie));
    memset(m_seen, 0, sizeof(m_seen));
}

SecureClient::~SecureClient() {
    crypto_wipe(m_identitySecret, sizeof(m_identitySecret));
    crypto_wipe(m_ephemeralSecret, sizeof(m_ephemeralSecret));
    crypto_wipe(m_sendKey, sizeof(m_sendKey));
    crypto_wipe(m_recvKey, sizeof(m_recvKey));
}

void SecureClient::SendHello() {
    uint8_t hello[HELLO_SIZE] = {PACKET_HANDSHAKE, 1, PROTOCOL_VERSION, 0};
    memcpy(hello + 4, m_ephemeralPublic, 32);
    memcpy(hello + 36, m_cookie, 16);
    m_callbacks.send(hello, sizeof(hello));
}

void SecureClient::Begin(uint32_t nowMs) {
    m_startedAt = nowMs;
    m_lastHandshakeSend = nowMs;
    SendHello();
}

void SecureClient::Fail(const char* reason) {
    m_state = State::Failed;
    m_queuedGamePackets.clear();
    if (m_callbacks.log) m_callbacks.log(reason);
}

void SecureClient::Tick(uint32_t nowMs) {
    if (m_state == State::Handshaking) {
        const uint32_t elapsed = nowMs - m_startedAt;
        if (!m_serverAnswered && elapsed >= SILENT_SERVER_MS) {
            m_state = State::Plaintext;
            if (m_callbacks.log) m_callbacks.log("This server doesn't support encryption. Using the game's plaintext protocol.");
            FlushQueuedPackets(nowMs);
            return;
        }
        if (elapsed >= HANDSHAKE_LIMIT_MS) {
            Fail("The encrypted handshake didn't finish.");
            return;
        }
        if (nowMs - m_lastHandshakeSend >= HANDSHAKE_RESEND_MS) {
            m_lastHandshakeSend = nowMs;
            if (m_step == Step::Hello) SendHello();
            else if (m_step == Step::Finish) m_callbacks.send(m_finishPacket, FINISH_SIZE);
        }
        return;
    }
    if (m_state != State::Established) return;
    for (size_t i = 0; i < m_outgoingMessages.size(); ++i) {
        OutgoingMessage& message = m_outgoingMessages[i];
        if (nowMs - message.lastSent >= MESSAGE_RESEND_MS) {
            message.lastSent = nowMs;
            SendSealed(message.packet.data(), static_cast<int>(message.packet.size()));
        }
    }
}

void SecureClient::SendSealed(const uint8_t* plain, int len) {
    uint8_t sealed[MAX_DATAGRAM];
    if (len < 0 || len + SEALED_OVERHEAD > MAX_DATAGRAM) return;
    memset(sealed, 0, 4);
    sealed[0] = PACKET_SEALED;
    const uint64_t counter = m_sendCounter++;
    WriteLe64(sealed + 4, counter);
    AeadLock(m_sendKey, counter, sealed, 12, plain, static_cast<size_t>(len), sealed + SEALED_OVERHEAD, sealed + 12);
    m_callbacks.send(sealed, len + SEALED_OVERHEAD);
}

void SecureClient::FlushOutgoingMessages(uint32_t nowMs) {
    for (size_t i = 0; i < m_outgoingMessages.size(); ++i) {
        m_outgoingMessages[i].lastSent = nowMs;
        SendSealed(m_outgoingMessages[i].packet.data(), static_cast<int>(m_outgoingMessages[i].packet.size()));
    }
}

void SecureClient::FlushQueuedPackets(uint32_t) {
    while (!m_queuedGamePackets.empty()) {
        const std::vector<uint8_t>& packet = m_queuedGamePackets.front();
        if (m_state == State::Established) SendSealed(packet.data(), static_cast<int>(packet.size()));
        else m_callbacks.send(packet.data(), static_cast<int>(packet.size()));
        m_queuedGamePackets.pop_front();
    }
}

void SecureClient::SendGamePacket(const uint8_t* data, int len, uint32_t) {
    switch (m_state) {
        case State::Established:
            SendSealed(data, len);
            break;
        case State::Plaintext:
            m_callbacks.send(data, len);
            break;
        case State::Handshaking:
            if (m_queuedGamePackets.size() >= MAX_QUEUED_GAME_PACKETS) m_queuedGamePackets.pop_front();
            m_queuedGamePackets.push_back(std::vector<uint8_t>(data, data + len));
            break;
        case State::Failed:
            break; // the game times out and reports a lost connection
    }
}

void SecureClient::OnCookie(const uint8_t* data, int len, uint32_t nowMs) {
    if (m_step != Step::Hello || len != 20) return;
    memcpy(m_cookie, data + 4, 16);
    m_serverAnswered = true;
    m_lastHandshakeSend = nowMs;
    SendHello();
}

void SecureClient::OnServerHello(const uint8_t* data, int len, uint32_t nowMs) {
    if (m_step != Step::Hello || len != HELLO_REPLY_SIZE) return;
    const uint8_t* serverEphemeral = data + 4;
    const uint8_t* serverKey = data + 36;
    const uint8_t* signature = data + 68;

    uint8_t transcript[64];
    crypto_blake2b_ctx hash;
    crypto_blake2b_init(&hash, 64);
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>("OPS1-transcript"), 15);
    crypto_blake2b_update(&hash, m_ephemeralPublic, 32);
    crypto_blake2b_update(&hash, serverEphemeral, 32);
    crypto_blake2b_update(&hash, serverKey, 32);
    crypto_blake2b_final(&hash, transcript);

    uint8_t message[11 + 64];
    memcpy(message, "OPS1-server", 11);
    memcpy(message + 11, transcript, 64);
    if (crypto_ed25519_check(signature, serverKey, message, sizeof(message)) != 0) return; // forged: keep waiting for the real one

    uint8_t shared[32];
    crypto_x25519(shared, m_ephemeralSecret, serverEphemeral);
    static const uint8_t ZERO[32] = {0};
    if (crypto_verify32(shared, ZERO) == 0) {
        Fail("The server sent an invalid key.");
        return;
    }

    uint8_t keys[64];
    crypto_blake2b_init(&hash, 64);
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>("OPS1-keys"), 9);
    crypto_blake2b_update(&hash, shared, 32);
    crypto_blake2b_update(&hash, transcript, 64);
    crypto_blake2b_final(&hash, keys);
    memcpy(m_sendKey, keys, 32);
    memcpy(m_recvKey, keys + 32, 32);
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(keys, sizeof(keys));

    uint8_t toSign[11 + 64];
    memcpy(toSign, "OPS1-client", 11);
    memcpy(toSign + 11, transcript, 64);
    uint8_t plain[32 + 64];
    memcpy(plain, m_identityPublic, 32);
    crypto_ed25519_sign(plain + 32, m_identitySecret, toSign, sizeof(toSign));

    memset(m_finishPacket, 0, 4);
    m_finishPacket[0] = PACKET_HANDSHAKE;
    m_finishPacket[1] = 4;
    AeadLock(m_sendKey, 0, m_finishPacket, 4, plain, sizeof(plain), m_finishPacket + 20, m_finishPacket + 4);
    m_serverAnswered = true;
    m_step = Step::Finish;
    m_lastHandshakeSend = nowMs;
    m_callbacks.send(m_finishPacket, FINISH_SIZE);
}

void SecureClient::OnServerFinish(const uint8_t* data, int len, uint32_t nowMs) {
    if (m_step != Step::Finish || len != FINISH_REPLY_SIZE) return;
    uint8_t status = 1;
    if (!AeadUnlock(m_recvKey, 0, data, 4, data + 20, 1, data + 4, &status)) return;
    if (status != 0) {
        Fail("The server refused the encrypted connection.");
        return;
    }
    m_step = Step::Done;
    m_state = State::Established;
    crypto_wipe(m_ephemeralSecret, sizeof(m_ephemeralSecret));
    if (m_callbacks.log) m_callbacks.log("Encrypted connection established.");
    FlushOutgoingMessages(nowMs);
    FlushQueuedPackets(nowMs);
}

bool SecureClient::CheckReplay(uint64_t counter) {
    if (counter == 0 || counter + REPLAY_WINDOW <= m_highestReceived) return false;
    const size_t bit = static_cast<size_t>(counter % REPLAY_WINDOW);
    const uint64_t mask = uint64_t(1) << (bit % 64);
    if (counter > m_highestReceived) {
        const uint64_t distance = counter - m_highestReceived;
        if (distance >= REPLAY_WINDOW) {
            memset(m_seen, 0, sizeof(m_seen));
        } else {
            for (uint64_t c = m_highestReceived + 1; c <= counter; ++c) {
                const size_t slot = static_cast<size_t>(c % REPLAY_WINDOW);
                m_seen[slot / 64] &= ~(uint64_t(1) << (slot % 64));
            }
        }
        m_highestReceived = counter;
    } else if (m_seen[bit / 64] & mask) {
        return false;
    }
    m_seen[bit / 64] |= mask;
    return true;
}

void SecureClient::OnChannelPacket(const uint8_t* data, int len) {
    if (len < CLIENT_HEADER_SIZE) return;
    const uint8_t kind = data[1];
    const uint32_t seq = ReadLe32(data + 4);

    if (kind == CLIENT_KIND_ACK) {
        for (std::deque<OutgoingMessage>::iterator it = m_outgoingMessages.begin(); it != m_outgoingMessages.end(); ++it) {
            if (it->seq == seq) {
                m_outgoingMessages.erase(it);
                break;
            }
        }
        return;
    }
    if (kind != CLIENT_KIND_MESSAGE) return;

    uint8_t ack[CLIENT_HEADER_SIZE] = {PACKET_CLIENT, CLIENT_KIND_ACK};
    WriteLe32(ack + 4, seq);
    SendSealed(ack, sizeof(ack));

    if (seq < m_expectedMessageSeq) return; // a retransmit
    if (m_heldMessages.size() < MAX_HELD_MESSAGES) {
        m_heldMessages[seq].assign(reinterpret_cast<const char*>(data + CLIENT_HEADER_SIZE), static_cast<size_t>(len - CLIENT_HEADER_SIZE));
    }
    for (std::map<uint32_t, std::string>::iterator it = m_heldMessages.find(m_expectedMessageSeq); it != m_heldMessages.end();
         it = m_heldMessages.find(m_expectedMessageSeq)) {
        const std::string xml = it->second;
        m_heldMessages.erase(it);
        ++m_expectedMessageSeq;
        if (m_callbacks.onMessage) m_callbacks.onMessage(xml);
    }
}

int SecureClient::OnNetworkPacket(const uint8_t* data, int len, uint8_t* out, uint32_t nowMs) {
    if (len < 1) return 0;

    if (data[0] == PACKET_HANDSHAKE) {
        if (m_state != State::Handshaking || len < 4) return 0;
        if (data[1] == 2) OnCookie(data, len, nowMs);
        else if (data[1] == 3) OnServerHello(data, len, nowMs);
        else if (data[1] == 5) OnServerFinish(data, len, nowMs);
        return 0;
    }

    if (m_state == State::Plaintext) {
        memcpy(out, data, static_cast<size_t>(len));
        return len;
    }

    if (data[0] == PACKET_SEALED && m_state == State::Established && len >= SEALED_OVERHEAD) {
        const uint64_t counter = ReadLe64(data + 4);
        const int size = len - SEALED_OVERHEAD;
        if (counter == 0 || counter + REPLAY_WINDOW <= m_highestReceived) return 0;
        if (!AeadUnlock(m_recvKey, counter, data, 12, data + SEALED_OVERHEAD, static_cast<size_t>(size), data + 12, out)) return 0;
        if (!CheckReplay(counter) || size < 1) return 0;
        if (out[0] == PACKET_CLIENT) {
            OnChannelPacket(out, size);
            return 0;
        }
        return size;
    }

    // Plaintext during or after the handshake: either the game's server answered the queued
    // packets (impossible, they aren't sent yet) or someone is forging packets.
    return 0;
}

void SecureClient::SendChannelMessage(const std::string& xml, uint32_t nowMs) {
    if (m_state == State::Plaintext || m_state == State::Failed) return;
    if (xml.size() > MAX_MESSAGE_SIZE || m_outgoingMessages.size() >= MAX_OUTGOING_MESSAGES) return;
    OutgoingMessage message;
    message.seq = m_nextMessageSeq++;
    message.packet.resize(CLIENT_HEADER_SIZE + xml.size());
    message.packet[0] = PACKET_CLIENT;
    message.packet[1] = CLIENT_KIND_MESSAGE;
    WriteLe32(&message.packet[4], message.seq);
    memcpy(&message.packet[CLIENT_HEADER_SIZE], xml.data(), xml.size());
    message.lastSent = nowMs;
    if (m_state == State::Established) SendSealed(message.packet.data(), static_cast<int>(message.packet.size()));
    else message.lastSent = nowMs - MESSAGE_RESEND_MS; // goes out with the first Tick after the handshake
    m_outgoingMessages.push_back(message);
}
