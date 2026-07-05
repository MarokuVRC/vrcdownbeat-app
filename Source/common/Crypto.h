#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>

namespace bandjam::crypto
{
/** End-to-end encryption for a host<->musician connection.

    Both sides exchange ephemeral X25519 public keys right after the TCP
    connection is up (before any protocol message); the shared secret is
    hashed into one key per direction and every wire frame is then sealed
    with XChaCha20-Poly1305 (Monocypher). Keys live only for the lifetime of
    the connection and are never written anywhere.

    Because the handshake happens between the two apps, the encryption is
    END-TO-END: the room relay (and anyone else on the path) only ever sees
    ciphertext. */
struct Session
{
    uint8_t  txKey[32] {};
    uint8_t  rxKey[32] {};
    uint64_t txCounter { 0 };
    uint64_t rxCounter { 0 };
    bool     active { false };
};

/** Blocking key exchange on a fresh connection. @p isHost picks the key
    direction (host and musician must pass opposite values). */
bool handshake (juce::StreamingSocket& socket, bool isHost, Session& session);

constexpr int kMacBytes = 16;

/** Seals plain -> cipher (cipher must hold size + kMacBytes). Bumps txCounter. */
void seal (Session& session, const void* plain, size_t size, uint8_t* cipher);

/** Opens cipher (size includes the MAC) -> plain (must hold size - kMacBytes).
    Returns false on tampering/desync. Bumps rxCounter on success. */
bool open (Session& session, const uint8_t* cipher, size_t size, void* plain);

} // namespace bandjam::crypto
