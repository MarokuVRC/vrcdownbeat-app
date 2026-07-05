#include "PlayCaptureEngine.h"
#include "common/Settings.h"
#include <cmath>

namespace bandjam
{
PlayCaptureEngine::PlayCaptureEngine()
{
    deviceManager.addChangeListener (this);
}

PlayCaptureEngine::~PlayCaptureEngine()
{
    stopJam();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    instrument.unloadPlugin();
}

namespace
{
    /** Blocks until any in-flight audio callback returns, keeps it detached
        while alive, then re-attaches it. Lets us mutate engine data that the
        audio thread reads. */
    struct ScopedCallbackSuspender
    {
        ScopedCallbackSuspender (juce::AudioDeviceManager& m, juce::AudioIODeviceCallback* c)
            : manager (m), callback (c)
        {
            manager.removeAudioCallback (callback);
        }
        ~ScopedCallbackSuspender() { manager.addAudioCallback (callback); }

        juce::AudioDeviceManager& manager;
        juce::AudioIODeviceCallback* callback;
    };
}

void PlayCaptureEngine::initialiseDevice()
{
    std::unique_ptr<juce::XmlElement> savedState;
    if (settings::deviceStateFile().existsAsFile())
        savedState = juce::XmlDocument::parse (settings::deviceStateFile());

    auto err = deviceManager.initialise (1, 2, savedState.get(), true);
    if (err.isNotEmpty() && savedState != nullptr)
        deviceManager.initialise (1, 2, nullptr, true); // saved device gone - fall back

    deviceManager.addAudioCallback (this);
}

void PlayCaptureEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        settings::deviceStateFile().replaceWithText (xml->toString());

    if (auto* device = deviceManager.getCurrentAudioDevice())
        deviceRate = device->getCurrentSampleRate();

    if (onDeviceChanged)
        onDeviceChanged();
}

double PlayCaptureEngine::getDeviceSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();
    return 0.0;
}

//==============================================================================
bool PlayCaptureEngine::adoptSong (const LocalSong& meta, std::vector<songloader::DecodedStem>&& decoded,
                                   juce::String& error)
{
    unload();

    if (decoded.empty())
    {
        error = "No stems loaded.";
        return false;
    }
    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        error = "No audio device active.";
        return false;
    }

    juce::OwnedArray<RamStem> newStems;
    juce::int64 maxLen = 0;

    for (auto& d : decoded)
    {
        auto* stem = new RamStem();
        stem->name = d.name;
        stem->gain.store (juce::Decibels::decibelsToGain (d.gainDb));
        stem->mute.store (d.mute);
        stem->buffer = std::move (d.buffer);
        maxLen = juce::jmax (maxLen, (juce::int64) stem->buffer.getNumSamples());
        newStems.add (stem);
    }

    {
        ScopedCallbackSuspender suspend (deviceManager, this);
        stems.swapWith (newStems);
        songRate            = meta.sampleRate > 0.0 ? meta.sampleRate : 44100.0;
        lengthDeviceSamples = maxLen;
        loadedSongId        = meta.id;
        previewPos.store (0);
        loaded.store (true);
    }
    return true;
}

void PlayCaptureEngine::unload()
{
    stopJam();
    stopMonitor();
    previewStop();

    ScopedCallbackSuspender suspend (deviceManager, this);
    loaded.store (false);
    stems.clear();
    loadedSongId.clear();
    lengthDeviceSamples = 0;
}

juce::String PlayCaptureEngine::getStemName (int index) const
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        return stems.getUnchecked (index)->name;
    return {};
}

void PlayCaptureEngine::setStemGainDb (int index, float gainDb)
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        stems.getUnchecked (index)->gain.store (juce::Decibels::decibelsToGain (gainDb));
}

void PlayCaptureEngine::setStemMute (int index, bool mute)
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        stems.getUnchecked (index)->mute.store (mute);
}

float PlayCaptureEngine::getStemLevel (int index) const
{
    if (juce::isPositiveAndBelow (index, stems.size()))
        return stems.getUnchecked (index)->level.load();
    return 0.0f;
}

//==============================================================================
bool PlayCaptureEngine::loadInstrument (const juce::File& vst3File, juce::String& error)
{
    auto* device = deviceManager.getCurrentAudioDevice();
    const double rate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
    const int    size = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

    ScopedCallbackSuspender suspend (deviceManager, this);
    return instrument.loadPlugin (vst3File, rate, juce::jmax (2048, size), error);
}

void PlayCaptureEngine::unloadInstrument()
{
    ScopedCallbackSuspender suspend (deviceManager, this);
    instrument.unloadPlugin();
    micLevel.store (0.0f);
}

//==============================================================================
void PlayCaptureEngine::previewPlay()
{
    if (! loaded.load() || mode.load() == Mode::jam)
        return;

    mode.store (Mode::preview);
    previewPlaying.store (true);
}

void PlayCaptureEngine::previewPause()
{
    previewPlaying.store (false);
}

void PlayCaptureEngine::previewStop()
{
    previewPlaying.store (false);
    previewPos.store (0);
    if (mode.load() == Mode::preview)
        mode.store (Mode::idle);
}

void PlayCaptureEngine::previewSeekSeconds (double seconds)
{
    const auto pos = (juce::int64) (juce::jmax (0.0, seconds) * deviceRate);
    previewPos.store (juce::jlimit ((juce::int64) 0, lengthDeviceSamples, pos));
}

bool PlayCaptureEngine::isPreviewPlaying() const noexcept
{
    return mode.load() == Mode::preview && previewPlaying.load();
}

//==============================================================================
bool PlayCaptureEngine::startMonitor (juce::String& error)
{
    if (mode.load() == Mode::jam)
    {
        error = "A jam is already running.";
        return false;
    }

    stopMonitor();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        error = "No audio device active.";
        return false;
    }

    deviceRate = device->getCurrentSampleRate();

    skipRemaining.store (0);
    leadSilence.store (0);

    captureCapacity = juce::nextPowerOfTwo ((int) (deviceRate * 4.0));
    captureFifo     = std::make_unique<juce::AbstractFifo> (captureCapacity);
    captureBuffer.allocate ((size_t) captureCapacity, true);
    captureResampler.prepare (deviceRate, songRate > 0.0 ? songRate : 44100.0, 16384);
    sentSamples = 0;
    activeSink  = [this] (juce::int64 s, const float* d, int n)
    {
        if (onMonitorBlock) onMonitorBlock (s, d, n);
    };

    sender = std::make_unique<SenderThread> (*this);
    sender->startThread (juce::Thread::Priority::high);

    monitorActive.store (true);
    return true;
}

void PlayCaptureEngine::stopMonitor()
{
    monitorActive.store (false);
    micLevel.store (0.0f);

    if (mode.load() == Mode::jam)
        return; // the pipeline belongs to the jam now

    teardownCapturePipeline();
}

//==============================================================================
bool PlayCaptureEngine::startJam (juce::String& error)
{
    if (! loaded.load())
    {
        error = "No song loaded.";
        return false;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        error = "No audio device active.";
        return false;
    }

    previewStop();
    stopMonitor();
    stopJam();

    deviceRate = device->getCurrentSampleRate();

    // Latency compensation (see class docs). A VST instrument has no input
    // path (the signal is born digitally), so only the output latency counts.
    const int inLatency  = device->getInputLatencyInSamples();
    const int outLatency = device->getOutputLatencyInSamples();
    const int reported   = instrument.isLoaded() ? outLatency : inLatency + outLatency;
    const int manual     = (int) std::llround (getManualOffsetMs() * deviceRate / 1000.0);
    const int total      = reported + manual;
    lastAppliedSkip      = total;

    skipRemaining.store (juce::jmax (0, total));
    leadSilence.store (juce::jmax (0, -total));

    captureCapacity = juce::nextPowerOfTwo ((int) (deviceRate * 4.0));
    captureFifo     = std::make_unique<juce::AbstractFifo> (captureCapacity);
    captureBuffer.allocate ((size_t) captureCapacity, true);
    captureResampler.prepare (deviceRate, songRate, 16384);
    sentSamples = 0;
    activeSink  = [this] (juce::int64 s, const float* d, int n)
    {
        if (onCaptureBlock) onCaptureBlock (s, d, n);
    };

    sender = std::make_unique<SenderThread> (*this);
    sender->startThread (juce::Thread::Priority::high);

    jamPos.store (0);
    mode.store (Mode::jam);
    return true;
}

void PlayCaptureEngine::stopJam()
{
    if (mode.load() == Mode::jam)
        mode.store (Mode::idle);

    monitorActive.store (false);
    teardownCapturePipeline();

    jamPos.store (0);
    micLevel.store (0.0f);
    backingLevel.store (0.0f);
}

void PlayCaptureEngine::teardownCapturePipeline()
{
    if (sender != nullptr)
    {
        sender->signalThreadShouldExit();
        sender->notify();
        sender->stopThread (3000);
        sender.reset();
    }

    ScopedCallbackSuspender suspend (deviceManager, this);
    captureFifo.reset();
    activeSink = nullptr;
}

//==============================================================================
void PlayCaptureEngine::renderStems (juce::int64 position, float* const* out, int numOut, int numSamples)
{
    if (numOut <= 0 || position >= lengthDeviceSamples)
        return;

    float peak = 0.0f;

    for (auto* stem : stems)
    {
        const auto& buf = stem->buffer;
        const auto  len = (juce::int64) buf.getNumSamples();

        if (stem->mute.load() || position >= len)
        {
            stem->level.store (0.0f);
            continue;
        }

        const int n = (int) juce::jmin ((juce::int64) numSamples, len - position);
        const float gain = stem->gain.load();

        const float* left  = buf.getReadPointer (0, (int) position);
        const float* right = buf.getNumChannels() > 1 ? buf.getReadPointer (1, (int) position) : left;

        juce::FloatVectorOperations::addWithMultiply (out[0], left, gain, n);
        if (numOut > 1)
            juce::FloatVectorOperations::addWithMultiply (out[1], right, gain, n);

        const auto range = juce::FloatVectorOperations::findMinAndMax (left, n);
        stem->level.store (juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd())) * gain);
    }

    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax (peak, std::abs (out[0][i]));
    backingLevel.store (peak);
}

void PlayCaptureEngine::captureInput (const float* input, int numSamples)
{
    if (captureFifo == nullptr)
        return;

    // 1) Latency skip: discard the front of the stream (jam only; 0 in monitor).
    int offset = 0;
    {
        const auto toSkip = skipRemaining.load();
        if (toSkip > 0)
        {
            const int now = (int) juce::jmin ((juce::int64) numSamples, toSkip);
            skipRemaining.fetch_sub (now);
            offset = now;
        }
    }

    const int available = numSamples - offset;
    if (available <= 0)
        return;

    const int freeSpace = captureFifo->getFreeSpace();
    const int n = juce::jmin (available, freeSpace);
    if (n <= 0)
        return;

    int s1, n1, s2, n2;
    captureFifo->prepareToWrite (n, s1, n1, s2, n2);

    auto writePart = [&] (int start, int count, int sourceOffset)
    {
        if (count <= 0) return;
        if (input != nullptr)
            juce::FloatVectorOperations::copy (captureBuffer.getData() + start, input + offset + sourceOffset, count);
        else
            juce::FloatVectorOperations::clear (captureBuffer.getData() + start, count);
    };
    writePart (s1, n1, 0);
    writePart (s2, n2, n1);
    captureFifo->finishedWrite (n1 + n2);

    if (sender != nullptr)
        sender->notify();
}

void PlayCaptureEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                          float* const* outputChannelData, int numOutputChannels,
                                                          int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    const auto currentMode = mode.load();
    const bool instActive  = instrument.isLoaded();
    const float* mic = (numInputChannels > 0) ? inputChannelData[0] : nullptr;

    // The VST instrument renders every block so the musician always hears it.
    // Its mono mix replaces the hardware input as the captured signal.
    const float* captureSource = mic;
    if (instActive)
    {
        instrument.process (numSamples);
        instrument.addToOutput (outputChannelData, numOutputChannels, numSamples);

        if (instMonoCapacity >= numSamples)
        {
            instrument.mixMono (instMono.getData(), numSamples);
            captureSource = instMono.getData();
        }
    }

    auto updateLevel = [&]
    {
        if (instActive)
        {
            micLevel.store (instrument.getLevel());
            return;
        }
        float peak = 0.0f;
        if (mic != nullptr)
            for (int i = 0; i < numSamples; ++i)
                peak = juce::jmax (peak, std::abs (mic[i]));
        micLevel.store (peak);
    };

    // VRChat/virtual-mic feed: own signal (mic or instrument) + optionally the
    // local playback. When the instrument is active AND the song is included,
    // the output already contains the instrument - don't add it twice.
    auto feedStream = [&]
    {
        if (! streamOut.isRunning() || instMonoCapacity < numSamples)
            return;

        const bool includeSong  = streamIncludeSong.load();
        const bool includeInput = streamIncludeInput.load();
        const float* outL = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
        const float* outR = numOutputChannels > 1 && outputChannelData[1] != nullptr ? outputChannelData[1] : outL;
        const bool addOwnSignal = includeInput && captureSource != nullptr && ! (instActive && includeSong);

        for (int i = 0; i < numSamples; ++i)
        {
            const float own = addOwnSignal ? captureSource[i] : 0.0f;
            streamL[i] = own + (includeSong && outL != nullptr ? outL[i] : 0.0f);
            streamR[i] = own + (includeSong && outR != nullptr ? outR[i] : 0.0f);
        }
        streamOut.push (streamL.getData(), streamR.getData(), numSamples);
    };

    if (currentMode == Mode::jam)
    {
        const auto pos = jamPos.load();
        if (loaded.load())
            renderStems (pos, outputChannelData, numOutputChannels, numSamples);
        jamPos.store (pos + numSamples);

        updateLevel();
        captureInput (captureSource, numSamples);
        feedStream();
        return;
    }

    // idle / preview: optional playback ...
    if (currentMode == Mode::preview && previewPlaying.load() && loaded.load())
    {
        const auto pos = previewPos.load();
        renderStems (pos, outputChannelData, numOutputChannels, numSamples);
        previewPos.store (juce::jmin (pos + numSamples, lengthDeviceSamples));
        if (pos + numSamples >= lengthDeviceSamples)
            previewPlaying.store (false);
    }

    // The input/instrument is always live - also while idle and offline, so
    // the meters (and the VRChat stream) work outside jams.
    updateLevel();

    // ... plus the soundcheck stream to the host, independent of playback.
    if (monitorActive.load())
        captureInput (captureSource, numSamples);

    feedStream();
}

void PlayCaptureEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    deviceRate = device->getCurrentSampleRate();

    const int block = juce::jmax (2048, device->getCurrentBufferSizeSamples());
    instrument.prepare (deviceRate, block);
    instMono.allocate ((size_t) block, true);
    streamL.allocate ((size_t) block, true);
    streamR.allocate ((size_t) block, true);
    instMonoCapacity = block;

    streamOut.setSourceRate (deviceRate);
}

void PlayCaptureEngine::audioDeviceStopped() {}

//==============================================================================
void PlayCaptureEngine::SenderThread::run()
{
    juce::HeapBlock<float> raw (16384), converted (16384);

    auto emit = [&] (const float* data, int n)
    {
        if (engine.activeSink)
            engine.activeSink (engine.sentSamples, data, n);
        engine.sentSamples += n;
    };

    // Negative total offset: the stream starts with silence, shifting the
    // musician's audio later relative to the backing track.
    auto silenceLeft = engine.leadSilence.load();
    while (silenceLeft > 0 && ! threadShouldExit())
    {
        const int n = (int) juce::jmin ((juce::int64) 16384, silenceLeft);
        juce::FloatVectorOperations::clear (raw.getData(), n);
        engine.captureResampler.pushInput (raw.getData(), n);
        silenceLeft -= n;

        const int out = engine.captureResampler.pullOutput (converted.getData(), 16384);
        if (out > 0)
            emit (converted.getData(), out);
    }
    engine.leadSilence.store (0);

    while (! threadShouldExit())
    {
        wait (5);

        auto* fifo = engine.captureFifo.get();
        if (fifo == nullptr)
            continue;

        while (fifo->getNumReady() > 0 && ! threadShouldExit())
        {
            const int toRead = juce::jmin (fifo->getNumReady(), 8192);

            int s1, n1, s2, n2;
            fifo->prepareToRead (toRead, s1, n1, s2, n2);
            if (n1 > 0) juce::FloatVectorOperations::copy (raw.getData(),      engine.captureBuffer.getData() + s1, n1);
            if (n2 > 0) juce::FloatVectorOperations::copy (raw.getData() + n1, engine.captureBuffer.getData() + s2, n2);
            fifo->finishedRead (n1 + n2);

            engine.captureResampler.pushInput (raw.getData(), n1 + n2);

            int out = 0;
            while ((out = engine.captureResampler.pullOutput (converted.getData(), 8192)) > 0)
            {
                emit (converted.getData(), out);
                if (threadShouldExit())
                    return;
            }
        }
    }
}

//==============================================================================
double PlayCaptureEngine::getPositionSeconds() const
{
    if (deviceRate <= 0.0)
        return 0.0;
    const auto pos = mode.load() == Mode::jam ? jamPos.load() : previewPos.load();
    return (double) pos / deviceRate;
}

double PlayCaptureEngine::getLengthSeconds() const
{
    return deviceRate > 0.0 ? (double) lengthDeviceSamples / deviceRate : 0.0;
}

juce::String PlayCaptureEngine::getDeviceTypeName() const
{
    return deviceManager.getCurrentAudioDeviceType();
}

juce::String PlayCaptureEngine::getDeviceName() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();
    return {};
}

bool PlayCaptureEngine::isUsingAsio() const
{
    return getDeviceTypeName() == "ASIO";
}

int PlayCaptureEngine::getReportedLatencySamples() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    return 0;
}

double PlayCaptureEngine::getReportedLatencyMs() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto rate = device->getCurrentSampleRate();
        if (rate > 0.0)
            return getReportedLatencySamples() * 1000.0 / rate;
    }
    return 0.0;
}

double PlayCaptureEngine::getManualOffsetMs()
{
    return (double) settings::get ("manualOffsetMs", 0.0);
}

void PlayCaptureEngine::setManualOffsetMs (double ms)
{
    settings::set ("manualOffsetMs", ms);
}

} // namespace bandjam
