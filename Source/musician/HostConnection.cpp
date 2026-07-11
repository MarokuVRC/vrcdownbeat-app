#include "HostConnection.h"
#include "common/AppPaths.h"
#include "common/Settings.h"
#include <cstring>

namespace bandjam
{
/** Streams the offered song files to the host once it accepted, so the
    reader loop and the UI stay responsive during the upload. */
class HostConnection::SongUploader : public juce::Thread
{
public:
    explicit SongUploader (HostConnection& c) : juce::Thread ("bandjam-song-upload"), conn (c) {}
    void run() override { conn.runSongUpload (*this); }

private:
    HostConnection& conn;
};

HostConnection::HostConnection() : juce::Thread ("bandjam-hostconnection") {}

HostConnection::~HostConnection()
{
    disconnect();
}

void HostConnection::connectTo (const juce::String& hostToUse, int portToUse,
                                const juce::String& nameToUse)
{
    disconnect();

    host         = hostToUse.trim();
    port         = portToUse;
    musicianName = nameToUse.trim();
    roomCode.clear();
    startThread();
}

void HostConnection::connectViaRelay (const juce::String& relayHost, int relayPort,
                                      const juce::String& codeToUse, const juce::String& nameToUse)
{
    disconnect();

    host         = relayHost.trim();
    port         = relayPort;
    musicianName = nameToUse.trim();
    roomCode     = codeToUse.trim().toUpperCase();
    startThread();
}

void HostConnection::disconnect()
{
    signalThreadShouldExit();
    socket.close();
    stopThread (4000);
    connected.store (false);

    if (songUploader != nullptr)
        songUploader->stopThread (4000);
    {
        const juce::ScopedLock sl (uploadLock);
        upOfferId.clear();
        upSongName.clear();
        upFiles.clear();
        upTotalBytes = 0;
    }
}

void HostConnection::run()
{
    juce::String failReason;

    if (! socket.connect (host, port, 5000))
    {
        const auto what = roomCode.isNotEmpty() ? juce::String ("Relay server not reachable.")
                                                : juce::String ("Connection failed (host not reachable).");
        onMessageThread ([cb = onDisconnected, what] { if (cb) cb (what); });
        return;
    }

    // Room mode: ask the relay to pair us with the host, then continue with
    // the normal protocol over the same socket.
    if (roomCode.isNotEmpty())
    {
        // The name lets the relay log "<name> joined" instead of an IP.
        const auto safeName = relay::sanitizeName (musicianName);
        const auto join = "JOIN " + roomCode
                          + (safeName.isNotEmpty() ? " " + safeName : juce::String()) + "\n";
        const auto* utf8 = join.toRawUTF8();
        juce::String reply;

        if (socket.write (utf8, (int) std::strlen (utf8)) != (int) std::strlen (utf8)
            || ! wire::readLine (socket, reply))
        {
            socket.close();
            onMessageThread ([cb = onDisconnected]
                             { if (cb) cb ("Relay server did not answer."); });
            return;
        }

        if (reply != "OK")
        {
            socket.close();
            auto why = reply == "ERR no-such-room"
                           ? juce::String ("Room \"" + roomCode + "\" not found - check the code with your host.")
                           : reply == "ERR busy"
                                 ? juce::String ("The room is full right now - try again in a moment.")
                                 : "Relay refused the connection (" + reply + ").";
            onMessageThread ([cb = onDisconnected, why] { if (cb) cb (why); });
            return;
        }
    }

    // End-to-end key exchange first - everything after this is encrypted
    // (the relay in between only ever sees ciphertext).
    cryptoSession = {};
    if (! crypto::handshake (socket, false, cryptoSession))
    {
        socket.close();
        onMessageThread ([cb = onDisconnected]
                         { if (cb) cb ("Secure handshake failed - is the host up to date?"); });
        return;
    }

    if (! performHandshake (failReason))
    {
        socket.close();
        onMessageThread ([cb = onDisconnected, failReason]
                         { if (cb) cb (failReason.isNotEmpty() ? failReason : "Sign-in rejected."); });
        return;
    }

    connected.store (true);

    Message msg;
    while (! threadShouldExit() && wire::receive (socket, msg, &cryptoSession))
        handleMessage (msg);

    const bool wasConnected = connected.exchange (false);
    socket.close();

    // Abort a half-finished download so no corrupt file stays behind.
    {
        const juce::ScopedLock sl (transferLock);
        if (transferStream != nullptr)
        {
            transferStream.reset();
            pendingDest.deleteFile();
        }
    }

    // Abort a half-finished recording transfer (reader thread state).
    recStream.reset();
    if (recTempFolder != juce::File())
        recTempFolder.deleteRecursively();
    recTempFolder = juce::File();
    recRecId.clear();
    recOfferedTotals.clear();

    // Drop a pending song offer - the uploader (if running) notices the
    // closed connection on its next send and exits.
    {
        const juce::ScopedLock sl (uploadLock);
        upOfferId.clear();
        upSongName.clear();
        upFiles.clear();
        upTotalBytes = 0;
    }

    if (wasConnected)
        onMessageThread ([cb = onDisconnected]
                         { if (cb) cb ("Connection lost."); });
}

bool HostConnection::performHandshake (juce::String& failReason)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", musicianName);
    obj->setProperty ("version", kProtocolVersion);
    if (! wire::sendJson (socket, MsgType::hello, juce::var (obj), &cryptoSession))
    {
        failReason = "Send failed.";
        return false;
    }

    Message msg;
    if (! wire::receive (socket, msg, &cryptoSession) || msg.type != MsgType::helloAck)
    {
        failReason = "No answer from the host.";
        return false;
    }

    const auto json = msg.parseJson();
    if (! (bool) json.getProperty ("accepted", false))
    {
        failReason = json.getProperty ("message", "Sign-in rejected.").toString();
        return false;
    }

    const auto hostName = json.getProperty ("hostName", "Host").toString();
    onMessageThread ([cb = onConnected, hostName] { if (cb) cb (hostName); });
    return true;
}

void HostConnection::handleMessage (const Message& msg)
{
    switch (msg.type)
    {
        case MsgType::songList:
        {
            const auto json = msg.parseJson();
            onMessageThread ([cb = onSongList, json] { if (cb) cb (json); });
            break;
        }

        case MsgType::stemBegin:
        {
            const auto json   = msg.parseJson();
            const auto songId = json.getProperty ("songId", juce::String()).toString();
            const auto stemId = json.getProperty ("stemId", juce::String()).toString();

            const juce::ScopedLock sl (transferLock);
            if (songId == pendingSongId && stemId == pendingStemId && pendingDest != juce::File())
            {
                pendingDest.getParentDirectory().createDirectory();
                pendingDest.deleteFile();
                transferStream = std::make_unique<juce::FileOutputStream> (pendingDest);
                if (! transferStream->openedOk())
                    transferStream.reset();

                transferSongId   = songId;
                transferStemId   = stemId;
                transferTotal    = (juce::int64) json.getProperty ("fileSize", 0);
                transferReceived = 0;
            }
            break;
        }

        case MsgType::stemChunk:
        {
            const juce::ScopedLock sl (transferLock);
            if (transferStream != nullptr && msg.body.getSize() > 0)
            {
                transferStream->write (msg.body.getData(), msg.body.getSize());
                transferReceived += (juce::int64) msg.body.getSize();

                onMessageThread ([cb = onStemProgress, songId = transferSongId,
                                  stemId = transferStemId, got = transferReceived, total = transferTotal]
                                 { if (cb) cb (songId, stemId, got, total); });
            }
            break;
        }

        case MsgType::stemEnd:
        {
            const auto json   = msg.parseJson();
            const auto songId = json.getProperty ("songId", juce::String()).toString();
            const auto stemId = json.getProperty ("stemId", juce::String()).toString();
            const bool ok     = (bool) json.getProperty ("ok", false);
            const auto error  = json.getProperty ("error", juce::String()).toString();
            finishTransfer (ok, error, songId, stemId);
            break;
        }

        case MsgType::jamPrepare:
        {
            const auto json = msg.parseJson();
            onMessageThread ([cb = onJamPrepare, json] { if (cb) cb (json); });
            break;
        }

        case MsgType::jamState:
        {
            const auto json = msg.parseJson();
            onMessageThread ([cb = onJamState, json] { if (cb) cb (json); });
            break;
        }

        case MsgType::jamCountdown:
        {
            const int seconds = (int) msg.parseJson().getProperty ("seconds", 3);
            onMessageThread ([cb = onJamCountdown, seconds] { if (cb) cb (seconds); });
            break;
        }

        case MsgType::jamGo:
        {
            const auto json = msg.parseJson();
            onMessageThread ([cb = onJamGo, json] { if (cb) cb (json); });
            break;
        }

        case MsgType::jamStop:
        {
            const auto reason = msg.parseJson().getProperty ("reason", juce::String()).toString();
            onMessageThread ([cb = onJamStop, reason] { if (cb) cb (reason); });
            break;
        }

        case MsgType::chat:
        {
            const auto json = msg.parseJson();
            const auto name = json.getProperty ("name", juce::String()).toString();
            const auto text = json.getProperty ("text", juce::String()).toString();
            onMessageThread ([cb = onChat, name, text] { if (cb) cb (name, text); });
            break;
        }

        case MsgType::voiceBlock:
        {
            juce::String speaker;
            juce::int64 startSample = 0;
            const juce::int16* samples = nullptr;
            int numSamples = 0;
            if (wire::decodeVoiceBlock (msg, speaker, startSample, samples, numSamples) && onVoiceBlock)
                onVoiceBlock (speaker, samples, numSamples);   // reader thread, by design
            break;
        }

        case MsgType::songAnswer:
        {
            const auto json    = msg.parseJson();
            const auto offerId = json.getProperty ("offerId", juce::String()).toString();
            const bool accept  = (bool) json.getProperty ("accept", false);

            bool matches = false;
            {
                const juce::ScopedLock sl (uploadLock);
                matches = offerId.isNotEmpty() && offerId == upOfferId;
                if (matches && ! accept)
                {
                    upOfferId.clear();
                    upSongName.clear();
                    upFiles.clear();
                    upTotalBytes = 0;
                }
            }
            if (! matches)
                break;

            onMessageThread ([cb = onSongOfferAnswer, accept] { if (cb) cb (accept); });

            if (accept)
            {
                if (songUploader == nullptr)
                    songUploader = std::make_unique<SongUploader> (*this);
                if (! songUploader->isThreadRunning())
                    songUploader->startThread();
            }
            break;
        }

        case MsgType::recordingOffer:
        {
            const auto json  = msg.parseJson();
            const auto recId = json.getProperty ("recId", juce::String()).toString();
            recOfferedTotals[recId] = (juce::int64) json.getProperty ("totalBytes", 0);
            onMessageThread ([cb = onRecordingOffer, json] { if (cb) cb (json); });
            break;
        }

        case MsgType::recordingFileBegin:
        {
            const auto json  = msg.parseJson();
            const auto recId = json.getProperty ("recId", juce::String()).toString();
            if (recId.isEmpty())
                break;

            if (recId != recRecId)
            {
                // First file of a new transfer: start a fresh ".part" folder.
                recRecId      = recId;
                recTempFolder = settings::recordingsFolder().getChildFile (juce::File::createLegalFileName (recId) + ".part");
                recTempFolder.deleteRecursively();
                recTempFolder.createDirectory();
                recReceivedBytes = 0;
                const auto it = recOfferedTotals.find (recId);
                recTotalBytes = it != recOfferedTotals.end() ? it->second : 0;
            }

            recStream.reset();
            const auto fileName = json.getProperty ("fileName", juce::String()).toString();
            const auto dest = recTempFolder.getChildFile (juce::File::createLegalFileName (fileName));
            dest.deleteFile();
            recStream = std::make_unique<juce::FileOutputStream> (dest);
            if (! recStream->openedOk())
                recStream.reset();
            break;
        }

        case MsgType::recordingChunk:
        {
            if (recStream != nullptr && msg.body.getSize() > 0)
            {
                recStream->write (msg.body.getData(), msg.body.getSize());
                recReceivedBytes += (juce::int64) msg.body.getSize();

                const auto now = juce::Time::getMillisecondCounter();
                if (now - recLastProgressMs >= 200)
                {
                    recLastProgressMs = now;
                    onMessageThread ([cb = onRecordingProgress, recId = recRecId,
                                      got = recReceivedBytes, total = recTotalBytes]
                                     { if (cb) cb (recId, got, total); });
                }
            }
            break;
        }

        case MsgType::recordingFileEnd:
        {
            if (recStream != nullptr)
            {
                recStream->flush();
                recStream.reset();
            }
            break;
        }

        case MsgType::recordingEnd:
        {
            const auto json  = msg.parseJson();
            const auto recId = json.getProperty ("recId", juce::String()).toString();
            bool ok          = (bool) json.getProperty ("ok", false);
            auto error       = json.getProperty ("error", juce::String()).toString();
            recStream.reset();

            juce::File finalFolder;
            if (ok && recTempFolder != juce::File())
            {
                finalFolder = settings::recordingsFolder().getChildFile (juce::File::createLegalFileName (recId));
                finalFolder.deleteRecursively();
                if (! recTempFolder.moveFileTo (finalFolder))
                {
                    ok = false;
                    error = "Could not move the received recording into place.";
                    recTempFolder.deleteRecursively();
                    finalFolder = juce::File();
                }
            }
            else if (recTempFolder != juce::File())
            {
                recTempFolder.deleteRecursively();
            }

            recOfferedTotals.erase (recId);
            recRecId.clear();
            recTempFolder = juce::File();

            onMessageThread ([cb = onRecordingEnd, recId, ok, error, finalFolder]
                             { if (cb) cb (recId, ok, error, finalFolder); });
            break;
        }

        default:
            break;
    }
}

void HostConnection::finishTransfer (bool ok, const juce::String& error,
                                     const juce::String& songId, const juce::String& stemId)
{
    {
        const juce::ScopedLock sl (transferLock);
        if (transferStream != nullptr)
        {
            transferStream->flush();
            transferStream.reset();
        }
        if (! ok && pendingDest != juce::File())
            pendingDest.deleteFile();

        pendingSongId.clear();
        pendingStemId.clear();
        pendingDest = juce::File();
    }

    onMessageThread ([cb = onStemEnd, songId, stemId, ok, error]
                     { if (cb) cb (songId, stemId, ok, error); });
}

bool HostConnection::sendJson (MsgType type, const juce::var& json)
{
    if (! connected.load())
        return false;

    const juce::ScopedLock sl (writeLock);
    return wire::sendJson (socket, type, json, &cryptoSession);
}

bool HostConnection::sendBlock (MsgType type, juce::int64 startSample, const float* mono, int numSamples)
{
    if (! connected.load() || numSamples <= 0)
        return false;

    const auto block = wire::encodeAudioBlock (startSample, mono, numSamples);
    const juce::ScopedLock sl (writeLock);
    return wire::send (socket, type, block.getData(), (juce::uint32) block.getSize(), &cryptoSession);
}

bool HostConnection::sendAudioBlock (juce::int64 startSample, const float* mono, int numSamples)
{
    return sendBlock (MsgType::audioBlock, startSample, mono, numSamples);
}

bool HostConnection::sendMonitorBlock (juce::int64 startSample, const float* mono, int numSamples)
{
    return sendBlock (MsgType::monitorBlock, startSample, mono, numSamples);
}

bool HostConnection::sendChat (const juce::String& text)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("text", text);
    return sendJson (MsgType::chat, juce::var (obj));
}

bool HostConnection::sendVoiceBlock (juce::int64 startSample, const float* mono, int numSamples)
{
    if (! connected.load() || numSamples <= 0)
        return false;

    // The server fills in our name when relaying.
    const auto block = wire::encodeVoiceBlock ({}, startSample, mono, numSamples);
    const juce::ScopedLock sl (writeLock);
    return wire::send (socket, MsgType::voiceBlock, block.getData(), (juce::uint32) block.getSize(), &cryptoSession);
}

bool HostConnection::sendRecordingPrefs (bool autoReceive)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("autoReceive", autoReceive);
    return sendJson (MsgType::recordingPrefs, juce::var (obj));
}

bool HostConnection::sendRecordingAnswer (const juce::String& recId, bool accept)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("recId", recId);
    obj->setProperty ("accept", accept);
    return sendJson (MsgType::recordingAnswer, juce::var (obj));
}

bool HostConnection::sendRawBytes (MsgType type, const void* data, juce::uint32 bytes)
{
    if (! connected.load())
        return false;

    const juce::ScopedLock sl (writeLock);
    return wire::send (socket, type, data, bytes, &cryptoSession);
}

bool HostConnection::offerSong (const juce::String& name, const juce::Array<juce::File>& files)
{
    if (! connected.load() || name.trim().isEmpty() || files.isEmpty())
        return false;
    if (songUploader != nullptr && songUploader->isThreadRunning())
        return false;   // an upload is still finishing

    juce::int64 totalBytes = 0;
    for (const auto& f : files)
    {
        if (! f.existsAsFile())
            return false;
        totalBytes += f.getSize();
    }

    const auto offerId = juce::Uuid().toString();
    {
        const juce::ScopedLock sl (uploadLock);
        if (upOfferId.isNotEmpty())
            return false;   // one offer at a time
        upOfferId    = offerId;
        upSongName   = name.trim();
        upFiles      = files;
        upTotalBytes = totalBytes;
    }

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("offerId", offerId);
    obj->setProperty ("name", name.trim());
    obj->setProperty ("numFiles", files.size());
    obj->setProperty ("totalBytes", totalBytes);

    if (! sendJson (MsgType::songOffer, juce::var (obj)))
    {
        const juce::ScopedLock sl (uploadLock);
        upOfferId.clear();
        upSongName.clear();
        upFiles.clear();
        upTotalBytes = 0;
        return false;
    }
    return true;
}

void HostConnection::runSongUpload (juce::Thread& runner)
{
    juce::String offerId;
    juce::Array<juce::File> files;
    juce::int64 totalBytes = 0;
    {
        const juce::ScopedLock sl (uploadLock);
        offerId    = upOfferId;
        files      = upFiles;
        totalBytes = upTotalBytes;
    }
    if (offerId.isEmpty() || files.isEmpty())
        return;

    auto clearJob = [this]
    {
        const juce::ScopedLock sl (uploadLock);
        upOfferId.clear();
        upSongName.clear();
        upFiles.clear();
        upTotalBytes = 0;
    };
    auto finish = [&] (bool ok, const juce::String& error)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("offerId", offerId);
        obj->setProperty ("ok", ok);
        if (error.isNotEmpty()) obj->setProperty ("error", error);
        sendJson (MsgType::songEnd, juce::var (obj));

        clearJob();
        onMessageThread ([cb = onSongUploadEnd, ok, error] { if (cb) cb (ok, error); });
    };

    constexpr int kChunkBytes = 256 * 1024;
    juce::HeapBlock<char> chunk (kChunkBytes);
    juce::int64  sentBytes = 0;
    juce::uint32 lastProgressMs = 0;

    for (int index = 0; index < files.size(); ++index)
    {
        if (runner.threadShouldExit() || ! connected.load())
        {
            clearJob();
            onMessageThread ([cb = onSongUploadEnd] { if (cb) cb (false, "Connection lost."); });
            return;
        }

        const auto& file = files.getReference (index);
        juce::FileInputStream in (file);
        if (! in.openedOk())
        {
            finish (false, "Could not open " + file.getFileName());
            return;
        }

        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("offerId", offerId);
            obj->setProperty ("fileName", file.getFileName());
            obj->setProperty ("fileSize", in.getTotalLength());
            obj->setProperty ("fileIndex", index);
            obj->setProperty ("numFiles", files.size());
            if (! sendJson (MsgType::songFileBegin, juce::var (obj)))
                { clearJob(); onMessageThread ([cb = onSongUploadEnd] { if (cb) cb (false, "Send failed."); }); return; }
        }

        while (! in.isExhausted() && ! runner.threadShouldExit())
        {
            const int got = in.read (chunk.getData(), kChunkBytes);
            if (got <= 0)
                break;
            if (! sendRawBytes (MsgType::songChunk, chunk.getData(), (juce::uint32) got))
                { clearJob(); onMessageThread ([cb = onSongUploadEnd] { if (cb) cb (false, "Send failed."); }); return; }

            sentBytes += got;
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastProgressMs >= 200)
            {
                lastProgressMs = now;
                onMessageThread ([cb = onSongUploadProgress, sentBytes, totalBytes]
                                 { if (cb) cb (sentBytes, totalBytes); });
            }
        }

        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("offerId", offerId);
            obj->setProperty ("fileName", file.getFileName());
            obj->setProperty ("ok", true);
            if (! sendJson (MsgType::songFileEnd, juce::var (obj)))
                { clearJob(); onMessageThread ([cb = onSongUploadEnd] { if (cb) cb (false, "Send failed."); }); return; }
        }
    }

    finish (true, {});
}

bool HostConnection::requestStem (const juce::String& songId, const juce::String& stemId,
                                  const juce::File& destFile)
{
    if (! connected.load())
        return false;

    {
        const juce::ScopedLock sl (transferLock);
        if (transferStream != nullptr || pendingSongId.isNotEmpty())
            return false; // one at a time

        pendingSongId = songId;
        pendingStemId = stemId;
        pendingDest   = destFile;
    }

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("songId", songId);
    obj->setProperty ("stemId", stemId);
    return sendJson (MsgType::stemRequest, juce::var (obj));
}

} // namespace bandjam
