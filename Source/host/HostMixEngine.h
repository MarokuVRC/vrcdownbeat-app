#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "host/SongLibrary.h"
#include "common/SongLoader.h"
#include "common/StreamOutput.h"
#include "common/StreamingResampler.h"
#include <atomic>
#include <functional>

namespace bandjam
{
/** The host's live mix engine.

    Everything runs in the song's own sample rate ("Song-Zeit"): the backing
    stems live in RAM at song rate (decoded asynchronously via songloader and
    handed over in prepareMonitor()), and every musician's incoming stream is
    written into a per-musician ring buffer.

    States:
    - monitor: the "Prepare Jam" soundcheck. Musicians stream their input
      live (monitorBlock); each stream is played as soon as ~150 ms are
      buffered, so the host can set levels before the jam. No backing track.
    - gate -> playing: the jam itself. Playback starts only once every
      musician has at least leadSeconds buffered (the "3-Sekunden-Gate") -
      from then on backing track and musician streams are read in lock-step
      from sample 0, which makes the mix sample-accurate. Unwritten regions
      (network dropouts) are silence; a dropout can never cause a permanent
      offset. Optionally the final mix is written to a WAV file.

    Only at the very output is the mix converted to the device rate, if the
    device could not be opened at song rate. */
class HostMixEngine : public juce::AudioIODeviceCallback,
                      private juce::ChangeListener
{
public:
    enum class State { idle, monitor, gate, playing };

    HostMixEngine() = default;
    ~HostMixEngine() override;

    /** Opens the host's audio device (restoring saved settings) and keeps it
        open with the callback attached: the mix plays whenever a jam runs,
        and the host's own input is live at all times (talk / instrument). */
    void initialiseDevice();
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** Host's own input (talk mic or instrument, first two input channels):
        mixed live into the local output, the VRChat stream and - during a
        jam - the WAV recording. The host hears the band mix ~3 s behind the
        musicians and plays along to what they hear, so a live passthrough is
        exactly in time with the delayed mix. */
    void  setHostInputGainDb (float db) { hostInGain.store (juce::Decibels::decibelsToGain (db)); }
    void  setHostInputMute (bool mute)  { hostInMute.store (mute); }
    float getHostInputLevel() const noexcept { return hostInLevel.load(); }

    /** What the VRChat/virtual-mic stream contains ("Audio Stream" tab):
        the band mix (jam/preview playback) and the host's own input are
        included independently of the local monitor mute. */
    void setStreamIncludeMix (bool include) noexcept   { streamIncludeMix.store (include); }
    bool getStreamIncludeMix() const noexcept          { return streamIncludeMix.load(); }
    void setStreamIncludeInput (bool include) noexcept { streamIncludeInput.store (include); }
    bool getStreamIncludeInput() const noexcept        { return streamIncludeInput.load(); }

    // -- preview player -----------------------------------------------------------
    /** Local listening outside a jam: adopts stems decoded at the current
        device rate (songloader::decodeAsync) and plays them through the
        host's device whenever no jam is active. Preparing a jam unloads it. */
    bool adoptPreview (const juce::String& previewName, std::vector<songloader::DecodedStem>&& decodedStems,
                       juce::String& error);
    void unloadPreview();
    bool isPreviewLoaded() const noexcept  { return previewLoaded.load(); }
    bool isPreviewPlaying() const noexcept { return previewPlaying.load(); }
    juce::String getPreviewName() const    { return previewName; }

    void previewPlay()  { if (previewLoaded.load()) previewPlaying.store (true); }
    void previewPause() { previewPlaying.store (false); }
    void previewStop()  { previewPlaying.store (false); previewPos.store (0); }
    void previewSeekSeconds (double seconds);
    double getPreviewPositionSeconds() const;
    double getPreviewLengthSeconds() const;

    /** Live mixer for the preview stems (like the jam stems, message thread). */
    int          getNumPreviewStems() const noexcept { return previewStems.size(); }
    juce::String getPreviewStemName (int index) const;
    void         setPreviewStemGainDb (int index, float gainDb);
    void         setPreviewStemMute (int index, bool mute);
    float        getPreviewStemLevel (int index) const;

    /** Adopts pre-decoded stems (at song rate), opens the output device and
        creates one stream per expected musician, then enters monitor state
        (audio callback active, playing incoming soundcheck streams). */
    bool prepareMonitor (const LibrarySong& song, std::vector<songloader::DecodedStem>&& decodedStems,
                         const juce::StringArray& performerNames, double leadSecondsToUse,
                         juce::String& error);

    /** Call at "Go": resets the musician streams to jam time and enters the
        gate; playback begins once the gate condition is met (or after a
        safety timeout). Optionally records the jam as separate stems: one
        mono WAV per musician plus the host input live during the jam, and
        the backing-track stems written out when the jam stops - everything
        sample-aligned in one folder under recordings/, with a meta.json the
        Recordings tab reads. */
    void startJamPlayback (bool recordToFile);

    void stopAndClose();

    State getState() const noexcept  { return state.load(); }
    bool  isPrepared() const noexcept { return state.load() != State::idle; }
    bool  hasStarted() const noexcept { return state.load() == State::playing; }

    juce::File getRecordingFolder() const { return recordingDir; }
    /** Message thread; fired after stopAndClose() once all stem files and
        meta.json of a recorded jam are on disk (receives the jam folder). */
    std::function<void (juce::File)> onRecordingSaved;

    /** Reader threads: jam audio at its absolute position (gate/playing only). */
    void pushJamAudio (const juce::String& performerName, juce::int64 startSample,
                       const juce::int16* samples, int numSamples);
    /** Reader threads: soundcheck audio (monitor state only). */
    void pushMonitorAudio (const juce::String& performerName, juce::int64 startSample,
                           const juce::int16* samples, int numSamples);

    // -- status / mixer (message thread) ---------------------------------------
    double getPositionSeconds() const;
    double getSongLengthSeconds() const;
    double getLeadSeconds() const noexcept { return leadSeconds; }

    juce::StringArray getPerformerNames() const;
    double getBufferedSeconds (const juce::String& performerName) const;
    juce::StringArray getWaitingFor() const;
    float  getPerformerLevel (const juce::String& performerName) const;
    void   setPerformerGainDb (const juce::String& performerName, float gainDb);
    void   setPerformerMute (const juce::String& performerName, bool mute);

    int          getNumStems() const noexcept { return stems.size(); }
    juce::String getStemName (int index) const;
    void         setStemGainDb (int index, float gainDb);
    void         setStemMute (int index, bool mute);

    float getOutputLevel() const noexcept { return outputLevel.load(); }

    /** VRChat/virtual-mic stream: receives a copy of the master mix whenever
        soundcheck/jam audio plays; its own talk input works anytime. */
    StreamOutput& getStreamOutput() noexcept { return streamOut; }

    // -- AudioIODeviceCallback ---------------------------------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct RamStem
    {
        juce::String name;
        juce::AudioBuffer<float> buffer;      ///< at song rate
        std::atomic<float> gain  { 1.0f };
        std::atomic<bool>  mute  { false };
        std::atomic<float> level { 0.0f };    ///< post-gain peak (preview player)
    };

    /** One musician's jitter/positioning buffer (song rate, mono).
        Power-of-two ring indexed by absolute sample position. The writer
        (reader thread) fills cells; the audio thread copies and re-zeroes
        them, so unwritten regions are always silence. */
    struct PerformerStream
    {
        PerformerStream (const juce::String& performerName, int capacityPowerOfTwo)
            : name (performerName), capacity (capacityPowerOfTwo), mask (capacityPowerOfTwo - 1)
        {
            ring.allocate ((size_t) capacity, true);
        }

        void write (juce::int64 startSample, const juce::int16* samples, int numSamples,
                    juce::int64 readPos);
        void consume (juce::int64 position, float* dest, int numSamples);

        juce::String name;
        juce::HeapBlock<float> ring;
        int capacity;
        juce::int64 mask;
        juce::CriticalSection cellLock;

        std::atomic<juce::int64> written { 0 };  ///< frontier (max end position seen)
        std::atomic<float> gain  { 1.0f };
        std::atomic<float> level { 0.0f };
        std::atomic<bool>  mute  { false };

        // Monitor (soundcheck) playback state.
        std::atomic<juce::int64> monitorReadPos { 0 };
        bool monitorConsuming { false };          ///< audio thread only
    };

    PerformerStream* findStream (const juce::String& performerName) const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void renderChunk (State st, float* left, float* right, int numSamples);
    /** Final jam mix in song time; advances playPos; feeds the recorder. */
    void renderSongChunk (float* left, float* right, int numSamples);
    /** Soundcheck mix: whatever the musicians are streaming right now. */
    void renderMonitorChunk (float* left, float* right, int numSamples);

    juce::AudioDeviceManager deviceManager;

    juce::OwnedArray<RamStem> stems;
    juce::OwnedArray<PerformerStream> streams;  ///< fixed between prepareMonitor() and stopAndClose()
    mutable juce::CriticalSection streamsLock;  ///< guards push*Audio() vs. stopAndClose()

    juce::String songName;
    double      songRate    { 44100.0 };
    double      deviceRate  { 44100.0 };
    juce::int64 songLength  { 0 };
    double      leadSeconds { 3.0 };
    juce::int64 leadSamples { 0 };

    std::atomic<State> state { State::idle };
    std::atomic<juce::int64> playPos { 0 };
    juce::uint32 armTimeMs { 0 };

    StreamingResampler outResamplerL, outResamplerR;
    juce::HeapBlock<float> chunkL, chunkR, streamScratch;
    static constexpr int kChunk = 512;

    std::atomic<float> outputLevel { 0.0f };

    // Host's own live input (talk mic / instrument). Muted by default so a
    // default-device mic can't feed back before the UI applies the settings.
    std::atomic<float> hostInGain  { 1.0f };
    std::atomic<bool>  hostInMute  { true };
    std::atomic<float> hostInLevel { 0.0f };
    bool deviceInitialised { false };

    // Bridges the host input (device rate) into the recording (song rate).
    StreamingResampler hostRecResamplerL, hostRecResamplerR;
    juce::HeapBlock<float> hostRecL, hostRecR, recMixL, recMixR;

    StreamOutput streamOut;   ///< virtual-mic feed (VRChat etc.)
    std::atomic<bool> streamIncludeMix   { true };
    std::atomic<bool> streamIncludeInput { true };
    juce::HeapBlock<float> streamFeedL, streamFeedR;   ///< scratch: stream feed mix
    int streamFeedCapacity { 0 };

    // Preview player (decoded at device rate; plays while no jam is active).
    juce::OwnedArray<RamStem> previewStems;
    juce::String previewName;
    double previewRate { 44100.0 };
    juce::int64 previewLength { 0 };
    std::atomic<bool> previewLoaded  { false };
    std::atomic<bool> previewPlaying { false };
    std::atomic<juce::int64> previewPos { 0 };

    // Stem recording (song rate). One live take per musician (mono, raw
    // pre-gain signal) plus the host input (stereo); the backing stems are
    // written from RAM when the jam stops. Writers are created on the
    // message thread in startJamPlayback() *before* state becomes gate, so
    // the audio thread never races their creation; destroyed in
    // stopAndClose() after the callback has been removed.
    struct RecordTrack
    {
        juce::String streamName;    ///< PerformerStream name; empty = host input
        juce::String displayName;
        juce::String fileName;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    };
    RecordTrack* findRecordTrack (const juce::String& streamName) const;

    juce::TimeSliceThread recordThread { "bandjam-record" };
    juce::OwnedArray<RecordTrack> recordTracks;
    RecordTrack* hostRecordTrack { nullptr };
    std::atomic<bool> recordingActive { false };
    juce::File recordingDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostMixEngine)
};

} // namespace bandjam
