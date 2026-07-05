#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "musician/DownloadStore.h"
#include "musician/InstrumentHost.h"
#include "common/SongLoader.h"
#include "common/StreamOutput.h"
#include "common/StreamingResampler.h"
#include <atomic>
#include <functional>

namespace bandjam
{
/** The musician's audio engine.

    Playback: all stems of a song are decoded into RAM at the device rate
    (asynchronously via songloader, then handed over with adoptSong()) and
    mixed with live per-stem gain/mute - for preview listening and for the
    jam itself.

    Instrument (VST3): an optional plugin (e.g. Superior Drummer 3) renders
    inside this callback, driven by a MIDI input. Its output is always mixed
    into the local monitoring, and it REPLACES the hardware input as the
    captured signal - so the backing track can never leak into the recording
    and no external audio routing is needed.

    Monitor ("soundcheck" during Prepare Jam): the instrument (or hardware
    input) is streamed continuously to the host (no latency compensation -
    it's just for setting levels before the jam).

    Jam capture (the 1:1 principle): on startJam() playback begins at sample 0
    and the signal is captured continuously. The front of the stream is
    discarded to compensate latency:
      - hardware input: inputLatency + outputLatency (+ manual offset), like
        recording latency compensation in a DAW;
      - VST instrument: outputLatency only (+ manual offset) - the signal is
        born digitally, so there is no input path to compensate.
    After the skip, capture sample 0 lines up with backing sample 0. The
    stream is then converted to the song's sample rate and leaves via
    onCaptureBlock with a running sample index in song time - ready to be
    placed 1:1 by the host. */
class PlayCaptureEngine : public juce::AudioIODeviceCallback,
                          private juce::ChangeListener
{
public:
    PlayCaptureEngine();
    ~PlayCaptureEngine() override;

    /** Opens the audio device (1 in / 2 out), restoring saved settings. */
    void initialiseDevice();
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    double getDeviceSampleRate() const;

    /** Fired on the message thread whenever the device setup changes. */
    std::function<void()> onDeviceChanged;

    // -- song loading -----------------------------------------------------------
    /** Takes ownership of stems decoded by songloader (at the device rate).
        Fast - just moves buffers. Decode with songloader::decodeAsync first. */
    bool adoptSong (const LocalSong& meta, std::vector<songloader::DecodedStem>&& stems,
                    juce::String& error);
    void unload();
    bool isLoaded() const noexcept { return loaded.load(); }
    juce::String getLoadedSongId() const { return loadedSongId; }

    int          getNumStems() const noexcept { return stems.size(); }
    juce::String getStemName (int index) const;
    void         setStemGainDb (int index, float gainDb);
    void         setStemMute (int index, bool mute);
    float        getStemLevel (int index) const;

    // -- VST instrument ------------------------------------------------------------
    bool loadInstrument (const juce::File& vst3File, juce::String& error);
    void unloadInstrument();
    bool hasInstrument() const noexcept { return instrument.isLoaded(); }
    juce::String getInstrumentName() const { return instrument.getPluginName(); }
    juce::File getSavedInstrumentFile() const { return instrument.getSavedPluginFile(); }
    void showInstrumentEditor() { instrument.showEditor(); }
    void saveInstrumentState() const { instrument.saveState(); }

    static juce::Array<juce::MidiDeviceInfo> getMidiInputs() { return InstrumentHost::getMidiInputs(); }
    void setMidiInput (const juce::String& identifier) { instrument.setMidiInput (identifier); }
    juce::String getMidiInputIdentifier() const { return instrument.getMidiInputIdentifier(); }
    bool midiOpenFailed() const noexcept { return instrument.midiOpenFailed(); }
    juce::uint32 getMidiEventCount() const noexcept { return instrument.getMidiEventCount(); }
    int getLastMidiNote() const noexcept { return instrument.getLastMidiNote(); }

    // -- VRChat / virtual-mic stream --------------------------------------------
    /** The feed is your own signal (mic or VST instrument), plus the local
        playback (backing track etc.) if includeSong is on. */
    StreamOutput& getStreamOutput() noexcept { return streamOut; }
    void setStreamIncludeSong (bool include) noexcept { streamIncludeSong.store (include); }
    bool getStreamIncludeSong() const noexcept { return streamIncludeSong.load(); }
    void setStreamIncludeInput (bool include) noexcept { streamIncludeInput.store (include); }
    bool getStreamIncludeInput() const noexcept { return streamIncludeInput.load(); }

    // -- preview ----------------------------------------------------------------
    void previewPlay();
    void previewPause();
    void previewStop();
    void previewSeekSeconds (double seconds);
    bool isPreviewPlaying() const noexcept;

    // -- monitor (soundcheck) -----------------------------------------------------
    bool startMonitor (juce::String& error);
    void stopMonitor();
    bool isMonitoring() const noexcept { return monitorActive.load(); }

    /** Sender thread: monitor audio in song rate with a running index. */
    std::function<void (juce::int64 startSample, const float* mono, int numSamples)> onMonitorBlock;

    // -- jam ----------------------------------------------------------------------
    bool startJam (juce::String& error);
    void stopJam();
    bool isJamRunning() const noexcept { return mode.load() == Mode::jam; }

    /** Sender thread: latency-compensated capture audio in song sample rate
        with its running sample index (0 = backing track start). */
    std::function<void (juce::int64 startSample, const float* mono, int numSamples)> onCaptureBlock;

    // -- status --------------------------------------------------------------------
    double getPositionSeconds() const;
    double getLengthSeconds() const;
    float  getBackingLevel() const noexcept { return backingLevel.load(); }
    float  getMicLevel() const noexcept     { return micLevel.load(); }

    juce::String getDeviceTypeName() const;
    juce::String getDeviceName() const;
    bool         isUsingAsio() const;
    int          getReportedLatencySamples() const;  ///< in + out, device rate
    double       getReportedLatencyMs() const;
    int          getLastAppliedSkipSamples() const noexcept { return lastAppliedSkip; }

    static double getManualOffsetMs();
    static void   setManualOffsetMs (double ms);

    // -- AudioIODeviceCallback ---------------------------------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    enum class Mode { idle, preview, jam };

    struct RamStem
    {
        juce::String name;
        juce::AudioBuffer<float> buffer;       ///< at device rate
        std::atomic<float> gain  { 1.0f };
        std::atomic<bool>  mute  { false };
        std::atomic<float> level { 0.0f };     ///< post-gain peak while playing
    };

    class SenderThread : public juce::Thread
    {
    public:
        explicit SenderThread (PlayCaptureEngine& e) : juce::Thread ("bandjam-capture-sender"), engine (e) {}
        void run() override;
        PlayCaptureEngine& engine;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void renderStems (juce::int64 position, float* const* out, int numOut, int numSamples);
    void captureInput (const float* input, int numSamples);
    void teardownCapturePipeline();

    juce::AudioDeviceManager deviceManager;
    InstrumentHost instrument;

    juce::OwnedArray<RamStem> stems;             ///< fixed while loaded (no live add/remove)
    juce::String loadedSongId;
    std::atomic<bool> loaded { false };
    double songRate       { 44100.0 };            ///< the song's own rate (jam timebase)
    double deviceRate     { 44100.0 };
    juce::int64 lengthDeviceSamples { 0 };

    std::atomic<Mode> mode { Mode::idle };
    std::atomic<bool> previewPlaying { false };
    std::atomic<bool> monitorActive  { false };
    std::atomic<juce::int64> previewPos { 0 };
    std::atomic<juce::int64> jamPos     { 0 };

    std::atomic<float> backingLevel { 0.0f };
    std::atomic<float> micLevel     { 0.0f };

    // Capture pipeline (audio thread -> sender thread), used by jam AND monitor.
    std::unique_ptr<juce::AbstractFifo> captureFifo;
    juce::HeapBlock<float>              captureBuffer;
    int                                 captureCapacity { 0 };
    std::atomic<juce::int64> skipRemaining { 0 };  ///< device samples still to discard (jam only)
    std::atomic<juce::int64> leadSilence   { 0 };  ///< device samples of silence to prepend (jam only)
    int lastAppliedSkip { 0 };

    juce::HeapBlock<float> instMono;               ///< scratch: instrument mono mix
    int instMonoCapacity { 0 };

    StreamOutput streamOut;                        ///< virtual-mic feed (VRChat etc.)
    std::atomic<bool> streamIncludeSong  { true };
    std::atomic<bool> streamIncludeInput { true };
    juce::HeapBlock<float> streamL, streamR;       ///< scratch: stream feed mix

    StreamingResampler            captureResampler;   ///< device rate -> song rate (sender thread)
    juce::int64                   sentSamples { 0 };
    std::function<void (juce::int64, const float*, int)> activeSink; ///< set before the sender starts
    std::unique_ptr<SenderThread> sender;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayCaptureEngine)
};

} // namespace bandjam
