#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "common/Protocol.h"
#include <map>

namespace bandjam
{
/** The musician's connection to a host: connect + handshake, song list,
    one-at-a-time stem downloads and jam messages.

    All on*-callbacks fire on the message thread. sendAudioBlock() may be
    called from any thread (the capture sender thread during a jam). */
class HostConnection : private juce::Thread
{
public:
    HostConnection();
    ~HostConnection() override;

    /** Async: starts the connect/handshake thread. Progress via callbacks. */
    void connectTo (const juce::String& host, int port, const juce::String& musicianName);

    /** Async: connects through the room relay instead of directly to the
        host. After the relay pairs us with the host, the socket carries the
        normal protocol - everything else behaves exactly like connectTo(). */
    void connectViaRelay (const juce::String& relayHost, int relayPort,
                          const juce::String& roomCode, const juce::String& musicianName);

    void disconnect();
    bool isConnected() const noexcept { return connected.load(); }
    juce::String getMusicianName() const { return musicianName; }

    bool sendJson (MsgType type, const juce::var& json);
    bool sendAudioBlock (juce::int64 startSample, const float* mono, int numSamples);
    bool sendMonitorBlock (juce::int64 startSample, const float* mono, int numSamples);
    bool sendChat (const juce::String& text);
    bool sendVoiceBlock (juce::int64 startSample, const float* mono, int numSamples);

    /** Requests one stem; the reader thread writes it to @p destFile.
        Only one transfer at a time - wait for onStemEnd before the next. */
    bool requestStem (const juce::String& songId, const juce::String& stemId,
                      const juce::File& destFile);

    /** Tells the host whether we want jam recordings offered automatically. */
    bool sendRecordingPrefs (bool autoReceive);

    /** Answers a recording offer (recId from onRecordingOffer). On accept the
        host streams the folder; files land under appdata/recordings/<recId>. */
    bool sendRecordingAnswer (const juce::String& recId, bool accept);

    // -- events (message thread) ------------------------------------------------
    std::function<void (const juce::String& hostName)> onConnected;
    std::function<void (const juce::String& reason)>   onDisconnected;
    std::function<void (juce::var songList)>           onSongList;
    std::function<void (const juce::String& songId, const juce::String& stemId,
                        juce::int64 received, juce::int64 total)> onStemProgress;
    std::function<void (const juce::String& songId, const juce::String& stemId,
                        bool ok, const juce::String& error)> onStemEnd;
    std::function<void (juce::var)>                    onJamPrepare;
    std::function<void (juce::var)>                    onJamState;
    std::function<void (int seconds)>                  onJamCountdown;
    std::function<void (juce::var)>                    onJamGo;
    std::function<void (const juce::String& reason)>   onJamStop;
    std::function<void (const juce::String& name, const juce::String& text)> onChat;

    /** The host offers a recording: JSON { recId, song, date, numFiles, totalBytes }. */
    std::function<void (juce::var offer)> onRecordingOffer;
    std::function<void (const juce::String& recId, juce::int64 receivedBytes,
                        juce::int64 totalBytes)> onRecordingProgress;
    std::function<void (const juce::String& recId, bool ok, const juce::String& error,
                        const juce::File& folder)> onRecordingEnd;

    /** Reader thread (live path): a remote speaker's voice block. */
    std::function<void (const juce::String& speakerName,
                        const juce::int16* samples, int numSamples)> onVoiceBlock;

private:
    void run() override;
    bool sendBlock (MsgType type, juce::int64 startSample, const float* mono, int numSamples);
    bool performHandshake (juce::String& failReason);
    void handleMessage (const Message& msg);
    void finishTransfer (bool ok, const juce::String& error,
                         const juce::String& songId, const juce::String& stemId);

    template <typename Fn>
    static void onMessageThread (Fn&& fn) { juce::MessageManager::callAsync (std::forward<Fn> (fn)); }

    juce::StreamingSocket socket;
    juce::CriticalSection writeLock;
    crypto::Session cryptoSession;    ///< end-to-end encryption (host <-> us)

    juce::String host, musicianName;
    int port { kDefaultPort };
    juce::String roomCode;        ///< non-empty = connect via the relay

    std::atomic<bool> connected { false };

    // Current download (reader thread only, except pending* set from message thread pre-request)
    juce::CriticalSection transferLock;
    juce::String pendingSongId, pendingStemId;
    juce::File   pendingDest;
    std::unique_ptr<juce::FileOutputStream> transferStream;
    juce::String transferSongId, transferStemId;
    juce::int64  transferTotal    { 0 };
    juce::int64  transferReceived { 0 };

    // Incoming recording transfer (reader thread only). Files are written to a
    // ".part" folder which is renamed into place when the transfer succeeds.
    std::map<juce::String, juce::int64> recOfferedTotals;
    juce::String recRecId;
    juce::File   recTempFolder;
    std::unique_ptr<juce::FileOutputStream> recStream;
    juce::int64  recReceivedBytes { 0 };
    juce::int64  recTotalBytes    { 0 };
    juce::uint32 recLastProgressMs { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostConnection)
};

} // namespace bandjam
