#include "HostMixEngine.h"
#include "common/AppPaths.h"
#include "common/Settings.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <thread>

namespace bandjam
{
//==============================================================================
// PerformerStream
//==============================================================================
void HostMixEngine::PerformerStream::write (juce::int64 startSample, const juce::int16* samples,
                                            int numSamples, juce::int64 readPos)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    const juce::int64 end = startSample + numSamples;

    // Too late entirely (already played past it) -> drop what's behind.
    juce::int64 from = juce::jmax (startSample, readPos);
    if (end <= from)
        return;

    // Absurdly far ahead of playback (would wrap over unread cells) -> drop.
    if (end - readPos > (juce::int64) capacity - 4096)
        return;

    constexpr float scale = 1.0f / 32768.0f;
    float peak = 0.0f;

    {
        const juce::ScopedLock sl (cellLock);
        for (juce::int64 i = from; i < end; ++i)
        {
            const float v = (float) samples[i - startSample] * scale;
            ring[(size_t) (i & mask)] = v;
            peak = juce::jmax (peak, std::abs (v));
        }

        juce::int64 expected = written.load();
        while (expected < end && ! written.compare_exchange_weak (expected, end)) {}
    }

    level.store (peak);
}

void HostMixEngine::PerformerStream::consume (juce::int64 position, float* dest, int numSamples)
{
    const juce::ScopedLock sl (cellLock);
    for (int i = 0; i < numSamples; ++i)
    {
        const size_t idx = (size_t) ((position + i) & mask);
        dest[i] = ring[idx];
        ring[idx] = 0.0f;   // consumed cells return to silence for the next lap
    }
}

//==============================================================================
HostMixEngine::~HostMixEngine()
{
    stopAndClose();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void HostMixEngine::initialiseDevice()
{
    std::unique_ptr<juce::XmlElement> savedState;
    if (settings::hostDeviceStateFile().existsAsFile())
        savedState = juce::XmlDocument::parse (settings::hostDeviceStateFile());

    auto err = deviceManager.initialise (2, 2, savedState.get(), true);
    if (err.isNotEmpty() && savedState != nullptr)
        deviceManager.initialise (2, 2, nullptr, true); // saved device gone - fall back

    deviceManager.addChangeListener (this);

    if (! deviceInitialised)
    {
        deviceManager.addAudioCallback (this);
        deviceInitialised = true;
    }
}

void HostMixEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        settings::hostDeviceStateFile().replaceWithText (xml->toString());
}

//==============================================================================
bool HostMixEngine::adoptPreview (const juce::String& previewNameToUse,
                                  std::vector<songloader::DecodedStem>&& decodedStems,
                                  juce::String& error)
{
    if (state.load() != State::idle)
    {
        error = "A jam is active - stop it before previewing a song.";
        return false;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        initialiseDevice();
    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        error = "No audio device is open - check the audio settings.";
        return false;
    }

    unloadPreview();   // also detaches/reattaches the callback safely

    deviceManager.removeAudioCallback (this);   // blocks until the callback returns

    previewName = previewNameToUse;
    previewRate = deviceRate;
    juce::int64 maxLen = 0;
    for (auto& d : decodedStems)
    {
        auto* stem = new RamStem();
        stem->name   = d.name;
        stem->buffer = std::move (d.buffer);
        stem->gain.store (juce::Decibels::decibelsToGain (d.gainDb));
        stem->mute.store (d.mute);
        maxLen = juce::jmax (maxLen, (juce::int64) stem->buffer.getNumSamples());
        previewStems.add (stem);
    }
    previewLength = maxLen;
    previewPos.store (0);
    previewLoaded.store (true);

    if (deviceInitialised)
        deviceManager.addAudioCallback (this);
    return true;
}

void HostMixEngine::unloadPreview()
{
    previewPlaying.store (false);
    if (! previewLoaded.exchange (false) && previewStems.isEmpty())
        return;

    deviceManager.removeAudioCallback (this);
    previewStems.clear();
    previewName.clear();
    previewLength = 0;
    previewPos.store (0);

    if (deviceInitialised)
        deviceManager.addAudioCallback (this);
}

juce::String HostMixEngine::getPreviewStemName (int index) const
{
    if (juce::isPositiveAndBelow (index, previewStems.size()))
        return previewStems.getUnchecked (index)->name;
    return {};
}

void HostMixEngine::setPreviewStemGainDb (int index, float gainDb)
{
    if (juce::isPositiveAndBelow (index, previewStems.size()))
        previewStems.getUnchecked (index)->gain.store (juce::Decibels::decibelsToGain (gainDb));
}

void HostMixEngine::setPreviewStemMute (int index, bool mute)
{
    if (juce::isPositiveAndBelow (index, previewStems.size()))
        previewStems.getUnchecked (index)->mute.store (mute);
}

float HostMixEngine::getPreviewStemLevel (int index) const
{
    if (juce::isPositiveAndBelow (index, previewStems.size()))
        return previewStems.getUnchecked (index)->level.load();
    return 0.0f;
}

void HostMixEngine::previewSeekSeconds (double seconds)
{
    previewPos.store (juce::jlimit ((juce::int64) 0, previewLength,
                                    (juce::int64) std::llround (seconds * previewRate)));
}

double HostMixEngine::getPreviewPositionSeconds() const
{
    return previewRate > 0.0 ? (double) previewPos.load() / previewRate : 0.0;
}

double HostMixEngine::getPreviewLengthSeconds() const
{
    return previewRate > 0.0 ? (double) previewLength / previewRate : 0.0;
}

//==============================================================================
bool HostMixEngine::prepareMonitor (const LibrarySong& song, std::vector<songloader::DecodedStem>&& decodedStems,
                                    const juce::StringArray& performerNames, double leadSecondsToUse,
                                    juce::String& error)
{
    unloadPreview();   // the jam takes over the device
    stopAndClose();

    if (decodedStems.empty())
    {
        error = "The song has no stems.";
        return false;
    }

    songName    = song.name;
    songRate    = song.sampleRate > 0.0 ? song.sampleRate : 44100.0;
    leadSeconds = juce::jlimit (0.5, 15.0, leadSecondsToUse);
    leadSamples = (juce::int64) std::llround (leadSeconds * songRate);

    // -- adopt the pre-decoded backing track -------------------------------------
    juce::int64 maxLen = 0;
    for (auto& d : decodedStems)
    {
        auto* stem = new RamStem();
        stem->name   = d.name;
        stem->buffer = std::move (d.buffer);
        maxLen = juce::jmax (maxLen, (juce::int64) stem->buffer.getNumSamples());
        stems.add (stem);
    }
    songLength = maxLen;

    // -- one positioning buffer per musician (~30 s @ song rate) ----------------
    {
        const juce::ScopedLock sl (streamsLock);
        const int capacity = juce::nextPowerOfTwo ((int) (songRate * 30.0));
        for (const auto& name : performerNames)
            streams.add (new PerformerStream (name, capacity));
    }

    // -- audio device: the host's own selection, kept open persistently ----------
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        initialiseDevice();

    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        error = "No audio device is open - check the 'Host audio' settings.";
        stems.clear();
        const juce::ScopedLock sl (streamsLock);
        streams.clear();
        return false;
    }

    // Prefer running the device at the song's rate (no resampling). The
    // user's device choice is kept either way - if the device refuses the
    // rate, the output resamplers bridge the difference.
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        if (! juce::approximatelyEqual (setup.sampleRate, songRate))
        {
            setup.sampleRate = songRate;
            deviceManager.setAudioDeviceSetup (setup, false);
        }
    }

    deviceRate = songRate;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        deviceRate = device->getCurrentSampleRate();

    outResamplerL.prepare (songRate, deviceRate, 8192);
    outResamplerR.prepare (songRate, deviceRate, 8192);
    hostRecResamplerL.prepare (deviceRate, songRate, 8192);
    hostRecResamplerR.prepare (deviceRate, songRate, 8192);
    chunkL.allocate (kChunk, true);
    chunkR.allocate (kChunk, true);
    streamScratch.allocate (kChunk, true);
    hostRecL.allocate (kChunk, true);
    hostRecR.allocate (kChunk, true);
    recMixL.allocate (kChunk, true);
    recMixR.allocate (kChunk, true);

    playPos.store (0);
    outputLevel.store (0.0f);
    recordingDir = juce::File();

    state.store (State::monitor);   // the persistent callback picks this up
    return true;
}

void HostMixEngine::startJamPlayback (bool recordToFile)
{
    if (state.load() != State::monitor)
        return;

    // Create the recorders before the state changes, so they exist (and are
    // visible) before the audio thread can ever reach the playing state.
    if (recordToFile)
    {
        recordingDir = paths::recordingsRoot().getChildFile (
            paths::safeFileName (songName) + "_"
            + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"));
        recordingDir.createDirectory();

        if (recordingDir.isDirectory())
        {
            recordThread.startThread();

            auto makeWriter = [this] (const juce::File& file, int channels)
                -> std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>
            {
                juce::WavAudioFormat wavFormat;
                auto stream = std::make_unique<juce::FileOutputStream> (file);
                if (! stream->openedOk())
                    return nullptr;
                if (auto* writer = wavFormat.createWriterFor (stream.get(), songRate, (unsigned int) channels, 24, {}, 0))
                {
                    stream.release(); // now owned by the writer
                    return std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer, recordThread, 1 << 17);
                }
                return nullptr;
            };

            // One mono take per musician: the raw signal, before gain/mute,
            // so the balance can be redone in the Recordings tab.
            {
                const juce::ScopedLock sl (streamsLock);
                for (auto* stream : streams)
                {
                    auto track = std::make_unique<RecordTrack>();
                    track->streamName  = stream->name;
                    track->displayName = stream->name;
                    track->fileName    = "take_" + paths::safeFileName (stream->name) + ".wav";
                    track->writer      = makeWriter (recordingDir.getChildFile (track->fileName), 1);
                    if (track->writer != nullptr)
                        recordTracks.add (track.release());
                }
            }

            // The host's own input (stereo, also raw).
            {
                auto track = std::make_unique<RecordTrack>();
                track->displayName = "My Instrument";
                track->fileName    = "take_My Instrument.wav";
                track->writer      = makeWriter (recordingDir.getChildFile (track->fileName), 2);
                if (track->writer != nullptr)
                {
                    hostRecordTrack = track.get();
                    recordTracks.add (track.release());
                }
            }
        }

        recordingActive.store (! recordTracks.isEmpty());
        if (! recordingActive.load())
            recordingDir = juce::File();
    }

    // Switch to gate FIRST (the audio thread stops touching monitor state),
    // then reset all musician streams to jam time: index 0 == song sample 0.
    {
        const juce::ScopedLock sl (streamsLock);  // block reader threads during the reset
        state.store (State::gate);
        for (auto* stream : streams)
        {
            const juce::ScopedLock cl (stream->cellLock);
            juce::FloatVectorOperations::clear (stream->ring.getData(), stream->capacity);
            stream->written.store (0);
            stream->monitorReadPos.store (0);
            stream->monitorConsuming = false;
            stream->level.store (0.0f);
        }
    }

    playPos.store (0);
    armTimeMs = juce::Time::getMillisecondCounter();
}

void HostMixEngine::stopAndClose()
{
    // Suspend the persistent callback while jam state is torn down; the
    // device itself stays open (host input / stream keep working).
    deviceManager.removeAudioCallback (this);   // blocks until the callback returns

    const auto recordedLength = playPos.load();

    state.store (State::idle);
    playPos.store (0);
    outputLevel.store (0.0f);

    const bool hadRecording = recordingActive.load() && recordingDir.isDirectory();
    recordingActive.store (false);

    // Collect the take metadata before the writers are destroyed.
    juce::Array<juce::var> trackMeta;
    for (auto* track : recordTracks)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("file", track->fileName);
        obj->setProperty ("name", track->displayName);
        obj->setProperty ("kind", track->streamName.isEmpty() ? "host" : "musician");
        trackMeta.add (juce::var (obj));
    }
    hostRecordTrack = nullptr;
    recordTracks.clear();                       // flushes + finalises the take WAVs
    recordThread.stopThread (4000);

    // The backing stems sit unchanged in RAM, so instead of recording them
    // live they are written out now, trimmed to the recorded length. Move
    // the buffers into a worker so a long song never blocks the UI.
    std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> stemData;
    if (hadRecording)
        for (auto* stem : stems)
            stemData.emplace_back (stem->name, std::move (stem->buffer));

    {
        // Reader threads may be inside push*Audio() right now.
        const juce::ScopedLock sl (streamsLock);
        streams.clear();
    }
    stems.clear();

    if (deviceInitialised)
        deviceManager.addAudioCallback (this);

    if (hadRecording)
    {
        auto job = [dir = recordingDir, rate = songRate, len = recordedLength,
                    song = songName, tracks = std::move (trackMeta),
                    data = std::move (stemData), cb = onRecordingSaved]() mutable
        {
            juce::WavAudioFormat wav;
            int insertAt = 0;
            int index = 0;
            for (auto& [name, buffer] : data)
            {
                const auto fileName = "stem_" + juce::String (++index).paddedLeft ('0', 2)
                                        + "_" + paths::safeFileName (name) + ".wav";
                auto file = dir.getChildFile (fileName);
                file.deleteFile();

                auto stream = std::make_unique<juce::FileOutputStream> (file);
                if (! stream->openedOk())
                    continue;

                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                        wav.createWriterFor (stream.get(), rate,
                                             (unsigned int) juce::jmax (1, buffer.getNumChannels()), 24, {}, 0)))
                {
                    stream.release();
                    const int n = (int) juce::jmin ((juce::int64) buffer.getNumSamples(), len);
                    if (n > 0)
                        writer->writeFromAudioSampleBuffer (buffer, 0, n);

                    auto* obj = new juce::DynamicObject();
                    obj->setProperty ("file", fileName);
                    obj->setProperty ("name", name);
                    obj->setProperty ("kind", "stem");
                    tracks.insert (insertAt++, juce::var (obj));   // stems first, in song order
                }
            }

            auto* meta = new juce::DynamicObject();
            meta->setProperty ("song", song);
            meta->setProperty ("date", juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M"));
            meta->setProperty ("sampleRate", rate);
            meta->setProperty ("lengthSamples", (juce::int64) len);
            meta->setProperty ("tracks", tracks);
            dir.getChildFile ("meta.json").replaceWithText (juce::JSON::toString (juce::var (meta)));

            if (cb != nullptr)
                juce::MessageManager::callAsync ([cb, dir] { cb (dir); });
        };
        std::thread (std::move (job)).detach();
    }
}

//==============================================================================
void HostMixEngine::pushJamAudio (const juce::String& performerName, juce::int64 startSample,
                                  const juce::int16* samples, int numSamples)
{
    const juce::ScopedLock sl (streamsLock);
    const auto st = state.load();
    if (st != State::gate && st != State::playing)
        return;

    if (auto* stream = findStream (performerName))
        stream->write (startSample, samples, numSamples, playPos.load());
}

void HostMixEngine::pushMonitorAudio (const juce::String& performerName, juce::int64 startSample,
                                      const juce::int16* samples, int numSamples)
{
    const juce::ScopedLock sl (streamsLock);
    if (state.load() != State::monitor)
        return;

    if (auto* stream = findStream (performerName))
        stream->write (startSample, samples, numSamples, stream->monitorReadPos.load());
}

HostMixEngine::PerformerStream* HostMixEngine::findStream (const juce::String& performerName) const
{
    for (auto* s : streams)
        if (s->name.equalsIgnoreCase (performerName))
            return s;
    return nullptr;
}

HostMixEngine::RecordTrack* HostMixEngine::findRecordTrack (const juce::String& streamName) const
{
    for (auto* t : recordTracks)
        if (t->streamName == streamName)
            return t;
    return nullptr;
}

//==============================================================================
void HostMixEngine::renderChunk (State st, float* left, float* right, int numSamples)
{
    if (st == State::playing)
        renderSongChunk (left, right, numSamples);
    else
        renderMonitorChunk (left, right, numSamples);
}

void HostMixEngine::renderMonitorChunk (float* left, float* right, int numSamples)
{
    juce::FloatVectorOperations::clear (left,  numSamples);
    juce::FloatVectorOperations::clear (right, numSamples);

    const auto startThreshold = (juce::int64) (songRate * 0.15);  // ~150 ms jitter buffer

    for (auto* stream : streams)
    {
        const auto readPos = stream->monitorReadPos.load();
        const auto avail   = stream->written.load() - readPos;

        if (! stream->monitorConsuming)
        {
            if (avail < startThreshold)
                continue;
            stream->monitorConsuming = true;
        }

        stream->consume (readPos, streamScratch.getData(), numSamples);
        stream->monitorReadPos.store (readPos + numSamples);

        if (stream->written.load() - (readPos + numSamples) <= 0)
            stream->monitorConsuming = false;   // ran dry - rebuffer before continuing

        if (stream->mute.load())
            continue;

        const float gain = stream->gain.load();
        juce::FloatVectorOperations::addWithMultiply (left,  streamScratch.getData(), gain, numSamples);
        juce::FloatVectorOperations::addWithMultiply (right, streamScratch.getData(), gain, numSamples);
    }

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax (peak, std::abs (left[i]));
    outputLevel.store (peak);
}

void HostMixEngine::renderSongChunk (float* left, float* right, int numSamples)
{
    juce::FloatVectorOperations::clear (left,  numSamples);
    juce::FloatVectorOperations::clear (right, numSamples);

    const auto pos = playPos.load();

    // Backing track ------------------------------------------------------------
    for (auto* stem : stems)
    {
        if (stem->mute.load())
            continue;

        const auto& buf = stem->buffer;
        const auto  len = (juce::int64) buf.getNumSamples();
        if (pos >= len)
            continue;

        const int n = (int) juce::jmin ((juce::int64) numSamples, len - pos);
        const float gain = stem->gain.load();

        const float* srcL = buf.getReadPointer (0, (int) pos);
        const float* srcR = buf.getNumChannels() > 1 ? buf.getReadPointer (1, (int) pos) : srcL;

        juce::FloatVectorOperations::addWithMultiply (left,  srcL, gain, n);
        juce::FloatVectorOperations::addWithMultiply (right, srcR, gain, n);
    }

    // Musicians ------------------------------------------------------------------
    const bool recording = recordingActive.load();
    for (auto* stream : streams)
    {
        // Always consume (cells must be cleared), even while muted.
        stream->consume (pos, streamScratch.getData(), numSamples);

        // Record the raw (pre-gain/mute) take.
        if (recording)
            if (auto* track = findRecordTrack (stream->name))
            {
                const float* chans[1] = { streamScratch.getData() };
                track->writer->write (chans, numSamples);
            }

        if (stream->mute.load())
            continue;

        const float gain = stream->gain.load();
        juce::FloatVectorOperations::addWithMultiply (left,  streamScratch.getData(), gain, numSamples);
        juce::FloatVectorOperations::addWithMultiply (right, streamScratch.getData(), gain, numSamples);
    }

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax (peak, std::abs (left[i]));
    outputLevel.store (peak);

    if (recording && hostRecordTrack != nullptr)
    {
        // Host take: raw (pre-gain/mute) input, bridged from device rate
        // (pushed into hostRecResampler* by the device callback).
        const int gotL = hostRecResamplerL.pullOutput (hostRecL.getData(), numSamples);
        const int gotR = hostRecResamplerR.pullOutput (hostRecR.getData(), numSamples);
        const int got  = juce::jmin (gotL, gotR);

        // Pad the resampler's start-up latency with silence so the take
        // stays sample-aligned with the other tracks.
        if (got < numSamples)
        {
            const int pad = numSamples - got;
            juce::FloatVectorOperations::clear (recMixL.getData(), pad);
            juce::FloatVectorOperations::clear (recMixR.getData(), pad);
            const float* padChans[2] = { recMixL.getData(), recMixR.getData() };
            hostRecordTrack->writer->write (padChans, pad);
        }
        if (got > 0)
        {
            const float* chans[2] = { hostRecL.getData(), hostRecR.getData() };
            hostRecordTrack->writer->write (chans, got);
        }
    }

    playPos.store (pos + numSamples);
}

void HostMixEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                      float* const* outputChannelData, int numOutputChannels,
                                                      int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    float* outL = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
    float* outR = numOutputChannels > 1 && outputChannelData[1] != nullptr ? outputChannelData[1] : nullptr;
    if (outL == nullptr)
        return;

    const float* inL = numInputChannels > 0 && inputChannelData[0] != nullptr ? inputChannelData[0] : nullptr;
    const float* inR = numInputChannels > 1 && inputChannelData[1] != nullptr ? inputChannelData[1] : inL;

    auto st = state.load();

    // The gate: start only once every musician has the full lead buffered
    // (or after a safety timeout, so a silent/crashed musician can't stall
    // the jam forever - their part simply stays silent until data arrives).
    if (st == State::gate)
    {
        bool allBuffered = ! streams.isEmpty();
        for (auto* stream : streams)
            if (stream->written.load() < leadSamples)
                { allBuffered = false; break; }

        const bool timedOut = juce::Time::getMillisecondCounter() - armTimeMs
                                > (juce::uint32) ((leadSeconds + 8.0) * 1000.0);

        if (allBuffered || timedOut)
        {
            state.store (State::playing);
            st = State::playing;
        }
        // else: mix stays silent while the buffers fill; the host's own
        // input below keeps passing through.
    }

    if (st == State::monitor || st == State::playing)
    {
        // Feed the host input into the recording bridge before rendering, so
        // renderSongChunk() can mix this block's input into the WAV.
        if (st == State::playing && recordingActive.load() && inL != nullptr)
        {
            hostRecResamplerL.pushInput (inL, numSamples);
            hostRecResamplerR.pushInput (inR, numSamples);
        }

        if (outResamplerL.isPassThrough())
        {
            int done = 0;
            while (done < numSamples)
            {
                const int n = juce::jmin (kChunk, numSamples - done);
                renderChunk (st, chunkL.getData(), chunkR.getData(), n);
                juce::FloatVectorOperations::copy (outL + done, chunkL.getData(), n);
                if (outR != nullptr)
                    juce::FloatVectorOperations::copy (outR + done, chunkR.getData(), n);
                done += n;
            }
        }
        else
        {
            // Generate song-rate audio until the resamplers can emit numSamples.
            while (outResamplerL.availableOutput() < numSamples)
            {
                renderChunk (st, chunkL.getData(), chunkR.getData(), kChunk);
                outResamplerL.pushInput (chunkL.getData(), kChunk);
                outResamplerR.pushInput (chunkR.getData(), kChunk);
            }

            outResamplerL.pullOutput (outL, numSamples);
            if (outR != nullptr)
                outResamplerR.pullOutput (outR, numSamples);
            else
            {
                // keep both resamplers in lock-step even with a mono output
                juce::HeapBlock<float> discard ((size_t) numSamples);
                outResamplerR.pullOutput (discard.getData(), numSamples);
            }
        }
    }

    // Preview player: local listening while no jam is active.
    if (st == State::idle && previewLoaded.load() && previewPlaying.load())
    {
        const auto pos = previewPos.load();
        float peak = 0.0f;

        for (auto* stem : previewStems)
        {
            const auto& buf = stem->buffer;
            const auto  len = (juce::int64) buf.getNumSamples();

            if (stem->mute.load() || pos >= len)
            {
                stem->level.store (0.0f);
                continue;
            }

            const int n = (int) juce::jmin ((juce::int64) numSamples, len - pos);
            const float gain = stem->gain.load();

            const float* srcL = buf.getReadPointer (0, (int) pos);
            const float* srcR = buf.getNumChannels() > 1 ? buf.getReadPointer (1, (int) pos) : srcL;

            juce::FloatVectorOperations::addWithMultiply (outL, srcL, gain, n);
            if (outR != nullptr)
                juce::FloatVectorOperations::addWithMultiply (outR, srcR, gain, n);

            const auto range = juce::FloatVectorOperations::findMinAndMax (srcL, n);
            stem->level.store (juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd())) * gain);
        }

        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (outL[i]));
        outputLevel.store (peak);

        const auto next = pos + numSamples;
        previewPos.store (next);
        if (next >= previewLength)
        {
            previewPlaying.store (false);   // reached the end
            previewPos.store (0);
        }
    }

    // VRChat/virtual-mic feed: the band mix and the host's own input are
    // included independently ("Audio Stream" tab), and the local monitor
    // mute ("My input" strip) no longer decides what VRChat hears.
    if (streamFeedCapacity >= numSamples && streamOut.isRunning())
    {
        const bool includeMix = streamIncludeMix.load();
        if (includeMix)
        {
            juce::FloatVectorOperations::copy (streamFeedL.getData(), outL, numSamples);
            juce::FloatVectorOperations::copy (streamFeedR.getData(), outR != nullptr ? outR : outL, numSamples);
        }
        else
        {
            juce::FloatVectorOperations::clear (streamFeedL.getData(), numSamples);
            juce::FloatVectorOperations::clear (streamFeedR.getData(), numSamples);
        }

        if (streamIncludeInput.load() && inL != nullptr)
        {
            const float g = hostInGain.load();
            juce::FloatVectorOperations::addWithMultiply (streamFeedL.getData(), inL, g, numSamples);
            juce::FloatVectorOperations::addWithMultiply (streamFeedR.getData(), inR, g, numSamples);
        }

        streamOut.push (streamFeedL.getData(), streamFeedR.getData(), numSamples);
    }

    // Host's own live input (talk mic / instrument): passed through to the
    // local output - even while no jam is running. The host plays along to
    // the (already delayed) mix they hear, so a plain live add is in time
    // with that mix.
    if (inL != nullptr)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (inL[i]), std::abs (inR[i]));
        hostInLevel.store (peak);   // pre-gain, so the meter works while muted

        if (! hostInMute.load())
        {
            const float g = hostInGain.load();
            juce::FloatVectorOperations::addWithMultiply (outL, inL, g, numSamples);
            if (outR != nullptr)
                juce::FloatVectorOperations::addWithMultiply (outR, inR, g, numSamples);
        }
    }
    else
    {
        hostInLevel.store (0.0f);
    }
}

void HostMixEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        deviceRate = device->getCurrentSampleRate();
        streamOut.setSourceRate (deviceRate);

        const int block = juce::jmax (2048, device->getCurrentBufferSizeSamples());
        streamFeedL.allocate ((size_t) block, true);
        streamFeedR.allocate ((size_t) block, true);
        streamFeedCapacity = block;

        // Device (rate) changed while a jam is prepared/running: re-bridge
        // between song rate and the new device rate. Safe here - callbacks
        // have not started yet on the new device.
        if (state.load() != State::idle)
        {
            outResamplerL.prepare (songRate, deviceRate, 8192);
            outResamplerR.prepare (songRate, deviceRate, 8192);
            hostRecResamplerL.prepare (deviceRate, songRate, 8192);
            hostRecResamplerR.prepare (deviceRate, songRate, 8192);
        }
    }
}

void HostMixEngine::audioDeviceStopped() {}

//==============================================================================
double HostMixEngine::getPositionSeconds() const
{
    return songRate > 0.0 ? (double) playPos.load() / songRate : 0.0;
}

double HostMixEngine::getSongLengthSeconds() const
{
    return songRate > 0.0 ? (double) songLength / songRate : 0.0;
}

juce::StringArray HostMixEngine::getPerformerNames() const
{
    juce::StringArray names;
    for (auto* s : streams)
        names.add (s->name);
    return names;
}

double HostMixEngine::getBufferedSeconds (const juce::String& performerName) const
{
    if (auto* stream = findStream (performerName))
    {
        const auto base  = state.load() == State::monitor ? stream->monitorReadPos.load() : playPos.load();
        const auto ahead = stream->written.load() - base;
        return songRate > 0.0 ? juce::jmax (0.0, (double) ahead / songRate) : 0.0;
    }
    return 0.0;
}

juce::StringArray HostMixEngine::getWaitingFor() const
{
    juce::StringArray waiting;
    if (state.load() == State::gate)
        for (auto* s : streams)
            if (s->written.load() < leadSamples)
                waiting.add (s->name);
    return waiting;
}

float HostMixEngine::getPerformerLevel (const juce::String& performerName) const
{
    if (auto* stream = findStream (performerName))
        return stream->level.load();
    return 0.0f;
}

void HostMixEngine::setPerformerGainDb (const juce::String& performerName, float gainDb)
{
    if (auto* stream = findStream (performerName))
        stream->gain.store (juce::Decibels::decibelsToGain (gainDb));
}

void HostMixEngine::setPerformerMute (const juce::String& performerName, bool mute)
{
    if (auto* stream = findStream (performerName))
        stream->mute.store (mute);
}

juce::String HostMixEngine::getStemName (int index) const
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        return stems.getUnchecked (index)->name;
    return {};
}

void HostMixEngine::setStemGainDb (int index, float gainDb)
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        stems.getUnchecked (index)->gain.store (juce::Decibels::decibelsToGain (gainDb));
}

void HostMixEngine::setStemMute (int index, bool mute)
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        stems.getUnchecked (index)->mute.store (mute);
}

} // namespace bandjam
