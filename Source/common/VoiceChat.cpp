#include "VoiceChat.h"

namespace bandjam
{
VoiceChat::VoiceChat() : juce::Thread ("bandjam-voice-sender")
{
    sendData.allocate (kSendFifoSize, true);
    mixScratch.allocate (kChunk, true);
    pullScratch.allocate (kChunk, true);
}

VoiceChat::~VoiceChat()
{
    stop();
}

bool VoiceChat::start (const juce::String& talkMicName, juce::String& error)
{
    stop();

    manager.getAvailableDeviceTypes();
    manager.setCurrentAudioDeviceType ("Windows Audio", true);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName  = talkMicName;             // empty = listen-only
    setup.outputDeviceName = {};                      // system default
    setup.useDefaultInputChannels  = true;
    setup.useDefaultOutputChannels = true;

    const auto result = manager.initialise (talkMicName.isNotEmpty() ? 2 : 0, 2, nullptr, false, {}, &setup);
    if (result.isNotEmpty())
    {
        error = result;
        return false;
    }

    if (manager.getCurrentAudioDevice() == nullptr)
    {
        error = "Could not open a voice device.";
        return false;
    }

    currentMic = talkMicName;
    sendFifo   = std::make_unique<juce::AbstractFifo> (kSendFifoSize);
    sentSamples = 0;

    running.store (true);
    manager.addAudioCallback (this);
    startThread();
    return true;
}

void VoiceChat::stop()
{
    if (! running.exchange (false))
        return;

    signalThreadShouldExit();
    notify();
    stopThread (4000);

    manager.removeAudioCallback (this);
    manager.closeAudioDevice();

    const juce::ScopedLock sl (speakersLock);
    speakers.clear();
}

//==============================================================================
void VoiceChat::receiveVoice (const juce::String& speakerName, const juce::int16* samples, int numSamples)
{
    if (! running.load() || samples == nullptr || numSamples <= 0)
        return;

    const juce::ScopedLock sl (speakersLock);

    auto& slot = speakers[speakerName];
    if (slot == nullptr)
        slot = std::make_unique<Speaker>();

    slot->lastHeardMs = juce::Time::getMillisecondCounter();

    int start1, size1, start2, size2;
    slot->fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    constexpr float scale = 1.0f / 32768.0f;
    for (int i = 0; i < size1; ++i)
        slot->data[(size_t) (start1 + i)] = (float) samples[i] * scale;
    for (int i = 0; i < size2; ++i)
        slot->data[(size_t) (start2 + i)] = (float) samples[size1 + i] * scale;

    slot->fifo.finishedWrite (size1 + size2);   // overflow: excess is dropped
}

//==============================================================================
void VoiceChat::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                  float* const* outputChannelData, int numOutputChannels,
                                                  int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // -- capture -> send FIFO ---------------------------------------------------
    const float* in = numInputChannels > 0 ? inputChannelData[0] : nullptr;
    if (in != nullptr)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (in[i]));
        talkLevel.store (peak);

        if (talking.load() && sendFifo != nullptr)
        {
            captureResampler.pushInput (in, numSamples);

            float converted[512];
            int got;
            while ((got = captureResampler.pullOutput (converted, 512)) > 0)
            {
                int start1, size1, start2, size2;
                sendFifo->prepareToWrite (got, start1, size1, start2, size2);
                for (int i = 0; i < size1; ++i) sendData[(size_t) (start1 + i)] = converted[i];
                for (int i = 0; i < size2; ++i) sendData[(size_t) (start2 + i)] = converted[size1 + i];
                sendFifo->finishedWrite (size1 + size2);
            }
            notify();   // wake the sender thread
        }
        else
        {
            captureResampler.reset();
        }
    }
    else
    {
        talkLevel.store (0.0f);
    }

    // -- playback: mix all speakers ------------------------------------------------
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    float* outL = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
    if (outL == nullptr)
        return;
    float* outR = numOutputChannels > 1 ? outputChannelData[1] : nullptr;

    {
        const juce::ScopedTryLock sl (speakersLock);
        if (sl.isLocked())
        {
            const int jitterSamples = (int) (kVoiceRate * 0.15);   // ~150 ms

            while (playbackResampler.availableOutput() < numSamples)
            {
                juce::FloatVectorOperations::clear (mixScratch.getData(), kChunk);

                for (auto& [name, speaker] : speakers)
                {
                    juce::ignoreUnused (name);
                    const int ready = speaker->fifo.getNumReady();

                    if (! speaker->consuming)
                    {
                        if (ready < jitterSamples)
                            continue;
                        speaker->consuming = true;
                    }

                    const int n = juce::jmin (kChunk, ready);
                    if (n <= 0)
                    {
                        speaker->consuming = false;   // ran dry - rebuffer
                        continue;
                    }

                    int start1, size1, start2, size2;
                    speaker->fifo.prepareToRead (n, start1, size1, start2, size2);
                    for (int i = 0; i < size1; ++i) mixScratch[(size_t) i] += speaker->data[(size_t) (start1 + i)];
                    for (int i = 0; i < size2; ++i) mixScratch[(size_t) (size1 + i)] += speaker->data[(size_t) (start2 + i)];
                    speaker->fifo.finishedRead (size1 + size2);
                }

                playbackResampler.pushInput (mixScratch.getData(), kChunk);
            }
        }
    }

    int done = 0;
    while (done < numSamples)
    {
        const int got = playbackResampler.pullOutput (pullScratch.getData(),
                                                      juce::jmin (kChunk, numSamples - done));
        if (got <= 0)
            break;

        juce::FloatVectorOperations::add (outL + done, pullScratch.getData(), got);
        if (outR != nullptr)
            juce::FloatVectorOperations::add (outR + done, pullScratch.getData(), got);
        done += got;
    }
}

void VoiceChat::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    deviceInRate = deviceOutRate = device->getCurrentSampleRate();
    captureResampler.prepare (deviceInRate, kVoiceRate, 4096);
    playbackResampler.prepare (kVoiceRate, deviceOutRate, 4096);
}

void VoiceChat::audioDeviceStopped() {}

//==============================================================================
void VoiceChat::run()
{
    // Pulls captured voice out of the FIFO in ~50 ms blocks and hands it to
    // the network callback (blocking sends never touch the audio thread).
    constexpr int kBlock = 1200;   // 50 ms @ 24 kHz
    juce::HeapBlock<float> block ((size_t) kBlock);

    while (! threadShouldExit())
    {
        if (sendFifo == nullptr || sendFifo->getNumReady() < kBlock)
        {
            wait (20);
            continue;
        }

        int start1, size1, start2, size2;
        sendFifo->prepareToRead (kBlock, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) block[(size_t) i] = sendData[(size_t) (start1 + i)];
        for (int i = 0; i < size2; ++i) block[(size_t) (size1 + i)] = sendData[(size_t) (start2 + i)];
        sendFifo->finishedRead (size1 + size2);

        if (onVoiceBlock)
            onVoiceBlock (sentSamples, block.getData(), kBlock);
        sentSamples += kBlock;
    }
}

} // namespace bandjam
