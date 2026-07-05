#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "common/SongLoader.h"
#include "ui/Meters.h"
#include <vector>

namespace bandjam
{
/** The "Recordings" tab (host and musician): every recorded jam is a folder
    full of stems (backing track stems + one take per musician + the host
    input) plus a meta.json. The panel lists those folders, loads one into
    the owner's player, offers a volume/mute strip with a live meter per stem
    and exports the current mix as an MP3 (encoded by Windows Media
    Foundation, no external tools).

    The audio side is abstracted through Player callbacks so the host
    (HostMixEngine preview player) and the musician (PlayCaptureEngine local
    player) share the same panel. */
class RecordingsPanel : public juce::Component,
                        private juce::ListBoxModel,
                        private juce::Timer
{
public:
    /** Callbacks into the owner's audio engine. All fire on the message
        thread; every one of them must be set. */
    struct Player
    {
        std::function<bool()>   canLoad;               ///< engine idle enough to load/play?
        std::function<double()> getDeviceSampleRate;

        /** Adopts decoded stems under the given identity (id doubles as the
            loaded-song name so isCurrent() can detect replacement). */
        std::function<bool (const juce::String& id, const juce::String& title,
                            double sampleRate, juce::int64 lengthSamples,
                            std::vector<songloader::DecodedStem>&& stems,
                            juce::String& error)> adopt;
        std::function<bool (const juce::String& id)> isCurrent;
        std::function<void()> unload;

        std::function<void()> play, pause, stop;
        std::function<bool()> isPlaying;
        std::function<void (double seconds)> seek;
        std::function<double()> getPositionSeconds, getLengthSeconds;

        std::function<void (int index, float gainDb)> setStemGainDb;
        std::function<void (int index, bool mute)>    setStemMute;
        std::function<float (int index)>              getStemLevel;
    };

    struct Options
    {
        Player player;

        /** Host only: adds a "Send to..." button; the owner shows a musician
            picker anchored to the given component. */
        std::function<void (const juce::File& folder, const juce::String& song,
                            juce::Component& anchor)> onSendRecording;

        /** Musician only: adds the "receive recordings automatically" checkbox. */
        bool showAutoReceiveToggle { false };
        std::function<bool()>      getAutoReceive;
        std::function<void (bool)> onAutoReceiveChanged;
    };

    explicit RecordingsPanel (Options optionsToUse);
    ~RecordingsPanel() override;

    /** Rescans the recordings folder (after a jam was recorded / received). */
    void refreshList();

    /** Status line, e.g. for transfer progress ("Receiving... 12 / 80 MB"). */
    void showStatus (const juce::String& text, bool isError = false);

    /** Fired after a recording was loaded into / removed from the player,
        so the owning view can refresh its own transport controls. */
    std::function<void()> onPreviewChanged;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Entry
    {
        juce::File   dir;
        juce::String song, date;
        int          numTracks { 0 };
        double       lengthSeconds { 0.0 };
    };

    struct TrackInfo
    {
        juce::String fileName, name, kind;   ///< kind: "stem" | "musician" | "host"
    };

    // ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void timerCallback() override;

    void loadClicked();
    void deleteClicked();
    void exportClicked();
    void rebuildStrips();
    void layoutStrips();
    void updateButtons();
    void setStatus (const juce::String& text, juce::Colour colour);

    bool loadedIsActive() const;
    static std::vector<TrackInfo> parseTracks (const juce::var& meta);

    Options options;

    std::vector<Entry> entries;
    juce::File   loadedDir;
    juce::String loadedName;
    std::vector<TrackInfo> loadedTracks;
    std::vector<float> gainsDb;
    std::vector<char>  mutes;          // vector<bool> has no data(); char is fine
    int  loadGeneration { 0 };
    bool exporting { false };

    juce::Label      listCaption, loadedTitle, statusLabel, timeLabel, exportCaption;
    juce::ListBox    list { "recordings", this };
    juce::TextButton refreshButton, loadButton, deleteButton, openFolderButton, sendButton;
    juce::ToggleButton autoReceiveToggle;
    juce::TextButton playButton, stopButton, exportButton;
    juce::ComboBox   bitrateBox;
    juce::Slider     posSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Viewport   stripsViewport;
    juce::Component  stripsContainer;
    juce::OwnedArray<ChannelStrip> strips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordingsPanel)
};

} // namespace bandjam
