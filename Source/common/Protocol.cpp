#include "Protocol.h"
#include <cstring>

namespace bandjam::wire
{
namespace
{
    template <typename IntType>
    void writeLE (IntType value, void* dest)
    {
        value = juce::ByteOrder::swapIfBigEndian (value);
        std::memcpy (dest, &value, sizeof (value));
    }
}
bool writeAll (juce::StreamingSocket& socket, const void* data, int numBytes)
{
    auto* p = static_cast<const char*> (data);
    int remaining = numBytes;

    while (remaining > 0)
    {
        const int written = socket.write (p, remaining);
        if (written < 0)
            return false;

        if (written == 0)
        {
            // Socket buffer full (can happen on Windows under load) - back off briefly.
            if (! socket.isConnected())
                return false;
            juce::Thread::sleep (1);
            continue;
        }

        p         += written;
        remaining -= written;
    }
    return true;
}

bool readAll (juce::StreamingSocket& socket, void* dest, int numBytes)
{
    // blockUntilSpecifiedAmountHasArrived: returns numBytes or < 0 on error/close.
    const int got = socket.read (dest, numBytes, true);
    return got == numBytes;
}

bool send (juce::StreamingSocket& socket, MsgType type, const void* body,
           juce::uint32 bodyBytes, crypto::Session* session)
{
    if (session != nullptr && session->active)
    {
        // Sealed frame: [u32 cipherLen][cipher(header + body) + MAC]
        const size_t plainSize  = 6 + (size_t) bodyBytes;
        const size_t cipherSize = plainSize + (size_t) crypto::kMacBytes;

        juce::HeapBlock<juce::uint8> buffer (4 + cipherSize);
        juce::HeapBlock<juce::uint8> plain (plainSize);

        writeLE ((juce::uint32) bodyBytes, plain.getData());
        writeLE ((juce::uint16) type, plain.getData() + 4);
        if (bodyBytes > 0)
            std::memcpy (plain.getData() + 6, body, bodyBytes);

        writeLE ((juce::uint32) cipherSize, buffer.getData());
        crypto::seal (*session, plain.getData(), plainSize, buffer.getData() + 4);

        return writeAll (socket, buffer.getData(), (int) (4 + cipherSize));
    }

    char header[6];
    writeLE (bodyBytes, header);
    writeLE ((juce::uint16) type, header + 4);

    if (! writeAll (socket, header, 6))
        return false;
    if (bodyBytes > 0 && ! writeAll (socket, body, (int) bodyBytes))
        return false;
    return true;
}

bool sendJson (juce::StreamingSocket& socket, MsgType type, const juce::var& json,
               crypto::Session* session)
{
    const auto text = juce::JSON::toString (json, true);
    const auto utf8 = text.toRawUTF8();
    return send (socket, type, utf8, (juce::uint32) strlen (utf8), session);
}

bool receive (juce::StreamingSocket& socket, Message& out, crypto::Session* session)
{
    if (session != nullptr && session->active)
    {
        char lenBuf[4];
        if (! readAll (socket, lenBuf, 4))
            return false;

        const auto cipherLen = juce::ByteOrder::littleEndianInt (lenBuf);
        if (cipherLen < 6 + (juce::uint32) crypto::kMacBytes
            || cipherLen > kMaxBodyBytes + 6 + (juce::uint32) crypto::kMacBytes)
            return false; // desync or tampering - drop the connection

        juce::HeapBlock<juce::uint8> cipher (cipherLen);
        if (! readAll (socket, cipher.getData(), (int) cipherLen))
            return false;

        const size_t plainSize = cipherLen - (size_t) crypto::kMacBytes;
        juce::HeapBlock<juce::uint8> plain (plainSize);
        if (! crypto::open (*session, cipher.getData(), cipherLen, plain.getData()))
            return false; // authentication failed

        const auto bodyBytes = juce::ByteOrder::littleEndianInt (plain.getData());
        if ((size_t) bodyBytes + 6 != plainSize)
            return false;

        out.type = (MsgType) juce::ByteOrder::littleEndianShort (plain.getData() + 4);
        out.body.setSize (bodyBytes, false);
        if (bodyBytes > 0)
            std::memcpy (out.body.getData(), plain.getData() + 6, bodyBytes);
        return true;
    }

    char header[6];
    if (! readAll (socket, header, 6))
        return false;

    const auto bodyBytes = juce::ByteOrder::littleEndianInt (header);
    const auto type      = juce::ByteOrder::littleEndianShort (header + 4);

    if (bodyBytes > kMaxBodyBytes)
        return false; // stream desync - drop the connection

    out.type = (MsgType) type;
    out.body.setSize (bodyBytes, false);
    if (bodyBytes > 0 && ! readAll (socket, out.body.getData(), (int) bodyBytes))
        return false;

    return true;
}

bool readLine (juce::StreamingSocket& socket, juce::String& out, int maxLen)
{
    juce::MemoryBlock bytes;
    char c = 0;

    while ((int) bytes.getSize() < maxLen)
    {
        if (socket.read (&c, 1, true) != 1)
            return false;
        if (c == '\n')
        {
            out = juce::String::fromUTF8 (static_cast<const char*> (bytes.getData()),
                                          (int) bytes.getSize());
            return true;
        }
        if (c != '\r')
            bytes.append (&c, 1);
    }
    return false;
}

juce::MemoryBlock encodeAudioBlock (juce::int64 startSample, const float* mono, int numSamples)
{
    juce::MemoryBlock block (8 + (size_t) numSamples * 2, false);
    auto* p = static_cast<char*> (block.getData());

    writeLE ((juce::uint64) startSample, p);
    auto* out = reinterpret_cast<juce::int16*> (p + 8);

    for (int i = 0; i < numSamples; ++i)
    {
        const float clamped = juce::jlimit (-1.0f, 1.0f, mono[i]);
        juce::int16 v = (juce::int16) juce::roundToInt (clamped * 32767.0f);
        out[i] = (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) v);
    }
    return block;
}

bool decodeAudioBlock (const Message& msg, juce::int64& startSample,
                       const juce::int16*& samples, int& numSamples)
{
    if (msg.body.getSize() < 8 || ((msg.body.getSize() - 8) % 2) != 0)
        return false;

    auto* p = static_cast<const char*> (msg.body.getData());
    startSample = (juce::int64) juce::ByteOrder::littleEndianInt64 (p);
    samples     = reinterpret_cast<const juce::int16*> (p + 8);
    numSamples  = (int) ((msg.body.getSize() - 8) / 2);
    return startSample >= 0;
}

//==============================================================================
juce::MemoryBlock encodeVoiceBlockInt16 (const juce::String& speakerName,
                                         juce::int64 startSample, const juce::int16* samples, int numSamples)
{
    const auto utf8 = speakerName.toRawUTF8();
    const auto nameLen = (size_t) juce::jmin ((int) strlen (utf8), 255);

    juce::MemoryBlock block (1 + nameLen + 8 + (size_t) numSamples * 2, false);
    auto* p = static_cast<char*> (block.getData());

    *p = (char) (juce::uint8) nameLen;
    std::memcpy (p + 1, utf8, nameLen);
    writeLE ((juce::uint64) startSample, p + 1 + nameLen);

    auto* out = reinterpret_cast<juce::int16*> (p + 1 + nameLen + 8);
    for (int i = 0; i < numSamples; ++i)
        out[i] = (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) samples[i]);
    return block;
}

juce::MemoryBlock encodeVoiceBlock (const juce::String& speakerName,
                                    juce::int64 startSample, const float* mono, int numSamples)
{
    juce::HeapBlock<juce::int16> converted ((size_t) juce::jmax (1, numSamples));
    for (int i = 0; i < numSamples; ++i)
        converted[i] = (juce::int16) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, mono[i]) * 32767.0f);
    return encodeVoiceBlockInt16 (speakerName, startSample, converted.getData(), numSamples);
}

bool decodeVoiceBlock (const Message& msg, juce::String& speakerName,
                       juce::int64& startSample, const juce::int16*& samples, int& numSamples)
{
    const auto size = msg.body.getSize();
    if (size < 1)
        return false;

    auto* p = static_cast<const char*> (msg.body.getData());
    const auto nameLen = (size_t) (juce::uint8) p[0];
    if (size < 1 + nameLen + 8 || ((size - 1 - nameLen - 8) % 2) != 0)
        return false;

    speakerName = juce::String::fromUTF8 (p + 1, (int) nameLen);
    startSample = (juce::int64) juce::ByteOrder::littleEndianInt64 (p + 1 + nameLen);
    samples     = reinterpret_cast<const juce::int16*> (p + 1 + nameLen + 8);
    numSamples  = (int) ((size - 1 - nameLen - 8) / 2);
    return startSample >= 0;
}

} // namespace bandjam::wire
