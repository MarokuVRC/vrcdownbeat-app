#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "host/SongLibrary.h"
#include "host/HostMixEngine.h"
#include "host/JamServer.h"
#include "host/RelayLink.h"
#include "common/SongLoader.h"
#include "common/VoiceChat.h"
#include "ui/Meters.h"
#include "ui/LambdaPage.h"
#include "ui/AudioStreamPanel.h"
#include "ui/ChatPanel.h"
#include "ui/RecordingsPanel.h"
#include "ui/ChildWindow.h"
#include <map>

namespace bandjam
{
/** Host mode: song library, TCP server, jam control and the live mix.

    Jam state machine:
      idle -> loading   ("Prepare Jam": song is decoded asynchronously)
           -> preparing (jamPrepare broadcast; musicians auto-download + load,
                         then stream their input for the soundcheck; the host
                         sees each musician's download/ready status)
           -> countdown ("Start Jam", 3 s, broadcast)
           -> running   (jamGo sent; playback starts at the 3-s gate;
                         optional WAV recording of the final mix)
           -> idle      (jamStop) */
class HostView : public juce::Component,
                 public juce::ListBoxModel,     // songs list
                 public juce::MenuBarModel,     // "Connect" / "Settings" menus
                 public juce::FileDragAndDropTarget,   // drop audio files/folders on the song list
                 private juce::Timer
{
public:
    HostView();
    ~HostView() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** Called when the user wants to go back to the mode picker. */
    std::function<void()> onLeave;

    // songs ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;   // rename song + stems
    void showRenameSongDialog (const juce::String& songId);

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int menuIndex) override;

private:
    enum class Phase { idle, loading, preparing, countdown, running };

    struct PrepareState
    {
        juce::String state;    ///< "downloading" | "loading" | "ready" | "error"
        int          percent { 0 };
        juce::String error;
    };

    class MusiciansModel;

    void timerCallback() override;
    void showHostServerDialog();
    void showRoomServerDialog();
    void startOwnServer (const juce::String& name, int port);
    void startRoomServer (const juce::String& name);
    bool applyHostName (const juce::String& name);   ///< validates + stores; false = refused
    void stopHosting (bool showOff = true);
    void onHostingStarted();
    void updateConnectUi();
    void addSongClicked();
    void removeSongClicked();
    void updateStemInfo();
    void updateJamButtons();

    // Drag & drop import (audio files / stem folders onto the song list).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    bool dropIsOverSongList (int x, int y) const;
    void setSongListDragHighlight (bool on);
    void importDroppedSongs (const juce::StringArray& files);

    void prepareJamClicked();
    void startJamClicked();
    void stopJam (const juce::String& reason, bool broadcast);
    void handlePrepareStatus (const juce::String& name, const juce::String& jamId,
                              const juce::String& state, int percent, const juce::String& error);
    void handleClientsChanged();
    bool allParticipantsReady() const;
    void updatePrepareStatusText();
    void beginCountdown();
    void goLive();
    void broadcastJamState();
    void rebuildMixerStrips();
    void layoutMixerStrips();

    void appendLog (const juce::String& line);
    void showSendRecordingMenu (const juce::File& folder, const juce::String& song,
                                juce::Component& anchor);
    void handleSongOffer (const juce::String& clientName, const juce::var& offer);
    void handleSongReceived (const juce::String& clientName, const juce::String& songName,
                             const juce::File& folder, bool ok, const juce::String& error);
    void updateTalkAutoMute();
    void refreshIpLabel();
    void fetchPublicIp();
    void testPortClicked();
    void layoutSessionPage();
    void layoutAudioPage();
    void layoutStreamPage();
    void layoutPortTestPage();
    void initTalkMicControls();
    void refreshTalkMicDevices();
    void initInstrumentControls();
    void refreshInstrumentUi();
    void refreshMidiInputs();
    void loadInstrumentClicked();
    void initRecordingControls();
    void refreshRecordingFolderLabel();
    void updatePreviewAutoRecord();
    void startVoice();
    void stopVoice();
    void previewLoadClicked();
    void updatePreviewButtons();
    static juce::String pickLocalIp();
    static juce::String formatTime (double seconds);

    // Order matters for destruction: the server's reader threads use
    // library and engine, so it must be destroyed first (declared last).
    SongLibrary   library;
    HostMixEngine engine;
    VoiceChat     voice;      ///< band talk (its sender thread uses server)
    JamServer     server;
    RelayLink     relayLink { server };   ///< destroyed before server (declared after)

    // Jam state (message thread)
    Phase             phase { Phase::idle };
    juce::String      jamId, jamSongId, jamSongName;
    juce::StringArray participants;
    std::map<juce::String, PrepareState> prepareStates;
    juce::uint32      countdownDeadlineMs { 0 };
    int               stateBroadcastTick  { 0 };
    int               prepareGeneration   { 0 };   ///< invalidates in-flight async decodes
    int               previewGeneration   { 0 };   ///< invalidates in-flight preview decodes

    juce::String publicIp;        ///< empty while being fetched
    bool         portTestRunning { false };

    // -- widgets -----------------------------------------------------------------
    // Top bar: Connect/Settings menus (Windows style) + Change Role/Disconnect,
    // which stay reachable no matter which tool windows are open.
    juce::MenuBarComponent menuBar { this };
    juce::Label      headerLabel;
    juce::TextButton leaveButton;        ///< "Change Role" - back to the mode picker
    juce::TextButton disconnectButton;   ///< visible while hosting

    juce::Label      serverStatusLabel;
    juce::TextButton copyCodeButton;
    bool relayMode { false };   ///< true while hosting via room code
    bool ipsVisible { false };  ///< IPs stay masked unless the user reveals them

    // "Test Port" tool window content.
    juce::Label      portTestHintLabel, ipLabel, portTestResultLabel, portTestExplainLabel;
    juce::TextButton portTestButton, showIpsButton;

    // Main content is the session (jam workflow + chat). "Audio",
    // "Audio Stream" and "Recordings" open as separate tool windows.
    LambdaPage sessionPage, audioPage, streamPage, recordingsPage, portTestPage;
    std::unique_ptr<AudioStreamPanel> streamPanel;
    std::unique_ptr<RecordingsPanel> recordingsPanel;
    ChatPanel chatPanel;

    juce::Label      libraryCaption;
    juce::ListBox    songsList { "songs", this };
    juce::TextButton addSongButton, removeSongButton;
    juce::Label      stemInfoLabel;
    bool             songDragHighlight { false };

    juce::Label      jamCaption, jamStatusLabel, countdownLabel;
    juce::TextButton prepareJamButton, startJamButton, stopJamButton;
    juce::ToggleButton recordToggle;

    // Preview player: listen to a library song anytime (stops on Prepare Jam).
    juce::TextButton previewLoadButton, previewPlayButton, previewStopButton;
    juce::Slider     previewSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label      previewTimeLabel;

    juce::Label      mixerCaption;
    juce::Viewport   mixerViewport;
    juce::Component  mixerContainer;
    juce::OwnedArray<ChannelStrip> stemStrips;
    juce::OwnedArray<ChannelStrip> performerStrips;
    juce::StringArray              performerStripNames;
    LevelMeter       masterMeter;
    juce::Label      masterMeterLabel;

    juce::Label      audioCaption;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;

    // The host's own live input, shown as a permanent strip in the mix list.
    ChannelStrip     hostInputStrip { "My Instrument" };

    // Master over all backing-track stems (shown when a song has 2+ stems).
    ChannelStrip     stemMasterStrip { "All stems" };

    // Second input: the talk-only mic (feeds the VRChat stream / voice chat,
    // never the band mix).
    juce::Label       talkMicCaption, talkMicHintLabel;
    juce::ComboBox    talkMicBox;
    juce::StringArray talkMicDevices;

    // Auto-mute of the talk mic while a song plays / while a jam runs.
    juce::ToggleButton muteTalkOnPlayToggle, muteTalkOnJamToggle;
    bool muteTalkOnPlay { true }, muteTalkOnJam { true };
    bool talkAutoMuted { false };            ///< the auto-mute is currently engaged
    bool talkResumeVoice { false }, talkResumeStream { false };

    // VST3 instrument (host side): rendered into the host input path.
    juce::Label      instCaption, instStatusLabel, midiCaption, midiActivityLabel;
    juce::TextButton instLoadButton, instUiButton, instRemoveButton, midiRescanButton;
    juce::ComboBox   midiInputBox;
    juce::Array<juce::MidiDeviceInfo> midiInputs;
    juce::ToggleButton loadVstOnStartToggle;
    std::unique_ptr<juce::FileChooser> instChooser;

    // Recording settings: auto-record toggles + folder + file-name pattern.
    juce::Label        recSettingsCaption, recFolderLabel, recPatternCaption, recPatternHintLabel;
    juce::ToggleButton autoRecordPlayToggle, autoRecordJamToggle;
    juce::TextButton   recFolderButton;
    juce::TextEditor   recPatternEditor;
    bool autoRecordOnPlay { false }, autoRecordOnJam { false };
    bool previewRecFailed { false };   ///< suppresses auto-record retries this playback
    std::unique_ptr<juce::FileChooser> recFolderChooser;

    juce::Label      musiciansCaption;
    std::unique_ptr<MusiciansModel> musiciansModel;
    juce::ListBox    musiciansList;

    juce::Label      logCaption;
    juce::TextEditor logView;

    std::unique_ptr<juce::FileChooser> fileChooser;

    // Tool windows (declared last so they are destroyed before their content).
    std::unique_ptr<ChildWindow> audioWindow, streamWindow, recordingsWindow, portTestWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostView)
};

} // namespace bandjam
