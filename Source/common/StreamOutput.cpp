#include "StreamOutput.h"
#include <cmath>
#include <cstring>

namespace bandjam
{
namespace
{
    constexpr const char* kWasapiTypeName = "Windows Audio";

    juce::StringArray listWasapiDevices (bool inputs)
    {
        // Create the WASAPI (shared mode) device type directly: a fresh
        // AudioDeviceManager has an *empty* device-type list until it is
        // initialised, so asking it for the current type returns nothing.
        std::unique_ptr<juce::AudioIODeviceType> type (
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));

        if (type == nullptr)
            return {};

        type->scanForDevices();
        return type->getDeviceNames (inputs);
    }
}

StreamOutput::StreamOutput() = default;

StreamOutput::~StreamOutput()
{
    stop();
}

juce::StringArray StreamOutput::getOutputDeviceNames() { return listWasapiDevices (false); }
juce::StringArray StreamOutput::getInputDeviceNames()  { return listWasapiDevices (true); }

//==============================================================================
bool StreamOutput::start (const juce::String& outputDevice, const juce::String& talkInputDevice,
                          juce::String& error)
{
    stop();

    if (outputDevice.isEmpty())
        return false;

    // Force the manager to build its device-type list first, otherwise
    // setCurrentAudioDeviceType() is a silent no-op on a fresh manager.
    manager.getAvailableDeviceTypes();
    manager.setCurrentAudioDeviceType (kWasapiTypeName, false);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName        = outputDevice;
    setup.inputDeviceName         = talkInputDevice;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    // selectDefaultDeviceOnFailure=false: silently opening the speakers
    // instead of the cable would be worse than failing.
    const auto err = manager.initialise (talkInputDevice.isEmpty() ? 0 : 2, 2,
                                         nullptr, false, {}, &setup);
    if (err.isNotEmpty() || manager.getCurrentAudioDevice() == nullptr)
    {
        error = err.isNotEmpty() ? err : "Could not open the device.";
        manager.closeAudioDevice();
        return false;
    }

    // ~2.7 s at 48 kHz - plenty for clock drift, small enough to trim fast.
    fifoCapacityFrames = 1 << 17;
    fifoData.allocate ((size_t) fifoCapacityFrames * 2, true);
    fifo = std::make_unique<juce::AbstractFifo> (fifoCapacityFrames);

    currentOutput = outputDevice;
    manager.addAudioCallback (this);   // fires audioDeviceAboutToStart -> running=true
    return true;
}

void StreamOutput::stop()
{
    running.store (false);
    manager.removeAudioCallback (this);
    manager.closeAudioDevice();
    fifo.reset();
    currentOutput.clear();
    level.store (0.0f);
    talkLevel.store (0.0f);
}

//==============================================================================
StreamOutput::AuxSource::AuxSource()
    : fifo (1 << 16)   // ~1.3 s at 48 kHz
{
    data.allocate ((size_t) fifo.getTotalSize() * 2, true);
    chunkL.allocate ((size_t) kChunk, true);
    chunkR.allocate ((size_t) kChunk, true);
}

void StreamOutput::AuxSource::pushAudio (const float* interleavedStereo, int numFrames) noexcept
{
    if (interleavedStereo == nullptr || numFrames <= 0)
        return;

    const int n = juce::jmin (numFrames, fifo.getFreeSpace());   // drop on overflow

    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    if (n1 > 0) std::memcpy (data.getData() + (size_t) s1 * 2, interleavedStereo, (size_t) n1 * 2 * sizeof (float));
    if (n2 > 0) std::memcpy (data.getData() + (size_t) s2 * 2, interleavedStereo + (size_t) n1 * 2, (size_t) n2 * 2 * sizeof (float));
    fifo.finishedWrite (n1 + n2);
}

StreamOutput::AuxSource* StreamOutput::createAuxSource()
{
    auto* source = new AuxSource();
    if (deviceBlockSize > 0)
    {
        source->outL.allocate ((size_t) deviceBlockSize, true);
        source->outR.allocate ((size_t) deviceBlockSize, true);
    }

    const juce::ScopedLock lock (auxLock);
    auxSources.add (source);
    return source;
}

void StreamOutput::removeAuxSource (AuxSource* source)
{
    const juce::ScopedLock lock (auxLock);
    auxSources.removeObject (source);
}

void StreamOutput::mixAuxSources (float* outL, float* outR, int numSamples)
{
    const juce::ScopedTryLock lock (auxLock);
    if (! lock.isLocked())
        return;   // being mutated - one silent block is fine

    for (auto* aux : auxSources)
    {
        if (aux->outL.getData() == nullptr)
            continue;   // scratch not sized yet (created before device start)

        const double src = aux->sourceRate.load();
        if (std::abs (src - aux->preparedRate) > 0.5)
        {
            aux->preparedRate = src;
            aux->resamplerL.prepare (src, deviceRate, kChunk);
            aux->resamplerR.prepare (src, deviceRate, kChunk);
        }

        // Trim if the FIFO backed up (producer clock faster than us).
        const int highWater = (int) (src * 0.5);
        if (aux->fifo.getNumReady() > highWater)
        {
            const int drop = aux->fifo.getNumReady() - (int) (src * 0.2);
            int s1, n1, s2, n2;
            aux->fifo.prepareToRead (drop, s1, n1, s2, n2);
            aux->fifo.finishedRead (n1 + n2);
        }

        while (aux->resamplerL.availableOutput() < numSamples && aux->fifo.getNumReady() > 0)
        {
            const int want = juce::jmin (kChunk, aux->fifo.getNumReady());
            int s1, n1, s2, n2;
            aux->fifo.prepareToRead (want, s1, n1, s2, n2);

            int written = 0;
            auto readPart = [&] (int start, int count)
            {
                const float* srcData = aux->data.getData() + (size_t) start * 2;
                for (int i = 0; i < count; ++i)
                {
                    aux->chunkL[written + i] = srcData[i * 2];
                    aux->chunkR[written + i] = srcData[i * 2 + 1];
                }
                written += count;
            };
            readPart (s1, n1);
            readPart (s2, n2);
            aux->fifo.finishedRead (n1 + n2);

            aux->resamplerL.pushInput (aux->chunkL.getData(), written);
            aux->resamplerR.pushInput (aux->chunkR.getData(), written);
        }

        const int gotL = aux->resamplerL.pullOutput (aux->outL.getData(), numSamples);
        const int gotR = aux->resamplerR.pullOutput (aux->outR.getData(), numSamples);
        if (gotL > 0)
            juce::FloatVectorOperations::add (outL, aux->outL.getData(), gotL);
        if (outR != nullptr && gotR > 0)
            juce::FloatVectorOperations::add (outR, aux->outR.getData(), gotR);
    }
}

//==============================================================================
void StreamOutput::push (const float* left, const float* right, int numSamples) noexcept
{
    if (! running.load() || fifo == nullptr || numSamples <= 0)
        return;

    const int n = juce::jmin (numSamples, fifo->getFreeSpace());   // drop on overflow

    int s1, n1, s2, n2;
    fifo->prepareToWrite (n, s1, n1, s2, n2);

    auto writePart = [&] (int start, int count, int offset)
    {
        float* dest = fifoData.getData() + (size_t) start * 2;
        for (int i = 0; i < count; ++i)
        {
            dest[i * 2]     = left  != nullptr ? left [offset + i] : 0.0f;
            dest[i * 2 + 1] = right != nullptr ? right[offset + i]
                                               : (left != nullptr ? left[offset + i] : 0.0f);
        }
    };
    writePart (s1, n1, 0);
    writePart (s2, n2, n1);
    fifo->finishedWrite (n1 + n2);
}

//==============================================================================
void StreamOutput::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    deviceRate = device->getCurrentSampleRate();

    const int block = juce::jmax (2048, device->getCurrentBufferSizeSamples());
    chunkL.allocate ((size_t) kChunk, true);
    chunkR.allocate ((size_t) kChunk, true);
    outScratchL.allocate ((size_t) block, true);
    outScratchR.allocate ((size_t) block, true);

    deviceBlockSize = block;
    {
        const juce::ScopedLock lock (auxLock);
        for (auto* aux : auxSources)
        {
            aux->outL.allocate ((size_t) block, true);
            aux->outR.allocate ((size_t) block, true);
            aux->preparedRate = 0.0;
        }
    }

    resamplerSourceRate = 0.0;   // force re-prepare on the first callback
    running.store (true);
}

void StreamOutput::audioDeviceStopped()
{
    running.store (false);
}

void StreamOutput::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                     float* const* outputChannelData, int numOutputChannels,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (numOutputChannels <= 0 || outputChannelData[0] == nullptr)
        return;

    float* outL = outputChannelData[0];
    float* outR = numOutputChannels > 1 && outputChannelData[1] != nullptr ? outputChannelData[1] : nullptr;

    // 1) Talk mic passthrough (works even when no jam audio flows). The meter
    //    tracks the incoming level even while disabled.
    if (numInputChannels > 0 && inputChannelData[0] != nullptr)
    {
        float talkPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            talkPeak = juce::jmax (talkPeak, std::abs (inputChannelData[0][i]));
        talkLevel.store (talkPeak);

        if (talkEnabled.load())
        {
            juce::FloatVectorOperations::add (outL, inputChannelData[0], numSamples);
            if (outR != nullptr)
                juce::FloatVectorOperations::add (outR, inputChannelData[0], numSamples);
        }
    }
    else
    {
        talkLevel.store (0.0f);
    }

    // 2) Jam/engine feed from the FIFO, converted to this device's rate.
    if (fifo != nullptr)
    {
        const double src = sourceRate.load();

        // The two devices run on unrelated clocks: re-prepare on rate change
        // (rare; the tiny allocation glitch is fine for a chat/stream path).
        if (std::abs (src - resamplerSourceRate) > 0.5)
        {
            resamplerSourceRate = src;
            resamplerL.prepare (src, deviceRate, kChunk);
            resamplerR.prepare (src, deviceRate, kChunk);
        }

        // Trim if the FIFO backed up (source clock slightly faster than us).
        const int highWater = (int) (src * 0.5);
        if (fifo->getNumReady() > highWater)
        {
            const int drop = fifo->getNumReady() - (int) (src * 0.2);
            int s1, n1, s2, n2;
            fifo->prepareToRead (drop, s1, n1, s2, n2);
            fifo->finishedRead (n1 + n2);
        }

        const int n = numSamples;   // outScratch* are sized to the device block

        // Pull FIFO frames through the resamplers until we can fill the block.
        while (resamplerL.availableOutput() < n && fifo->getNumReady() > 0)
        {
            const int want = juce::jmin (kChunk, fifo->getNumReady());
            int s1, n1, s2, n2;
            fifo->prepareToRead (want, s1, n1, s2, n2);

            int written = 0;
            auto readPart = [&] (int start, int count)
            {
                const float* srcData = fifoData.getData() + (size_t) start * 2;
                for (int i = 0; i < count; ++i)
                {
                    chunkL[written + i] = srcData[i * 2];
                    chunkR[written + i] = srcData[i * 2 + 1];
                }
                written += count;
            };
            readPart (s1, n1);
            readPart (s2, n2);
            fifo->finishedRead (n1 + n2);

            resamplerL.pushInput (chunkL.getData(), written);
            resamplerR.pushInput (chunkR.getData(), written);
        }

        const int gotL = resamplerL.pullOutput (outScratchL.getData(), n);
        const int gotR = resamplerR.pullOutput (outScratchR.getData(), n);

        if (gotL > 0)
            juce::FloatVectorOperations::add (outL, outScratchL.getData(), gotL);
        if (outR != nullptr && gotR > 0)
            juce::FloatVectorOperations::add (outR, outScratchR.getData(), gotR);
        // Underrun remainder stays silent - a brief gap, never a time shift.
    }

    // 3) Aux sources (per-app loopback captures of Spotify & co.).
    mixAuxSources (outL, outR, numSamples);

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax (peak, std::abs (outL[i]));
    level.store (peak);
}

} // namespace bandjam
