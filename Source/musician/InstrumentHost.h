#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <functional>

namespace bandjam
{
/** Hosts one VST3 instrument (e.g. Superior Drummer 3) inside the musician's
    audio engine.

    Why: this removes the whole "route the drum plugin's audio back into the
    interface" problem. The plugin renders inside our audio callback, driven
    by a MIDI input - its output is mixed into the local monitoring AND used
    as the captured signal that goes to the host. The backing track can never
    leak into the recording, and only ONE program (BandJam) needs the ASIO
    driver.

    Threading: loadPlugin()/unloadPlugin()/setMidiInput() are message-thread
    only and must be called while the engine's audio callback is suspended
    (PlayCaptureEngine takes care of that). process()/addToOutput()/mixMono()
    run on the audio thread. */
class InstrumentHost : private juce::MidiInputCallback
{
public:
    /** settingsPrefix separates the persisted plugin/MIDI choice per role
        (musician uses no prefix - existing settings keep working - the host
        engine passes "host"). */
    explicit InstrumentHost (const juce::String& settingsPrefix = {});
    ~InstrumentHost();

    // -- plugin (message thread, callback suspended) -----------------------------
    bool loadPlugin (const juce::File& vst3File, double sampleRate, int blockSize,
                     juce::String& error);
    void unloadPlugin();      ///< saves the plugin state first
    bool isLoaded() const noexcept { return active.load(); }
    juce::String getPluginName() const;

    void showEditor();        ///< opens (or refocuses) the plugin's own window
    void closeEditor();

    /** Persists/restores plugin path + state + MIDI choice via settings. */
    void saveState() const;
    juce::File getSavedPluginFile() const;

    // -- MIDI (message thread) ----------------------------------------------------
    static juce::Array<juce::MidiDeviceInfo> getMidiInputs();
    void setMidiInput (const juce::String& identifier);   ///< empty = none
    juce::String getMidiInputIdentifier() const { return midiIdentifier; }

    /** True when a MIDI port was selected but could not be opened (usually
        another program - e.g. the plugin's standalone app - holds it). */
    bool midiOpenFailed() const noexcept { return midiFailed.load(); }
    juce::uint32 getMidiEventCount() const noexcept { return midiCount.load(); }
    int getLastMidiNote() const noexcept { return lastNote.load(); }

    // -- audio thread ---------------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);   ///< from audioDeviceAboutToStart
    void process (int numSamples);                        ///< renders one block
    void addToOutput (float* const* out, int numOut, int numSamples) const;
    void mixMono (float* dest, int numSamples) const;     ///< overwrite dest with mono sum
    float getLevel() const noexcept { return level.load(); }

private:
    class EditorWindow;

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    juce::String key (const char* name) const { return keyPrefix + name; }

    juce::String keyPrefix;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<EditorWindow> editorWindow;
    juce::File pluginFile;

    std::unique_ptr<juce::MidiInput> midiInput;
    juce::MidiMessageCollector midiCollector;
    juce::String midiIdentifier;

    juce::AudioBuffer<float> procBuffer;
    juce::MidiBuffer midiBuffer;
    double preparedRate  { 44100.0 };
    int    preparedBlock { 0 };
    int    lastRendered  { 0 };

    std::atomic<bool>  active { false };   ///< plugin ready for the audio thread
    std::atomic<float> level  { 0.0f };

    std::atomic<bool>         midiFailed { false };
    std::atomic<juce::uint32> midiCount  { 0 };
    std::atomic<int>          lastNote   { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentHost)
};

} // namespace bandjam
