#include "RelayLink.h"
#include <juce_events/juce_events.h>
#include <cstring>
#include <thread>

namespace bandjam
{
RelayLink::RelayLink (JamServer& serverToUse)
    : juce::Thread ("bandjam-relay-link"), server (serverToUse)
{
}

RelayLink::~RelayLink()
{
    close();
}

void RelayLink::open (const juce::String& hostToUse, int portToUse, const juce::String& displayName)
{
    close();

    relayHost = hostToUse.trim();
    relayPort = portToUse;
    hostName  = relay::sanitizeName (displayName);
    closing.store (false);
    startThread();
}

void RelayLink::close()
{
    closing.store (true);
    signalThreadShouldExit();
    control.close();
    stopThread (4000);
    active.store (false);

    {
        std::vector<std::thread> pending;
        {
            const juce::ScopedLock sl (dataThreadLock);
            pending.swap (dataThreads);
        }
        for (auto& t : pending)
            if (t.joinable())
                t.join();
    }

    const juce::ScopedLock sl (codeLock);
    roomCode.clear();
}

juce::String RelayLink::getRoomCode() const
{
    const juce::ScopedLock sl (codeLock);
    return roomCode;
}

bool RelayLink::sendLine (const juce::String& line)
{
    const auto utf8 = line.toRawUTF8();
    return control.write (utf8, (int) std::strlen (utf8)) == (int) std::strlen (utf8);
}

void RelayLink::run()
{
    auto reportClosed = [this] (const juce::String& reason)
    {
        if (closing.load())
            return;   // deliberate close - no callback
        if (auto cb = onClosed)
            juce::MessageManager::callAsync ([cb, reason] { cb (reason); });
    };

    if (! control.connect (relayHost, relayPort, 5000))
    {
        reportClosed ("Relay server not reachable (" + relayHost + ":" + juce::String (relayPort) + ").");
        return;
    }

    // The name lets the relay log "room opened by <name>" instead of an IP.
    if (! sendLine ("HOST " + juce::String (kProtocolVersion)
                    + (hostName.isNotEmpty() ? " " + hostName : juce::String()) + "\n"))
    {
        reportClosed ("Could not talk to the relay server.");
        return;
    }

    juce::String line;
    if (! wire::readLine (control, line) || ! line.startsWith ("ROOM "))
    {
        reportClosed (line.startsWith ("ERR") ? "Relay refused: " + line.substring (4)
                                              : "Unexpected answer from the relay server.");
        return;
    }

    const auto code = line.substring (5).trim();
    {
        const juce::ScopedLock sl (codeLock);
        roomCode = code;
    }
    active.store (true);

    if (auto cb = onRoomOpened)
        juce::MessageManager::callAsync ([cb, code] { cb (code); });

    // Control loop: the relay pushes PING keep-alives and one PEER line per
    // joining musician. For each PEER we open a fresh outbound data
    // connection and hand it to the JamServer.
    while (! threadShouldExit() && wire::readLine (control, line))
    {
        if (! line.startsWith ("PEER "))
            continue;   // PING or anything else

        const auto token = line.substring (5).trim();
        const auto host  = relayHost;
        const auto port  = relayPort;

        std::thread worker ([&serverRef = server, host, port, token]
        {
            auto socket = std::make_unique<juce::StreamingSocket>();
            if (! socket->connect (host, port, 5000))
                return;

            const auto greeting = "DATA " + token + "\n";
            const auto* utf8 = greeting.toRawUTF8();
            if (socket->write (utf8, (int) std::strlen (utf8)) != (int) std::strlen (utf8))
                return;

            serverRef.adoptSocket (socket.release());
        });

        {
            // Kept until close(); one entry per join costs nothing and
            // guarantees no worker outlives the JamServer.
            const juce::ScopedLock sl (dataThreadLock);
            dataThreads.push_back (std::move (worker));
        }
    }

    active.store (false);
    reportClosed ("Connection to the relay server was lost.");
}

} // namespace bandjam
