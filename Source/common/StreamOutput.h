#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "common/StreamingResampler.h"
#include <atomic>

namespace bandjam
{
/** Feeds a copy of BandJam's audio into a second output device - typically a
    virtual audio cable (VB-CABLE's "CABLE Input") whose other end is selected
    as the microphone in VRChat/Discord/OBS. This replaces Voicemeeter for the
    common "stream my PC audio through the mic" setup.

    Note: Windows only lets kernel drivers create virtual audio devices, so
    the (free) VB-CABLE driver provides the cable; BandJam does the mixing
    and routing that people normally run Voicemeeter for.

    It runs its own WASAPI device (shared mode - it can coexist with the main
    ASIO engine) and mixes three kinds of sources:
      - the feed: stereo audio push()ed from the main engine's callback at
        that engine's sample rate (a lock-free FIFO + resampler bridge the
        two clocks; if the clocks drift too far apart, the FIFO is trimmed);
      - an optional talk input device (own mic passthrough), so the band can
        chat in VRChat even while no jam is running - gated by setTalkEnabled;
      - any number of aux sources (e.g. per-app loopback captures of Spotify
        & co.), each with its own FIFO + resampler pair.

    start()/stop() and the device lists are message-thread; push() and the
    aux pushes are safe from any audio thread. */
class StreamOutput : public juce::AudioIODeviceCallback
{
public:
    StreamOutput();
    ~StreamOutput() override;

    /** WASAPI device names ("CABLE Input ..." shows up here once VB-CABLE
        is installed). */
    static juce::StringArray getOutputDeviceNames();
    static juce::StringArray getInputDeviceNames();

    /** Opens the stream device (and the optional talk input). */
    bool start (const juce::String& outputDevice, const juce::String& talkInputDevice,
                juce::String& error);
    void stop();
    bool isRunning() const noexcept { return running.load(); }
    juce::String getOutputDeviceName() const { return currentOutput; }

    /** Sample rate of the audio that will be push()ed (main engine rate). */
    void setSourceRate (double rate) noexcept { sourceRate.store (rate > 0.0 ? rate : 44100.0); }

    /** Main engine audio thread: appends stereo feed audio. Drops when the
        stream is off or the FIFO is full - never blocks. */
    void push (const float* left, const float* right, int numSamples) noexcept;

    float getLevel() const noexcept { return level.load(); }

    /** Talk mic: mixed into the stream only while enabled (the meter still
        shows the incoming level either way). */
    void setTalkEnabled (bool enabled) noexcept { talkEnabled.store (enabled); }
    bool isTalkEnabled() const noexcept { return talkEnabled.load(); }
    float getTalkLevel() const noexcept { return talkLevel.load(); }

    //==============================================================================
    /** An extra stereo source mixed into the stream (per-app loopback
        captures). Create/remove on the message thread; pushAudio is safe
        from any thread. The producer's rate can differ from the stream
        device - each aux has its own FIFO + resamplers. */
    class AuxSource
    {
    public:
        void setRate (double rate) noexcept { sourceRate.store (rate > 0.0 ? rate : 48000.0); }
        void pushAudio (const float* interleavedStereo, int numFrames) noexcept;

    private:
        friend class StreamOutput;
        AuxSource();

        juce::AbstractFifo fifo;
        juce::HeapBlock<float> data;                  ///< interleaved stereo
        std::atomic<double> sourceRate { 48000.0 };
        double preparedRate { 0.0 };                  ///< stream thread only
        StreamingResampler resamplerL, resamplerR;
        juce::HeapBlock<float> chunkL, chunkR, outL, outR;

        JUCE_DECLARE_NON_COPYABLE (AuxSource)
    };

    AuxSource* createAuxSource();
    void removeAuxSource (AuxSource* source);

    // -- AudioIODeviceCallback (stream device thread) -----------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void mixAuxSources (float* outL, float* outR, int numSamples);

    juce::AudioDeviceManager manager;
    juce::String currentOutput;

    // FIFO of interleaved stereo frames at source rate (single producer:
    // main engine callback; single consumer: stream device callback).
    std::unique_ptr<juce::AbstractFifo> fifo;
    juce::HeapBlock<float> fifoData;              ///< 2 * capacity floats
    int fifoCapacityFrames { 0 };

    std::atomic<double> sourceRate { 44100.0 };
    double deviceRate           { 48000.0 };
    double resamplerSourceRate  { 0.0 };          ///< stream thread: last prepared rate

    StreamingResampler resamplerL, resamplerR;
    juce::HeapBlock<float> chunkL, chunkR, outScratchL, outScratchR;
    static constexpr int kChunk = 512;

    // Aux sources: mutated on the message thread, read by the stream callback
    // under a try-lock (a skipped block during add/remove is inaudible).
    juce::CriticalSection auxLock;
    juce::OwnedArray<AuxSource> auxSources;
    int deviceBlockSize { 0 };

    std::atomic<bool>  running { false };
    std::atomic<float> level   { 0.0f };
    std::atomic<bool>  talkEnabled { true };
    std::atomic<float> talkLevel   { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamOutput)
};

} // namespace bandjam
