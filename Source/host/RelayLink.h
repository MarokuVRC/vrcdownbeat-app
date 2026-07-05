#pragma once

#include <juce_core/juce_core.h>
#include "common/Protocol.h"
#include "host/JamServer.h"
#include <thread>
#include <vector>

namespace bandjam
{
/** The host's connection to the room relay (relay/RelayServer.cpp).

    open() connects OUTBOUND to the relay and registers a room; the relay
    answers with a 6-letter room code that musicians type into their Connect
    tab. For every musician who joins, the relay announces a pairing token on
    this control connection; we then open one extra outbound data connection
    per musician and hand it to the JamServer via adoptSocket() - from there
    on it behaves exactly like a directly-accepted client socket.

    No port forwarding, no public IP anywhere on the host's side. */
class RelayLink : private juce::Thread
{
public:
    explicit RelayLink (JamServer& serverToUse);
    ~RelayLink() override;

    /** Async: connects to the relay and opens a room. @p displayName is the
        host's chosen name - the relay logs that instead of any IP address.
        Results via callbacks. */
    void open (const juce::String& relayHost, int relayPort, const juce::String& displayName);
    void close();

    bool isOpen() const noexcept { return active.load(); }
    juce::String getRoomCode() const;

    // -- events (message thread) ------------------------------------------------
    std::function<void (const juce::String& roomCode)> onRoomOpened;
    std::function<void (const juce::String& reason)>   onClosed;

private:
    void run() override;
    bool sendLine (const juce::String& line);

    JamServer& server;
    juce::StreamingSocket control;
    juce::String relayHost;
    juce::String hostName;
    int relayPort { relay::kDefaultPort };

    std::atomic<bool> active { false };
    std::atomic<bool> closing { false };

    mutable juce::CriticalSection codeLock;
    juce::String roomCode;

    // One short-lived thread per joining musician (opens the data connection);
    // joined in close() so they never outlive the server.
    juce::CriticalSection dataThreadLock;
    std::vector<std::thread> dataThreads;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RelayLink)
};

} // namespace bandjam
