#include "JamServer.h"
#include <juce_events/juce_events.h>
#include <map>
#include <vector>

namespace bandjam
{
//==============================================================================
/** One connected musician: owns the socket, runs the reader loop. */
class JamServer::Connection : public juce::Thread
{
public:
    Connection (JamServer& serverToUse, juce::StreamingSocket* socketToOwn)
        : juce::Thread ("bandjam-connection"),
          server (serverToUse), socket (socketToOwn)
    {
        startThread();
    }

    ~Connection() override
    {
        if (recSender != nullptr)
            recSender->stopThread (4000);
        socket->close();
        stopThread (4000);
    }

    juce::String getName() const
    {
        const juce::ScopedLock sl (metaLock);
        return name;
    }

    bool isHandshaked() const noexcept { return handshaked.load(); }
    bool isFinished() const noexcept   { return finished.load(); }
    bool isAutoReceive() const noexcept { return autoReceive.load(); }

    /** Message thread: offers a recorded-jam folder; the send starts once
        the musician accepts (recordingAnswer). */
    bool offerRecording (const juce::File& folder)
    {
        const auto recId = folder.getFileName();

        juce::int64 totalBytes = 0;
        int numFiles = 0;
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false))
        {
            totalBytes += f.getSize();
            ++numFiles;
        }
        if (numFiles == 0)
            return false;

        const auto meta = juce::JSON::parse (folder.getChildFile ("meta.json").loadFileAsString());

        {
            const juce::ScopedLock sl (metaLock);
            offeredRecordings[recId] = folder;
        }

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("recId", recId);
        obj->setProperty ("song", meta.getProperty ("song", recId).toString());
        obj->setProperty ("date", meta.getProperty ("date", juce::String()).toString());
        obj->setProperty ("numFiles", numFiles);
        obj->setProperty ("totalBytes", totalBytes);
        return sendJson (MsgType::recordingOffer, juce::var (obj));
    }

    bool sendJson (MsgType type, const juce::var& json)
    {
        const juce::ScopedLock sl (writeLock);
        return wire::sendJson (*socket, type, json, &cryptoSession);
    }

    bool sendRaw (MsgType type, const void* data, juce::uint32 bytes)
    {
        const juce::ScopedLock sl (writeLock);
        return wire::send (*socket, type, data, bytes, &cryptoSession);
    }

    void closeSocket() { socket->close(); }

    void run() override
    {
        // End-to-end key exchange first - everything after this is encrypted.
        if (! crypto::handshake (*socket, true, cryptoSession) || ! performHandshake())
        {
            finish();
            return;
        }

        Message msg;
        while (! threadShouldExit() && wire::receive (*socket, msg, &cryptoSession))
            if (! handleMessage (msg))
                break;

        finish();
    }

private:
    bool performHandshake()
    {
        Message msg;
        if (! wire::receive (*socket, msg, &cryptoSession) || msg.type != MsgType::hello)
            return false;

        const auto json    = msg.parseJson();
        const auto reqName = json.getProperty ("name", juce::String()).toString().trim();
        const int  version = (int) json.getProperty ("version", 0);

        auto reject = [this] (const juce::String& why)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("accepted", false);
            obj->setProperty ("message", why);
            sendJson (MsgType::helloAck, juce::var (obj));
            return false;
        };

        if (version != kProtocolVersion)
            return reject ("Incompatible program version (host: v" + juce::String (kProtocolVersion) + ").");
        if (reqName.isEmpty())
            return reject ("Name must not be empty.");
        if (server.getClientNames().contains (reqName, true))
            return reject ("The name \"" + reqName + "\" is already taken.");

        {
            const juce::ScopedLock sl (metaLock);
            name = reqName;
        }
        handshaked.store (true);

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("accepted", true);
        obj->setProperty ("hostName", server.getHostName());
        if (! sendJson (MsgType::helloAck, juce::var (obj)))
            return false;

        if (server.getSongListJson)
            sendJson (MsgType::songList, server.getSongListJson());

        // Deliberately no IP address here - it would leak into the visible log.
        server.log (reqName + " connected");
        server.clientsChanged();
        return true;
    }

    bool handleMessage (const Message& msg)
    {
        switch (msg.type)
        {
            case MsgType::stemRequest:
            {
                const auto json   = msg.parseJson();
                const auto songId = json.getProperty ("songId", juce::String()).toString();
                const auto stemId = json.getProperty ("stemId", juce::String()).toString();
                streamStem (songId, stemId);
                return true;
            }

            case MsgType::audioBlock:
            {
                juce::int64 startSample = 0;
                const juce::int16* samples = nullptr;
                int numSamples = 0;
                if (wire::decodeAudioBlock (msg, startSample, samples, numSamples)
                    && server.onAudioBlock)
                {
                    server.onAudioBlock (getName(), startSample, samples, numSamples);
                }
                return true;
            }

            case MsgType::monitorBlock:
            {
                juce::int64 startSample = 0;
                const juce::int16* samples = nullptr;
                int numSamples = 0;
                if (wire::decodeAudioBlock (msg, startSample, samples, numSamples)
                    && server.onMonitorBlock)
                {
                    server.onMonitorBlock (getName(), startSample, samples, numSamples);
                }
                return true;
            }

            case MsgType::voiceBlock:
            {
                juce::String speaker;
                juce::int64 startSample = 0;
                const juce::int16* samples = nullptr;
                int numSamples = 0;
                if (! wire::decodeVoiceBlock (msg, speaker, startSample, samples, numSamples))
                    return true;

                // Relay to everyone else with the sender's name attached, and
                // hand it to the host's own voice playback.
                const auto who = getName();
                const auto block = wire::encodeVoiceBlockInt16 (who, startSample, samples, numSamples);
                server.broadcastRawExcept (who, MsgType::voiceBlock, block.getData(), (juce::uint32) block.getSize());

                if (server.onVoiceBlock)
                    server.onVoiceBlock (who, samples, numSamples);
                return true;
            }

            case MsgType::chat:
            {
                const auto text = msg.parseJson().getProperty ("text", juce::String()).toString().trim();
                if (text.isEmpty())
                    return true;

                const auto who = getName();
                server.broadcastChat (who, text);   // echoes to all clients incl. the sender

                if (auto cb = server.onChat)
                    juce::MessageManager::callAsync ([cb, who, text] { cb (who, text); });
                return true;
            }

            case MsgType::prepareStatus:
            {
                const auto json    = msg.parseJson();
                const auto jamId   = json.getProperty ("jamId", juce::String()).toString();
                const auto state   = json.getProperty ("state", juce::String()).toString();
                const int  percent = (int) json.getProperty ("percent", 0);
                const auto error   = json.getProperty ("error", juce::String()).toString();
                const auto who     = getName();

                if (auto cb = server.onPrepareStatus)
                    juce::MessageManager::callAsync ([cb, who, jamId, state, percent, error]
                                                     { cb (who, jamId, state, percent, error); });
                return true;
            }

            case MsgType::recordingPrefs:
            {
                autoReceive.store ((bool) msg.parseJson().getProperty ("autoReceive", false));
                return true;
            }

            case MsgType::recordingAnswer:
            {
                const auto json   = msg.parseJson();
                const auto recId  = json.getProperty ("recId", juce::String()).toString();
                const bool accept = (bool) json.getProperty ("accept", false);

                juce::File folder;
                {
                    const juce::ScopedLock sl (metaLock);
                    if (const auto it = offeredRecordings.find (recId); it != offeredRecordings.end())
                    {
                        folder = it->second;
                        offeredRecordings.erase (it);
                    }
                }
                if (folder == juce::File())
                    return true;   // never offered / already answered

                if (! accept)
                {
                    server.log (getName() + " declined the recording \"" + recId + "\"");
                    return true;
                }

                enqueueRecordingSend (recId, folder);
                return true;
            }

            default:
                return true; // ignore unknown/unexpected types
        }
    }

    void streamStem (const juce::String& songId, const juce::String& stemId)
    {
        auto sendEnd = [&] (bool ok, const juce::String& error)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("songId", songId);
            obj->setProperty ("stemId", stemId);
            obj->setProperty ("ok", ok);
            if (error.isNotEmpty()) obj->setProperty ("error", error);
            sendJson (MsgType::stemEnd, juce::var (obj));
        };

        const auto file = server.getStemFile ? server.getStemFile (songId, stemId) : juce::File();
        if (file == juce::File() || ! file.existsAsFile())
        {
            sendEnd (false, "File not found.");
            return;
        }

        juce::FileInputStream in (file);
        if (! in.openedOk())
        {
            sendEnd (false, "Could not open the file.");
            return;
        }

        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("songId",   songId);
            obj->setProperty ("stemId",   stemId);
            obj->setProperty ("fileName", file.getFileName());
            obj->setProperty ("fileSize", in.getTotalLength());
            if (! sendJson (MsgType::stemBegin, juce::var (obj)))
                return;
        }

        juce::HeapBlock<char> chunk (kChunkBytes);
        while (! in.isExhausted() && ! threadShouldExit())
        {
            const int got = in.read (chunk.getData(), kChunkBytes);
            if (got <= 0)
                break;
            if (! sendRaw (MsgType::stemChunk, chunk.getData(), (juce::uint32) got))
                return;
        }

        sendEnd (true, {});
        server.log (getName() + " downloaded a stem (" + file.getFileName() + ")");
    }

    // -- recording transfer (dedicated sender thread per connection, so the
    //    reader loop keeps handling chat/voice while files stream out) ---------
    class RecordingSender : public juce::Thread
    {
    public:
        explicit RecordingSender (Connection& c) : juce::Thread ("bandjam-rec-send"), conn (c) {}

        void run() override
        {
            for (;;)
            {
                juce::String recId;
                juce::File folder;
                {
                    const juce::ScopedLock sl (conn.metaLock);
                    if (conn.recSendQueue.empty() || threadShouldExit())
                        return;
                    recId  = conn.recSendQueue.front().first;
                    folder = conn.recSendQueue.front().second;
                }

                conn.sendRecordingFolder (*this, recId, folder);

                const juce::ScopedLock sl (conn.metaLock);
                if (! conn.recSendQueue.empty())
                    conn.recSendQueue.erase (conn.recSendQueue.begin());
            }
        }

        Connection& conn;
    };

    void enqueueRecordingSend (const juce::String& recId, const juce::File& folder)
    {
        {
            const juce::ScopedLock sl (metaLock);
            recSendQueue.emplace_back (recId, folder);
        }

        if (recSender == nullptr)
            recSender = std::make_unique<RecordingSender> (*this);
        if (! recSender->isThreadRunning())
            recSender->startThread();
    }

    void sendRecordingFolder (juce::Thread& runner, const juce::String& recId, const juce::File& folder)
    {
        auto sendEnd = [&] (bool ok, const juce::String& error)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("recId", recId);
            obj->setProperty ("ok", ok);
            if (error.isNotEmpty()) obj->setProperty ("error", error);
            sendJson (MsgType::recordingEnd, juce::var (obj));
        };

        const auto files = folder.findChildFiles (juce::File::findFiles, false);
        if (files.isEmpty())
        {
            sendEnd (false, "The recording folder is empty.");
            return;
        }

        server.log ("Sending recording \"" + recId + "\" to " + getName() + "...");

        juce::HeapBlock<char> chunk (kChunkBytes);
        int index = 0;

        for (const auto& file : files)
        {
            if (runner.threadShouldExit())
                return;

            juce::FileInputStream in (file);
            if (! in.openedOk())
            {
                sendEnd (false, "Could not open " + file.getFileName());
                return;
            }

            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("recId", recId);
                obj->setProperty ("fileName", file.getFileName());
                obj->setProperty ("fileSize", in.getTotalLength());
                obj->setProperty ("fileIndex", index);
                obj->setProperty ("numFiles", files.size());
                if (! sendJson (MsgType::recordingFileBegin, juce::var (obj)))
                    return;
            }

            while (! in.isExhausted() && ! runner.threadShouldExit())
            {
                const int got = in.read (chunk.getData(), kChunkBytes);
                if (got <= 0)
                    break;
                if (! sendRaw (MsgType::recordingChunk, chunk.getData(), (juce::uint32) got))
                    return;
            }

            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("recId", recId);
                obj->setProperty ("fileName", file.getFileName());
                obj->setProperty ("ok", true);
                if (! sendJson (MsgType::recordingFileEnd, juce::var (obj)))
                    return;
            }
            ++index;
        }

        sendEnd (true, {});
        server.log ("Recording \"" + recId + "\" sent to " + getName());
    }

    void finish()
    {
        const bool wasHandshaked = handshaked.exchange (false);
        finished.store (true);

        if (wasHandshaked)
        {
            server.log (getName() + " disconnected");
            server.clientsChanged();
        }
    }

    static constexpr int kChunkBytes = 256 * 1024;

    JamServer& server;
    std::unique_ptr<juce::StreamingSocket> socket;

    mutable juce::CriticalSection metaLock;
    juce::String name;

    juce::CriticalSection writeLock;
    crypto::Session cryptoSession;
    std::atomic<bool> handshaked  { false };
    std::atomic<bool> finished    { false };
    std::atomic<bool> autoReceive { false };

    std::map<juce::String, juce::File> offeredRecordings;             ///< metaLock
    std::vector<std::pair<juce::String, juce::File>> recSendQueue;    ///< metaLock
    std::unique_ptr<RecordingSender> recSender;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Connection)
};

//==============================================================================
JamServer::JamServer() : juce::Thread ("bandjam-server") {}

JamServer::~JamServer()
{
    stop();
}

bool JamServer::start (int portToUse, juce::String& error)
{
    stop();

    listener = std::make_unique<juce::StreamingSocket>();
    if (! listener->createListener (portToUse))
    {
        error = "Could not open port " + juce::String (portToUse) + " "
                "(already in use or blocked by the firewall?).";
        listener.reset();
        return false;
    }

    port = portToUse;
    running.store (true);
    startThread();
    log ("Server running on port " + juce::String (port));
    return true;
}

void JamServer::startVirtual()
{
    stop();
    running.store (true);
    log ("Ready for relay connections");
}

void JamServer::adoptSocket (juce::StreamingSocket* socketToOwn)
{
    if (! running.load())
    {
        delete socketToOwn;
        return;
    }

    reapFinishedConnections();

    const juce::ScopedLock sl (connectionLock);
    connections.add (new Connection (*this, socketToOwn));
}

void JamServer::stop()
{
    if (! running.exchange (false) && listener == nullptr)
        return;

    signalThreadShouldExit();
    if (listener != nullptr)
        listener->close();
    stopThread (4000);
    listener.reset();

    {
        const juce::ScopedLock sl (connectionLock);
        connections.clear(); // destructors close sockets + join threads
    }
    clientsChanged();
    log ("Server stopped");
}

void JamServer::run()
{
    while (! threadShouldExit() && listener != nullptr)
    {
        auto* incoming = listener->waitForNextConnection(); // unblocked by listener->close()
        if (incoming == nullptr)
        {
            juce::Thread::sleep (50); // closed or listener error - avoid a busy loop
            continue;
        }

        if (threadShouldExit())
        {
            delete incoming;
            break;
        }

        reapFinishedConnections();

        const juce::ScopedLock sl (connectionLock);
        connections.add (new Connection (*this, incoming));
    }
}

void JamServer::reapFinishedConnections()
{
    const juce::ScopedLock sl (connectionLock);
    for (int i = connections.size(); --i >= 0;)
        if (connections.getUnchecked (i)->isFinished())
            connections.remove (i);
}

juce::StringArray JamServer::getClientNames() const
{
    juce::StringArray names;
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished())
            names.add (c->getName());
    return names;
}

void JamServer::broadcastJson (MsgType type, const juce::var& json)
{
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished())
            c->sendJson (type, json);
}

bool JamServer::sendJsonTo (const juce::String& clientName, MsgType type, const juce::var& json)
{
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished() && c->getName().equalsIgnoreCase (clientName))
            return c->sendJson (type, json);
    return false;
}

void JamServer::broadcastSongList()
{
    if (getSongListJson)
        broadcastJson (MsgType::songList, getSongListJson());
}

void JamServer::broadcastChat (const juce::String& name, const juce::String& text)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", name);
    obj->setProperty ("text", text);
    broadcastJson (MsgType::chat, juce::var (obj));
}

void JamServer::broadcastVoice (const juce::String& name, juce::int64 startSample,
                                const float* mono, int numSamples)
{
    const auto block = wire::encodeVoiceBlock (name, startSample, mono, numSamples);
    broadcastRawExcept ({}, MsgType::voiceBlock, block.getData(), (juce::uint32) block.getSize());
}

void JamServer::broadcastRawExcept (const juce::String& excludedName, MsgType type,
                                    const void* data, juce::uint32 bytes)
{
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished()
            && (excludedName.isEmpty() || ! c->getName().equalsIgnoreCase (excludedName)))
            c->sendRaw (type, data, bytes);
}

int JamServer::offerRecording (const juce::String& clientName, const juce::File& folder)
{
    int sent = 0;
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished()
            && (clientName.isEmpty() || c->getName().equalsIgnoreCase (clientName)))
            if (c->offerRecording (folder))
                ++sent;
    return sent;
}

int JamServer::offerRecordingToAutoReceivers (const juce::File& folder)
{
    int sent = 0;
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished() && c->isAutoReceive())
            if (c->offerRecording (folder))
                ++sent;
    return sent;
}

void JamServer::disconnectClient (const juce::String& clientName)
{
    const juce::ScopedLock sl (connectionLock);
    for (auto* c : connections)
        if (c->isHandshaked() && ! c->isFinished() && c->getName().equalsIgnoreCase (clientName))
            c->closeSocket();
}

void JamServer::setHostName (const juce::String& name)
{
    const juce::ScopedLock sl (hostNameLock);
    hostName = name;
}

juce::String JamServer::getHostName() const
{
    const juce::ScopedLock sl (hostNameLock);
    return hostName.isNotEmpty() ? hostName : "Host";
}

void JamServer::log (const juce::String& line)
{
    if (onLog == nullptr)
        return;

    juce::MessageManager::callAsync ([cb = onLog, line] { cb (line); });
}

void JamServer::clientsChanged()
{
    if (onClientsChanged == nullptr)
        return;

    juce::MessageManager::callAsync ([cb = onClientsChanged] { cb(); });
}

} // namespace bandjam
