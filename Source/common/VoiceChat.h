#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "common/Protocol.h"
#include "common/StreamingResampler.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace bandjam
{
/** Band voice chat: talk to the other connected members outside (or during)
    a jam, independent of the main audio engine.

    It runs its own little WASAPI device (shared mode - coexists with the
    main ASIO/WASAPI engine and the VRChat stream):

      - capture: the selected talk mic. While "talking" is enabled the input
        is resampled to kVoiceRate mono and leaves in small blocks via
        onVoiceBlock (a sender thread, so the network never blocks audio).
      - playback: the system default output. Every remote speaker gets a
        jitter buffer (~150 ms) and everything is mixed together.

    start()/stop()/setTalking() are message-thread; receiveVoice() may be
    called from any (network reader) thread. */
class VoiceChat : public juce::AudioIODeviceCallback,
                  private juce::Thread
{
public:
    VoiceChat();
    ~VoiceChat() override;

    /** Opens the device. @p talkMicName may be empty (listen-only). */
    bool start (const juce::String& talkMicName, juce::String& error);
    void stop();
    bool isRunning() const noexcept { return running.load(); }
    juce::String getTalkMicName() const { return currentMic; }

    /** Enables/disables sending the mic (the "Talk" toggle). */
    void setTalking (bool shouldTalk) noexcept { talking.store (shouldTalk); }
    bool isTalking() const noexcept { return talking.load(); }
    float getTalkLevel() const noexcept { return talkLevel.load(); }

    /** Sender thread: mono voice at kVoiceRate with a running sample index. */
    std::function<void (juce::int64 startSample, const float* mono, int numSamples)> onVoiceBlock;

    /** Network reader threads: a remote speaker's voice block. */
    void receiveVoice (const juce::String& speakerName, const juce::int16* samples, int numSamples);

    // -- AudioIODeviceCallback ----------------------------------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    /** One remote speaker's jitter buffer (mono, kVoiceRate). */
    struct Speaker
    {
        static constexpr int kCapacity = 1 << 16;   ///< ~2.7 s @ 24 kHz

        juce::AbstractFifo fifo { kCapacity };
        juce::HeapBlock<float> data { (size_t) kCapacity, true };
        bool consuming { false };                    ///< audio thread only
        juce::uint32 lastHeardMs { 0 };
    };

    void run() override;   // sender thread

    juce::AudioDeviceManager manager;
    juce::String currentMic;

    std::atomic<bool>  running   { false };
    std::atomic<bool>  talking   { false };
    std::atomic<float> talkLevel { 0.0f };

    double deviceInRate  { 48000.0 };
    double deviceOutRate { 48000.0 };

    // Capture: device rate -> kVoiceRate (audio thread), FIFO -> sender thread.
    StreamingResampler captureResampler;
    std::unique_ptr<juce::AbstractFifo> sendFifo;
    juce::HeapBlock<float> sendData;
    static constexpr int kSendFifoSize = 1 << 15;
    juce::int64 sentSamples { 0 };

    // Playback: mixed speakers at kVoiceRate -> device rate (audio thread).
    juce::CriticalSection speakersLock;
    std::map<juce::String, std::unique_ptr<Speaker>> speakers;
    StreamingResampler playbackResampler;
    juce::HeapBlock<float> mixScratch, pullScratch;
    static constexpr int kChunk = 256;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChat)
};

} // namespace bandjam
