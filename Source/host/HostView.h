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
    void toggleServer();
    void toggleRoom();
    bool applyHostName();   ///< validates + stores the name; false = refused
    void stopHosting (bool showOff = true);
    void onHostingStarted();
    void addSongClicked();
    void removeSongClicked();
    void updateStemInfo();
    void updateJamButtons();

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
    void initTalkMicControls();
    void refreshTalkMicDevices();
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
    juce::Label      headerLabel;
    juce::TextButton leaveButton;

    juce::Label      nameCaption, portCaption, ipLabel, serverStatusLabel, portTestResultLabel;
    juce::TextEditor nameEditor, portEditor;
    juce::TextButton serverButton, roomButton, copyCodeButton, portTestButton, showIpsButton;
    bool relayMode { false };   ///< true while hosting via room code
    bool ipsVisible { false };  ///< IPs stay masked unless the user reveals them

    // Tabs: "Session" (jam workflow + chat), "Audio" (device settings),
    // "Audio Stream" (routing board) and "Recordings" (stem remix + MP3).
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    LambdaPage sessionPage, audioPage, streamPage, recordingsPage;
    std::unique_ptr<AudioStreamPanel> streamPanel;
    std::unique_ptr<RecordingsPanel> recordingsPanel;
    ChatPanel chatPanel;

    juce::Label      libraryCaption;
    juce::ListBox    songsList { "songs", this };
    juce::TextButton addSongButton, removeSongButton;
    juce::Label      stemInfoLabel;

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

    juce::Label      musiciansCaption;
    std::unique_ptr<MusiciansModel> musiciansModel;
    juce::ListBox    musiciansList;

    juce::Label      logCaption;
    juce::TextEditor logView;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostView)
};

} // namespace bandjam
