#include "Crypto.h"

extern "C"
{
#include "monocypher.h"
}

#include <cstring>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <bcrypt.h>
#endif

namespace bandjam::crypto
{
namespace
{
    /** Cryptographically secure random bytes (OS RNG). */
    bool secureRandom (uint8_t* out, size_t bytes)
    {
       #if JUCE_WINDOWS
        return BCRYPT_SUCCESS (BCryptGenRandom (nullptr, out, (ULONG) bytes,
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG));
       #else
        juce::FileInputStream urandom (juce::File ("/dev/urandom"));
        return urandom.openedOk() && urandom.read (out, (int) bytes) == (int) bytes;
       #endif
    }

    bool readExact (juce::StreamingSocket& socket, void* dest, int bytes)
    {
        auto* p = static_cast<char*> (dest);
        while (bytes > 0)
        {
            const int got = socket.read (p, bytes, true);
            if (got <= 0)
                return false;
            p += got;
            bytes -= got;
        }
        return true;
    }

    void counterNonce (uint64_t counter, uint8_t nonce[24])
    {
        std::memset (nonce, 0, 24);
        for (int i = 0; i < 8; ++i)
            nonce[i] = (uint8_t) (counter >> (i * 8));
    }
}

bool handshake (juce::StreamingSocket& socket, bool isHost, Session& session)
{
    uint8_t mySecret[32], myPublic[32], theirPublic[32];
    if (! secureRandom (mySecret, sizeof (mySecret)))
        return false;

    crypto_x25519_public_key (myPublic, mySecret);

    // Both sides send first, then read - 32 bytes always fit in the TCP buffer.
    if (socket.write (myPublic, 32) != 32 || ! readExact (socket, theirPublic, 32))
    {
        crypto_wipe (mySecret, sizeof (mySecret));
        return false;
    }

    uint8_t shared[32];
    crypto_x25519 (shared, mySecret, theirPublic);
    crypto_wipe (mySecret, sizeof (mySecret));

    // Derive one key per direction: hash(shared || hostPk || musicianPk).
    const uint8_t* hostPk     = isHost ? myPublic : theirPublic;
    const uint8_t* musicianPk = isHost ? theirPublic : myPublic;

    crypto_blake2b_ctx ctx;
    crypto_blake2b_init (&ctx, 64);
    crypto_blake2b_update (&ctx, shared, 32);
    crypto_blake2b_update (&ctx, hostPk, 32);
    crypto_blake2b_update (&ctx, musicianPk, 32);

    uint8_t derived[64];
    crypto_blake2b_final (&ctx, derived);
    crypto_wipe (shared, sizeof (shared));

    // First half: host -> musician, second half: musician -> host.
    std::memcpy (isHost ? session.txKey : session.rxKey, derived,      32);
    std::memcpy (isHost ? session.rxKey : session.txKey, derived + 32, 32);
    crypto_wipe (derived, sizeof (derived));

    session.txCounter = 0;
    session.rxCounter = 0;
    session.active    = true;
    return true;
}

void seal (Session& session, const void* plain, size_t size, uint8_t* cipher)
{
    uint8_t nonce[24];
    counterNonce (session.txCounter++, nonce);

    // Layout: [ciphertext][16-byte MAC]
    crypto_aead_lock (cipher, cipher + size, session.txKey, nonce,
                      nullptr, 0, static_cast<const uint8_t*> (plain), size);
}

bool open (Session& session, const uint8_t* cipher, size_t size, void* plain)
{
    if (size < (size_t) kMacBytes)
        return false;

    uint8_t nonce[24];
    counterNonce (session.rxCounter, nonce);

    const size_t textSize = size - (size_t) kMacBytes;
    if (crypto_aead_unlock (static_cast<uint8_t*> (plain), cipher + textSize,
                            session.rxKey, nonce, nullptr, 0, cipher, textSize) != 0)
        return false;

    ++session.rxCounter;
    return true;
}

} // namespace bandjam::crypto
