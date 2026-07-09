#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "musician/DownloadStore.h"
#include "musician/HostConnection.h"
#include "musician/PlayCaptureEngine.h"
#include "host/SongLibrary.h"
#include "common/AppPaths.h"
#include "common/SongLoader.h"
#include "common/VoiceChat.h"
#include "ui/Meters.h"
#include "ui/LambdaPage.h"
#include "ui/AudioStreamPanel.h"
#include "ui/ChatPanel.h"
#include "ui/RecordingsPanel.h"

namespace bandjam
{
/** Musician mode: connect to a host, receive songs automatically when the
    host prepares a jam ("Prepare Jam" -> auto download + load + soundcheck),
    mix the stems locally, preview them, and play along in a jam. Includes
    the audio settings (device/ASIO, latency info, manual offset). */
class MusicianView : public juce::Component,
                     public juce::ListBoxModel,   // songs list
                     private juce::Timer
{
public:
    MusicianView();
    ~MusicianView() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** Called when the user wants to go back to the mode picker. */
    std::function<void()> onLeave;

    // songs ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;

private:
    enum class JamPhase { idle, downloading, loading, ready, countdown, running };

    class LocalSongsModel;

    void timerCallback() override;

    void toggleConnect();
    void handleDisconnected (const juce::String& reason);

    void downloadClicked();
    void deleteClicked();
    void startNextDownload();
    void handleStemEnd (const juce::String& songId, const juce::String& stemId,
                        bool ok, const juce::String& error);

    void rebuildStemStrips();
    void layoutStemStrips();
    void loadSelectedSong();
    void loadLocalSong (const LibrarySong& song);
    void updateTransportButtons();
    void updateSongButtons();

    void localSelectionChanged();
    void addLocalSongClicked();
    void removeLocalSongClicked();
    void sendLocalSongClicked();
    const LibrarySong* selectedLocalSong() const;

    void handleJamPrepare (const juce::var& json);
    void continueJamPreparation();
    void beginJamRamLoad();
    void failJamPreparation (const juce::String& error);
    int  jamDownloadPercent (double currentStemFraction) const;
    void sendPrepareStatus (const juce::String& state, int percent, const juce::String& error);
    void handleJamGo();
    void handleJamStop (const juce::String& reason);

    void refreshLatencyLabels();
    void refreshInstrumentUi();
    void loadInstrumentClicked();
    void refreshMidiInputs();
    void layoutSessionPage();
    void layoutAudioPage();
    void layoutStreamPage();
    void refreshTalkMicDevices();
    void startVoice();
    void stopVoice();
    void updateTalkAutoMute();
    void handleRecordingOffer (const juce::var& offer);

    const LocalSong* selectedSong() const;
    static std::vector<songloader::StemRequest> requestsFor (const LocalSong& song);
    static juce::String formatTime (double seconds);

    // Destruction order: engine last (its sender thread uses connection).
    DownloadStore     downloads;
    SongLibrary       localSongs { paths::localSongsRoot() };   ///< "My songs": local, editable
    HostConnection    connection;
    PlayCaptureEngine engine;
    VoiceChat         voice;      ///< band talk (sender thread uses connection too)

    // Download queue (message thread)
    struct PendingStem { juce::String songId, stemId, stemName; };
    juce::Array<PendingStem> downloadQueue;
    bool downloadActive { false };

    // Jam state (message thread)
    JamPhase     jamPhase { JamPhase::idle };
    juce::String jamId, jamSongId, jamSongName;
    juce::uint32 countdownDeadlineMs { 0 };
    juce::uint32 lastStatusSentMs { 0 };
    int          loadGeneration { 0 };   ///< invalidates in-flight async decodes
    juce::var    lastSongListJson;

    // -- widgets -----------------------------------------------------------------
    juce::Label      headerLabel;
    juce::TextButton leaveButton;

    juce::Label      nameCaption, hostCaption, portCaption, roomCaption, connStatusLabel;
    juce::TextEditor nameEditor, hostEditor, portEditor, roomEditor;
    juce::TextButton connectButton;

    // Tabs: "Session" (jam workflow + chat), "Audio" (device/VST3/MIDI
    // settings), "Audio Stream" (routing board) and "Recordings".
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    LambdaPage sessionPage, audioPage, streamPage, recordingsPage;
    std::unique_ptr<AudioStreamPanel> streamPanel;
    std::unique_ptr<RecordingsPanel> recordingsPanel;
    ChatPanel chatPanel;

    juce::Label      songsCaption, downloadStatusLabel;
    juce::ListBox    songsList { "songs", this };
    juce::TextButton downloadButton, deleteButton;

    // "My songs": a second, locally editable list. Songs can be previewed
    // and offered to the host (who has to accept before the upload starts).
    juce::Label      localSongsCaption;
    std::unique_ptr<LocalSongsModel> localModel;
    juce::ListBox    localList { "localsongs", nullptr };
    juce::TextButton localAddButton, localRemoveButton, localSendButton;
    std::unique_ptr<juce::FileChooser> songChooser;
    bool syncingSelection { false };   ///< guards the two lists' mutual deselect
    bool songUploadActive { false };

    juce::Label      playerCaption, songTitleLabel, timeLabel;
    juce::TextButton loadButton, playButton, stopButton;
    juce::Slider     positionSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Viewport   stemsViewport;
    juce::Component  stemsContainer;
    juce::OwnedArray<ChannelStrip> stemStrips;
    juce::Array<juce::String>      stemStripIds;

    juce::Label      jamCaption, jamStatusLabel, countdownLabel;
    juce::Label      backingMeterCaption, micMeterCaption;
    LevelMeter       backingMeter, micMeter;

    juce::Label      audioCaption, latencyLabel, offsetCaption, asioHintLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::Slider     offsetSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // Second input: the talk-only mic (feeds the VRChat stream / voice chat,
    // never your instrument path).
    juce::Label       talkMicCaption, talkMicHintLabel;
    juce::ComboBox    talkMicBox;
    juce::StringArray talkMicDevices;

    // Auto-mute of the talk mic while a song plays / while a jam runs.
    juce::ToggleButton muteTalkOnPlayToggle, muteTalkOnJamToggle;
    bool muteTalkOnPlay { true }, muteTalkOnJam { true };
    bool talkAutoMuted { false };
    bool talkResumeVoice { false }, talkResumeStream { false };

    juce::Label      instCaption, instStatusLabel, midiCaption, midiActivityLabel;
    juce::TextButton instLoadButton, instUiButton, instRemoveButton, midiRescanButton;
    juce::ComboBox   midiInputBox;
    juce::Array<juce::MidiDeviceInfo> midiInputs;
    std::unique_ptr<juce::FileChooser> instChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicianView)
};

} // namespace bandjam
