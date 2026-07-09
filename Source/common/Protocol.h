#pragma once

#include <juce_core/juce_core.h>
#include "common/Crypto.h"

namespace bandjam
{
constexpr int kDefaultPort       = 47800;
constexpr int kProtocolVersion   = 2;   ///< v2: end-to-end encrypted connections

/** The room relay: a tiny public server both sides connect OUTBOUND to, so
    hosts never share their IP or open ports (see relay/RelayServer.cpp).
    Control phase is '\n'-terminated ASCII lines; after pairing the socket
    carries the normal BandJam protocol unchanged. */
namespace relay
{
    constexpr int         kDefaultPort    = 47900;
    constexpr const char* kDefaultAddress = "178.104.32.120";

    /** Splits "host[:port]" (empty -> defaults). */
    inline void parseAddress (const juce::String& text, juce::String& host, int& port)
    {
        const auto trimmed = text.trim();
        host = trimmed.upToFirstOccurrenceOf (":", false, false).trim();
        const int p = trimmed.fromFirstOccurrenceOf (":", false, false).getIntValue();
        port = p > 0 ? p : kDefaultPort;
        if (host.isEmpty())
            host = kDefaultAddress;
    }

    /** Display name as sent on the relay control line ("HOST <ver> <name>" /
        "JOIN <code> <name>"): single line, printable, capped - the relay only
        uses it for its activity log (instead of logging IP addresses). */
    inline juce::String sanitizeName (const juce::String& name)
    {
        auto s = name.trim().removeCharacters ("\r\n");
        return s.substring (0, 32);
    }
}

/** Wire format: every message is  [uint32 bodyLength][uint16 type][body...],
    all little-endian. JSON bodies are UTF-8; audioBlock is binary. */
enum class MsgType : juce::uint16
{
    invalid       = 0,

    // Handshake -------------------------------------------------------------
    hello         = 1,   ///< M->H JSON { name, version }
    helloAck      = 2,   ///< H->M JSON { accepted, hostName, message }

    // Library / transfer ------------------------------------------------------
    songList      = 10,  ///< H->M JSON [ { id, name, sampleRate, lengthSamples, stems:[{id,name,fileName,fileSize}] } ]
    stemRequest   = 11,  ///< M->H JSON { songId, stemId }
    stemBegin     = 12,  ///< H->M JSON { songId, stemId, fileName, fileSize }
    stemChunk     = 13,  ///< H->M binary: raw file bytes of the current transfer
    stemEnd       = 14,  ///< H->M JSON { songId, stemId, ok, error }

    // Jam lifecycle -----------------------------------------------------------
    jamPrepare    = 20,  ///< H->M JSON { jamId, songId, songName, leadSeconds }
    prepareStatus = 21,  ///< M->H JSON { jamId, state:"downloading"|"loading"|"ready"|"error", percent, error }
    jamState      = 22,  ///< H->M JSON { phase, participants:[{name,ready,buffered}] }
    jamCountdown  = 23,  ///< H->M JSON { seconds }
    jamGo         = 24,  ///< H->M JSON { jamId }
    jamStop       = 25,  ///< H->M JSON { reason }

    // Live audio ---------------------------------------------------------------
    audioBlock    = 30,  ///< M->H binary: [int64 startSample][int16 samples...] (mono, song rate, jam time)
    monitorBlock  = 31,  ///< M->H binary: same encoding; soundcheck stream during "Prepare Jam"

    // Voice / chat ---------------------------------------------------------------
    voiceBlock    = 32,  ///< both ways, binary: [u8 nameLen][name][int64 index][int16...] (mono, kVoiceRate)
    chat          = 40,  ///< both ways JSON { name, text }

    // Recording transfer (host -> musician, one file at a time) -------------------
    recordingPrefs     = 50, ///< M->H JSON { autoReceive } - musician's "send me recordings" setting
    recordingOffer     = 51, ///< H->M JSON { recId, song, date, numFiles, totalBytes }
    recordingAnswer    = 52, ///< M->H JSON { recId, accept }
    recordingFileBegin = 53, ///< H->M JSON { recId, fileName, fileSize, fileIndex, numFiles }
    recordingChunk     = 54, ///< H->M binary: raw file bytes of the current transfer
    recordingFileEnd   = 55, ///< H->M JSON { recId, fileName, ok }
    recordingEnd       = 56, ///< H->M JSON { recId, ok, error }

    // Song upload (musician -> host, host must accept the offer first) -------------
    songOffer     = 60, ///< M->H JSON { offerId, name, numFiles, totalBytes }
    songAnswer    = 61, ///< H->M JSON { offerId, accept }
    songFileBegin = 62, ///< M->H JSON { offerId, fileName, fileSize, fileIndex, numFiles }
    songChunk     = 63, ///< M->H binary: raw file bytes of the current transfer
    songFileEnd   = 64, ///< M->H JSON { offerId, fileName, ok }
    songEnd       = 65, ///< M->H JSON { offerId, ok, error }
};

/** Fixed sample rate of the band voice chat (mono, resampled on both ends). */
constexpr double kVoiceRate = 24000.0;

/** One decoded protocol message. */
struct Message
{
    MsgType           type { MsgType::invalid };
    juce::MemoryBlock body;

    juce::var parseJson() const
    {
        return juce::JSON::parse (juce::String::fromUTF8 (static_cast<const char*> (body.getData()),
                                                          (int) body.getSize()));
    }
};

/** Low-level socket send/receive. All functions block; both sides use
    dedicated reader threads and serialise writes with a per-connection lock.

    When a crypto session is passed, every frame is sealed end-to-end as
    [u32 cipherLen][XChaCha20-Poly1305(frame) + MAC] - neither the relay nor
    anyone else on the path can read or tamper with the traffic. */
namespace wire
{
    constexpr juce::uint32 kMaxBodyBytes = 96 * 1024 * 1024;

    /** Writes exactly @p numBytes, looping over partial writes. */
    bool writeAll (juce::StreamingSocket& socket, const void* data, int numBytes);

    /** Reads exactly @p numBytes (blocking). Returns false on close/error. */
    bool readAll (juce::StreamingSocket& socket, void* dest, int numBytes);

    bool send      (juce::StreamingSocket& socket, MsgType type, const void* body,
                    juce::uint32 bodyBytes, crypto::Session* session = nullptr);
    bool sendJson  (juce::StreamingSocket& socket, MsgType type, const juce::var& json,
                    crypto::Session* session = nullptr);

    /** Blocking receive of one full message. Returns false on close/error/desync. */
    bool receive (juce::StreamingSocket& socket, Message& out, crypto::Session* session = nullptr);

    /** Blocking read of one '\n'-terminated ASCII line (relay control phase
        only, so byte-wise reading is fine). Strips the '\n' and any '\r'. */
    bool readLine (juce::StreamingSocket& socket, juce::String& out, int maxLen = 96);

    // -- audioBlock ------------------------------------------------------------
    /** float mono -> int16 body with leading sample index. */
    juce::MemoryBlock encodeAudioBlock (juce::int64 startSample, const float* mono, int numSamples);

    /** Returns false if the body is malformed. Pointers reference @p msg.body. */
    bool decodeAudioBlock (const Message& msg, juce::int64& startSample,
                           const juce::int16*& samples, int& numSamples);

    // -- voiceBlock ---------------------------------------------------------------
    /** Voice chat block with the speaker's name attached (so the host can
        relay it to the other musicians unchanged). */
    juce::MemoryBlock encodeVoiceBlock (const juce::String& speakerName,
                                        juce::int64 startSample, const float* mono, int numSamples);
    juce::MemoryBlock encodeVoiceBlockInt16 (const juce::String& speakerName,
                                             juce::int64 startSample, const juce::int16* samples, int numSamples);
    bool decodeVoiceBlock (const Message& msg, juce::String& speakerName,
                           juce::int64& startSample, const juce::int16*& samples, int& numSamples);
}

} // namespace bandjam
