#pragma once

#include <juce_core/juce_core.h>
#include "common/Protocol.h"

namespace bandjam
{
/** The host's TCP server. Accepts musician connections, answers the handshake,
    serves the song list and stem downloads, relays jam messages and hands
    incoming live-audio blocks straight to the mix engine.

    Threading: one accept thread plus one reader thread per connection.
    All std::function event callbacks marked "message thread" are marshalled;
    onAudioBlock is called directly on the connection's reader thread. */
class JamServer : private juce::Thread
{
public:
    JamServer();
    ~JamServer() override;

    bool start (int port, juce::String& error);

    /** Relay mode: no listening socket - connections come in from the room
        relay via adoptSocket(). Everything else works exactly the same. */
    void startVirtual();

    /** Adds an externally-connected socket (a paired relay data connection)
        as a regular client. Takes ownership. Any thread. */
    void adoptSocket (juce::StreamingSocket* socketToOwn);

    void stop();
    bool isRunning() const noexcept { return running.load(); }
    int  getPort() const noexcept   { return port; }

    /** The host's chosen display name (sent in helloAck, used for chat/voice).
        Set before start(); getter is safe from any thread. */
    void setHostName (const juce::String& name);
    juce::String getHostName() const;

    juce::StringArray getClientNames() const;

    void broadcastJson (MsgType type, const juce::var& json);
    bool sendJsonTo (const juce::String& clientName, MsgType type, const juce::var& json);
    void broadcastSongList();

    /** Chat line from the host (also echoed to onChat by the caller's UI). */
    void broadcastChat (const juce::String& name, const juce::String& text);

    /** Host's own voice (any thread - typically the VoiceChat sender). */
    void broadcastVoice (const juce::String& name, juce::int64 startSample,
                         const float* mono, int numSamples);

    /** Kicks a connection (e.g. after it misbehaved). */
    void disconnectClient (const juce::String& clientName);

    // -- recording transfer -------------------------------------------------------
    /** Offers a recorded-jam folder to one musician (empty name = everyone).
        The musician answers with recordingAnswer; on accept the folder's
        files are streamed on a dedicated per-connection sender thread (so
        chat/voice keep flowing). Returns the number of offers sent. */
    int offerRecording (const juce::String& clientName, const juce::File& folder);

    /** Offers the folder to every musician whose "auto receive recordings"
        preference is on. Returns the number of offers sent. */
    int offerRecordingToAutoReceivers (const juce::File& folder);

    // -- wiring (set before start) -------------------------------------------
    std::function<juce::var()> getSongListJson;                                        ///< any thread
    std::function<juce::File (const juce::String& songId,
                              const juce::String& stemId)> getStemFile;               ///< reader thread

    // -- events (message thread) ----------------------------------------------
    std::function<void (const juce::String& line)> onLog;
    std::function<void()>                          onClientsChanged;
    std::function<void (const juce::String& clientName, const juce::String& jamId,
                        const juce::String& state, int percent,
                        const juce::String& error)> onPrepareStatus;

    std::function<void (const juce::String& name, const juce::String& text)> onChat;

    // -- events (reader thread, live path) --------------------------------------
    std::function<void (const juce::String& clientName, juce::int64 startSample,
                        const juce::int16* samples, int numSamples)> onAudioBlock;
    std::function<void (const juce::String& clientName, juce::int64 startSample,
                        const juce::int16* samples, int numSamples)> onMonitorBlock;
    std::function<void (const juce::String& clientName,
                        const juce::int16* samples, int numSamples)> onVoiceBlock;

private:
    class Connection;

    void run() override;                       // accept loop
    void reapFinishedConnections();            // any thread (locks)
    void log (const juce::String& line);
    void clientsChanged();
    void broadcastRawExcept (const juce::String& excludedName, MsgType type,
                             const void* data, juce::uint32 bytes);

    std::unique_ptr<juce::StreamingSocket> listener;
    juce::OwnedArray<Connection> connections;
    mutable juce::CriticalSection connectionLock;

    std::atomic<bool> running { false };
    int port { kDefaultPort };

    mutable juce::CriticalSection hostNameLock;
    juce::String hostName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JamServer)
};

} // namespace bandjam
