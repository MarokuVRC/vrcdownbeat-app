#include "HostView.h"
#include "ui/Style.h"
#include "common/Settings.h"

namespace bandjam
{
//==============================================================================
/** Connected musicians incl. download/jam status. */
class HostView::MusiciansModel : public juce::ListBoxModel
{
public:
    explicit MusiciansModel (HostView& ownerToUse) : owner (ownerToUse) {}

    int getNumRows() override { return names.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
    {
        if (! juce::isPositiveAndBelow (row, names.size()))
            return;

        const auto& name = names.getReference (row);

        g.setColour (style::textPrimary());
        g.setFont (style::normalFont());
        g.drawText (name, 8, 0, width / 2 - 10, height, juce::Justification::centredLeft);

        juce::String status = "connected";
        juce::Colour colour = style::textDim();

        const bool isParticipant = owner.participants.contains (name);

        if ((owner.phase == Phase::loading || owner.phase == Phase::preparing) && isParticipant)
        {
            const auto it = owner.prepareStates.find (name);
            if (it == owner.prepareStates.end())
            {
                status = "waiting...";
                colour = style::warn();
            }
            else if (it->second.state == "downloading")
            {
                status = "downloading " + juce::String (it->second.percent) + " %";
                colour = style::warn();
            }
            else if (it->second.state == "loading")
            {
                status = "loading...";
                colour = style::warn();
            }
            else if (it->second.state == "ready")
            {
                status = "ready";
                colour = style::good();
            }
            else if (it->second.state == "error")
            {
                status = "error!";
                colour = style::bad();
            }
        }
        else if ((owner.phase == Phase::running || owner.phase == Phase::countdown) && isParticipant)
        {
            status = "buffer " + juce::String (owner.engine.getBufferedSeconds (name), 1) + " s";
            colour = style::accent();
        }

        g.setColour (colour);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (status, width / 2, 0, width / 2 - 8, height, juce::Justification::centredRight);
    }

    juce::StringArray names;

private:
    HostView& owner;
};

//==============================================================================
HostView::HostView()
{
    // -- top bar: menus + Change Role / Disconnect ---------------------------------
    addAndMakeVisible (menuBar);

    headerLabel.setText ("Host", juce::dontSendNotification);
    headerLabel.setFont (style::titleFont());
    addAndMakeVisible (headerLabel);

    leaveButton.setButtonText ("Change Role");
    leaveButton.onClick = [this]
    {
        stopJam ("Host closed", true);
        voice.stop();
        server.stop();
        if (onLeave) onLeave();
    };
    addAndMakeVisible (leaveButton);

    disconnectButton.setButtonText ("Disconnect");
    disconnectButton.setColour (juce::TextButton::buttonColourId, style::bad().withAlpha (0.55f));
    disconnectButton.onClick = [this] { stopHosting(); };
    addChildComponent (disconnectButton);   // only visible while hosting

    // -- status strip ----------------------------------------------------------------
    serverStatusLabel.setText ("Server is off - use the Connect menu to start hosting", juce::dontSendNotification);
    serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (serverStatusLabel);

    copyCodeButton.setButtonText ("Copy code");
    copyCodeButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (relayLink.getRoomCode());
    };
    addChildComponent (copyCodeButton);   // only visible while a room is open

    // -- "Test Port" window content ---------------------------------------------------
    portTestHintLabel.setText ("Checks from the internet whether musicians can reach your server "
                               "directly (only needed for \"Host Server\" - the Downbeat server "
                               "works without an open port).\n"
                               "Start your server first, then run the test.",
                               juce::dontSendNotification);
    portTestHintLabel.setFont (juce::Font (juce::FontOptions (13.5f)));
    portTestHintLabel.setColour (juce::Label::textColourId, style::textDim());
    portTestHintLabel.setJustificationType (juce::Justification::topLeft);
    portTestPage.addAndMakeVisible (portTestHintLabel);

    ipLabel.setColour (juce::Label::textColourId, style::textDim());
    ipLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    portTestPage.addAndMakeVisible (ipLabel);

    // IPs stay hidden until deliberately revealed (e.g. while screen-sharing).
    ipsVisible = (bool) settings::get ("showIps", false);
    showIpsButton.setButtonText (ipsVisible ? "Hide IPs" : "Show IPs");
    showIpsButton.setTooltip ("Shows or hides your local and public IP address. "
                              "Keep them hidden while streaming or sharing your screen.");
    showIpsButton.onClick = [this]
    {
        ipsVisible = ! ipsVisible;
        settings::set ("showIps", ipsVisible);
        showIpsButton.setButtonText (ipsVisible ? "Hide IPs" : "Show IPs");
        refreshIpLabel();
    };
    portTestPage.addAndMakeVisible (showIpsButton);

    portTestButton.setButtonText ("Run test");
    portTestButton.setEnabled (false);
    portTestButton.onClick = [this] { testPortClicked(); };
    portTestPage.addAndMakeVisible (portTestButton);

    portTestResultLabel.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
    portTestResultLabel.setColour (juce::Label::textColourId, style::textDim());
    portTestPage.addAndMakeVisible (portTestResultLabel);

    portTestExplainLabel.setFont (juce::Font (juce::FontOptions (13.5f)));
    portTestExplainLabel.setColour (juce::Label::textColourId, style::textDim());
    portTestExplainLabel.setJustificationType (juce::Justification::topLeft);
    portTestPage.addAndMakeVisible (portTestExplainLabel);

    portTestPage.onLayout = [this] { layoutPortTestPage(); };

    refreshIpLabel();
    fetchPublicIp();

    // -- pages ----------------------------------------------------------------
    // "Session" holds the jam workflow; "VRChat Stream" the virtual-mic and
    // per-app routing tools (kept separate so the main view stays uncluttered).
    sessionPage.onLayout = [this] { layoutSessionPage(); };

    // -- library -----------------------------------------------------------------
    style::styleSectionLabel (libraryCaption, "Library");
    sessionPage.addAndMakeVisible (libraryCaption);

    songsList.setRowHeight (26);
    sessionPage.addAndMakeVisible (songsList);

    addSongButton.setButtonText ("Add song...");
    addSongButton.onClick = [this] { addSongClicked(); };
    sessionPage.addAndMakeVisible (addSongButton);

    removeSongButton.setButtonText ("Remove");
    removeSongButton.onClick = [this] { removeSongClicked(); };
    sessionPage.addAndMakeVisible (removeSongButton);

    stemInfoLabel.setJustificationType (juce::Justification::topLeft);
    stemInfoLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    stemInfoLabel.setColour (juce::Label::textColourId, style::textDim());
    sessionPage.addAndMakeVisible (stemInfoLabel);

    // -- jam ------------------------------------------------------------------------
    style::styleSectionLabel (jamCaption, "Jam");
    sessionPage.addAndMakeVisible (jamCaption);

    prepareJamButton.setButtonText ("Prepare Jam");
    prepareJamButton.onClick = [this] { prepareJamClicked(); };
    sessionPage.addAndMakeVisible (prepareJamButton);

    startJamButton.setButtonText ("Start Jam");
    startJamButton.onClick = [this] { startJamClicked(); };
    sessionPage.addAndMakeVisible (startJamButton);

    stopJamButton.setButtonText ("Stop jam");
    stopJamButton.onClick = [this] { stopJam ("Stopped by host", true); };
    sessionPage.addAndMakeVisible (stopJamButton);

    recordToggle.setButtonText ("Record");
    sessionPage.addAndMakeVisible (recordToggle);

    jamStatusLabel.setText ("Start a server (Connect menu), pick a song, let musicians connect - then \"Prepare Jam\".",
                            juce::dontSendNotification);
    jamStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    sessionPage.addAndMakeVisible (jamStatusLabel);

    countdownLabel.setFont (juce::Font (juce::FontOptions (64.0f, juce::Font::bold)));
    countdownLabel.setColour (juce::Label::textColourId, style::accent());
    countdownLabel.setJustificationType (juce::Justification::centred);
    // Overlays the mixer: must never eat its mouse events, and
    // addChildComponent (NOT addAndMakeVisible) keeps it hidden until the
    // countdown actually runs.
    countdownLabel.setInterceptsMouseClicks (false, false);
    sessionPage.addChildComponent (countdownLabel);

    // -- preview player (listen to a library song anytime) ------------------------
    previewLoadButton.setButtonText ("Load");
    previewLoadButton.onClick = [this] { previewLoadClicked(); };
    sessionPage.addAndMakeVisible (previewLoadButton);

    previewPlayButton.setButtonText ("Play");
    previewPlayButton.setEnabled (false);
    previewPlayButton.onClick = [this]
    {
        if (engine.isPreviewPlaying()) engine.previewPause();
        else                           engine.previewPlay();
        updatePreviewButtons();
    };
    sessionPage.addAndMakeVisible (previewPlayButton);

    previewStopButton.setButtonText ("Stop");
    previewStopButton.setEnabled (false);
    previewStopButton.onClick = [this] { engine.previewStop(); updatePreviewButtons(); };
    sessionPage.addAndMakeVisible (previewStopButton);

    previewSlider.setRange (0.0, 1.0, 0.01);
    previewSlider.setEnabled (false);
    previewSlider.onDragEnd = [this] { engine.previewSeekSeconds (previewSlider.getValue()); };
    sessionPage.addAndMakeVisible (previewSlider);

    previewTimeLabel.setText ("0:00 / 0:00", juce::dontSendNotification);
    previewTimeLabel.setColour (juce::Label::textColourId, style::textDim());
    previewTimeLabel.setJustificationType (juce::Justification::centredRight);
    sessionPage.addAndMakeVisible (previewTimeLabel);

    // -- mixer -----------------------------------------------------------------------
    style::styleSectionLabel (mixerCaption, "Mix");
    sessionPage.addAndMakeVisible (mixerCaption);

    mixerViewport.setViewedComponent (&mixerContainer, false);
    mixerViewport.setScrollBarsShown (true, false);
    sessionPage.addAndMakeVisible (mixerViewport);

    masterMeterLabel.setText ("Output:", juce::dontSendNotification);
    masterMeterLabel.setColour (juce::Label::textColourId, style::textDim());
    masterMeterLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    sessionPage.addAndMakeVisible (masterMeterLabel);
    sessionPage.addAndMakeVisible (masterMeter);

    // -- host audio device (Audio tab) + own input (Session) -----------------------
    style::styleSectionLabel (audioCaption, "Host audio (your interface)");
    audioPage.addAndMakeVisible (audioCaption);

    engine.initialiseDevice();

    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(), 0, 2, 2, 2, false, false, false, false);
    audioPage.addAndMakeVisible (*deviceSelector);

    // The host's own instrument/talk mic, mixed live into output + stream +
    // recording. Starts muted (avoids mic->speaker feedback on first run);
    // the meter still shows the incoming level while muted.
    {
        const float savedGain = (float) (double) settings::get ("hostInputGainDb", 0.0);
        const bool  savedMute = (bool) settings::get ("hostInputMute", true);
        hostInputStrip.setValues (savedGain, savedMute);
        engine.setHostInputGainDb (savedGain);
        engine.setHostInputMute (savedMute);
    }
    hostInputStrip.onGain = [this] (float db)
    {
        engine.setHostInputGainDb (db);
        settings::set ("hostInputGainDb", db);
    };
    hostInputStrip.onMute = [this] (bool mute)
    {
        engine.setHostInputMute (mute);
        settings::set ("hostInputMute", mute);
    };
    mixerContainer.addAndMakeVisible (hostInputStrip);   // permanent first row of the mix

    // -- Audio tab: talk mic (second input, separate from the interface input) -------
    initTalkMicControls();

    // -- Audio settings: VST3 instrument + recording options --------------------------
    initInstrumentControls();
    initRecordingControls();

    // -- Audio Stream tab: routing board -----------------------------------------------
    {
        // Persisted include flags go straight into the engine before the
        // panel reads them back for the toggles.
        engine.setStreamIncludeMix ((bool) settings::get ("hostStreamIncludeMix", true));
        engine.setStreamIncludeInput ((bool) settings::get ("hostStreamIncludeInput", true));
        engine.getStreamOutput().setTalkEnabled ((bool) settings::get ("hostStreamTalkEnabled", true));

        AudioStreamPanel::Options streamOptions;
        streamOptions.outDeviceSettingsKey    = "hostStreamOutDevice";
        streamOptions.talkDeviceSettingsKey   = "hostStreamTalkDevice";
        streamOptions.capturedAppsSettingsKey = "hostStreamCapturedApps";

        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - band mix";
            source.subtitle      = "jam mix / local song playback";
            source.getLevel      = [this] { return engine.getOutputLevel(); };
            source.getToMic      = [this] { return engine.getStreamIncludeMix(); };
            source.setToMic      = [this] (bool on) { engine.setStreamIncludeMix (on);
                                                      settings::set ("hostStreamIncludeMix", on); };
            source.youFixedValue = true;
            source.youFixedHint  = "Always plays on your audio device";
            streamOptions.sources.push_back (std::move (source));
        }
        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - my instrument";
            source.subtitle      = "your instrument/mic on the interface";
            source.makeSubtitle  = [this]
            {
                return engine.hasInstrument() ? "VST3: " + engine.getInstrumentName()
                                              : juce::String ("your instrument/mic on the interface");
            };
            source.getLevel      = [this] { return engine.getHostInputLevel(); };
            source.getToMic      = [this] { return engine.getStreamIncludeInput(); };
            source.setToMic      = [this] (bool on) { engine.setStreamIncludeInput (on);
                                                      settings::set ("hostStreamIncludeInput", on); };
            source.getToYou      = [this] { return ! (bool) settings::get ("hostInputMute", true); };
            source.setToYou      = [this] (bool on)
            {
                engine.setHostInputMute (! on);
                settings::set ("hostInputMute", ! on);
                hostInputStrip.setValues ((float) (double) settings::get ("hostInputGainDb", 0.0), ! on);
            };
            streamOptions.sources.push_back (std::move (source));
        }
        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - talk mic";
            source.subtitle      = "set the device in Settings > Audio";
            source.makeSubtitle  = []
            {
                auto device = settings::get ("hostStreamTalkDevice", "").toString();
                return device.isNotEmpty() ? device : juce::String ("no talk mic - set one in Settings > Audio");
            };
            source.getLevel      = [this] { return engine.getStreamOutput().getTalkLevel(); };
            source.getToMic      = [this] { return engine.getStreamOutput().isTalkEnabled(); };
            source.setToMic      = [this] (bool on) { engine.getStreamOutput().setTalkEnabled (on);
                                                      settings::set ("hostStreamTalkEnabled", on); };
            source.youControlsWindowsListen = true;   // hear yourself via Windows "Listen"
            streamOptions.sources.push_back (std::move (source));
        }

        streamPanel = std::make_unique<AudioStreamPanel> (engine.getStreamOutput(), streamOptions);
        streamPage.addAndMakeVisible (*streamPanel);
    }

    // -- "Recordings" tab: remix recorded jam stems + MP3 export ---------------------
    {
        RecordingsPanel::Options recOptions;
        auto& player = recOptions.player;

        player.canLoad = [this] { return engine.getState() == HostMixEngine::State::idle; };
        player.getDeviceSampleRate = [this]
        {
            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
                return device->getCurrentSampleRate();
            return 0.0;
        };
        player.adopt = [this] (const juce::String& id, const juce::String&, double, juce::int64,
                               std::vector<songloader::DecodedStem>&& stems, juce::String& error)
        {
            return engine.adoptPreview (id, std::move (stems), error);
        };
        player.isCurrent = [this] (const juce::String& id)
        {
            return engine.isPreviewLoaded() && engine.getPreviewName() == id;
        };
        player.unload    = [this] { engine.unloadPreview(); };
        player.play      = [this] { engine.previewPlay(); };
        player.pause     = [this] { engine.previewPause(); };
        player.stop      = [this] { engine.previewStop(); };
        player.isPlaying = [this] { return engine.isPreviewPlaying(); };
        player.seek      = [this] (double seconds) { engine.previewSeekSeconds (seconds); };
        player.getPositionSeconds = [this] { return engine.getPreviewPositionSeconds(); };
        player.getLengthSeconds   = [this] { return engine.getPreviewLengthSeconds(); };
        player.setStemGainDb = [this] (int i, float db) { engine.setPreviewStemGainDb (i, db); };
        player.setStemMute   = [this] (int i, bool m)   { engine.setPreviewStemMute (i, m); };
        player.getStemLevel  = [this] (int i)           { return engine.getPreviewStemLevel (i); };

        recOptions.onSendRecording = [this] (const juce::File& folder, const juce::String& song,
                                             juce::Component& anchor)
        {
            showSendRecordingMenu (folder, song, anchor);
        };

        recordingsPanel = std::make_unique<RecordingsPanel> (std::move (recOptions));
    }
    recordingsPanel->onPreviewChanged = [this]
    {
        // A recording (un)loaded the engine's preview player - keep the
        // Session tab's transport and mixer in sync with it.
        previewSlider.setRange (0.0, juce::jmax (0.1, engine.getPreviewLengthSeconds()), 0.01);
        rebuildMixerStrips();
        updatePreviewButtons();
    };
    recordingsPage.addAndMakeVisible (*recordingsPanel);

    audioPage.onLayout      = [this] { layoutAudioPage(); };
    streamPage.onLayout     = [this] { layoutStreamPage(); };
    recordingsPage.onLayout = [this] { recordingsPanel->setBounds (recordingsPage.getLocalBounds()); };

    // -- musicians / chat / log ---------------------------------------------------------
    style::styleSectionLabel (musiciansCaption, "Musicians");
    sessionPage.addAndMakeVisible (musiciansCaption);

    musiciansModel = std::make_unique<MusiciansModel> (*this);
    musiciansList.setModel (musiciansModel.get());
    musiciansList.setRowHeight (26);
    sessionPage.addAndMakeVisible (musiciansList);

    sessionPage.addAndMakeVisible (chatPanel);
    chatPanel.setStatusText ("Start a server (Connect menu) to chat with the band.");
    chatPanel.setTalkAvailable (false);
    chatPanel.getTalkLevel = [this] { return voice.getTalkLevel(); };
    chatPanel.onSendText = [this] (const juce::String& text)
    {
        if (! server.isRunning())
        {
            chatPanel.addSystemMessage ("Server is off - message not sent");
            return;
        }
        server.broadcastChat (server.getHostName(), text);
        chatPanel.addMessage (server.getHostName(), text);
    };
    chatPanel.onTalkToggled = [this] (bool talk)
    {
        if (talk && talkAutoMuted)
        {
            chatPanel.setTalkActive (false);
            chatPanel.addSystemMessage ("Talk mic is auto-muted right now - turn the toggle off in the Audio settings to talk");
            return;
        }
        if (talk && ! voice.isRunning())
            startVoice();
        voice.setTalking (talk && voice.isRunning() && voice.getTalkMicName().isNotEmpty());
        if (talk && voice.getTalkMicName().isEmpty())
        {
            chatPanel.setTalkActive (false);
            chatPanel.addSystemMessage ("Select a talk mic in Settings > Audio first");
        }
    };

    style::styleSectionLabel (logCaption, "Log");
    sessionPage.addAndMakeVisible (logCaption);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setCaretVisible (false);
    logView.setFont (juce::Font (juce::FontOptions (12.5f)));
    sessionPage.addAndMakeVisible (logView);

    // The session is the main content; Audio, Audio Stream, Recordings and the
    // port test live in their own tool windows (Connect/Settings menus) so they
    // can stay open next to the main window.
    addAndMakeVisible (sessionPage);

    audioWindow      = std::make_unique<ChildWindow> ("Audio - VRC Downbeat",        audioPage,      980, 700);
    streamWindow     = std::make_unique<ChildWindow> ("Audio Stream - VRC Downbeat", streamPage,     1100, 760);
    recordingsWindow = std::make_unique<ChildWindow> ("Recordings - VRC Downbeat",   recordingsPage, 1100, 720);
    portTestWindow   = std::make_unique<ChildWindow> ("Test Port - VRC Downbeat",    portTestPage,   620, 420);

    // -- wiring ---------------------------------------------------------------------
    juce::Component::SafePointer<HostView> safe (this);

    library.onChanged = [safe]
    {
        if (safe == nullptr) return;
        safe->songsList.updateContent();
        safe->songsList.repaint();
        safe->updateStemInfo();
        safe->server.broadcastSongList();
    };

    server.getSongListJson = [this] { return library.toJson(); }; // reader threads; library outlives server
    server.getStemFile     = [this] (const juce::String& songId, const juce::String& stemId)
    {
        return library.getStemFile (songId, stemId);
    };
    server.onAudioBlock    = [this] (const juce::String& name, juce::int64 start,
                                     const juce::int16* samples, int n)
    {
        engine.pushJamAudio (name, start, samples, n); // engine outlives server
    };
    server.onMonitorBlock  = [this] (const juce::String& name, juce::int64 start,
                                     const juce::int16* samples, int n)
    {
        engine.pushMonitorAudio (name, start, samples, n);
    };
    server.onVoiceBlock    = [this] (const juce::String& name, const juce::int16* samples, int n)
    {
        voice.receiveVoice (name, samples, n);   // reader thread - VoiceChat locks internally
    };
    server.onChat = [safe] (const juce::String& name, const juce::String& text)
    {
        if (safe != nullptr) safe->chatPanel.addMessage (name, text);
    };
    voice.onVoiceBlock = [this] (juce::int64 startSample, const float* mono, int numSamples)
    {
        server.broadcastVoice (server.getHostName(), startSample, mono, numSamples);
    };

    server.onLog = [safe] (const juce::String& line)
    {
        if (safe != nullptr) safe->appendLog (line);
    };
    server.onClientsChanged = [safe]
    {
        if (safe != nullptr) safe->handleClientsChanged();
    };
    server.onPrepareStatus = [safe] (const juce::String& name, const juce::String& id,
                                     const juce::String& state, int percent, const juce::String& error)
    {
        if (safe != nullptr) safe->handlePrepareStatus (name, id, state, percent, error);
    };
    server.onSongOffer = [safe] (const juce::String& clientName, juce::var offer)
    {
        if (safe != nullptr) safe->handleSongOffer (clientName, offer);
    };
    server.onSongReceived = [safe] (const juce::String& clientName, const juce::String& songName,
                                    juce::File folder, bool ok, const juce::String& error)
    {
        if (safe != nullptr) safe->handleSongReceived (clientName, songName, folder, ok, error);
    };

    relayLink.onRoomOpened = [safe] (const juce::String& code)
    {
        if (safe == nullptr)
            return;
        safe->serverStatusLabel.setText ("Room code: " + code + "   -   give it to your musicians",
                                         juce::dontSendNotification);
        safe->serverStatusLabel.setColour (juce::Label::textColourId, style::good());
        safe->copyCodeButton.setVisible (true);
        safe->appendLog ("Room opened on the relay - code: " + code);
        safe->onHostingStarted();
        safe->updateJamButtons();
    };
    relayLink.onClosed = [safe] (const juce::String& reason)
    {
        if (safe == nullptr || ! safe->relayMode)
            return;
        safe->stopHosting (false);
        safe->serverStatusLabel.setText (reason, juce::dontSendNotification);
        safe->serverStatusLabel.setColour (juce::Label::textColourId, style::bad());
        safe->appendLog ("Relay: " + reason);
    };

    engine.onPreviewRecordingSaved = [safe] (juce::File folder)
    {
        if (safe != nullptr)
        {
            safe->appendLog ("Song recording saved: " + folder.getFullPathName());
            if (safe->recordingsPanel != nullptr)
                safe->recordingsPanel->refreshList();
        }
    };

    engine.onRecordingSaved = [safe] (juce::File folder)
    {
        if (safe != nullptr)
        {
            safe->appendLog ("Recording saved (all stems): " + folder.getFullPathName());
            safe->jamStatusLabel.setText ("Recording saved - open Settings > Recordings to remix and export it.",
                                          juce::dontSendNotification);
            if (safe->recordingsPanel != nullptr)
                safe->recordingsPanel->refreshList();

            // Musicians who enabled "receive recordings automatically" get
            // the stems offered right away (their client accepts silently).
            const int offered = safe->server.offerRecordingToAutoReceivers (folder);
            if (offered > 0)
                safe->appendLog ("Recording offered to " + juce::String (offered)
                                 + (offered == 1 ? " musician" : " musicians") + " (auto-receive).");
        }
    };

    updateJamButtons();
    startTimerHz (30);   // fast enough for live-feeling meters
}

HostView::~HostView()
{
    stopTimer();
    ++prepareGeneration;   // orphan any in-flight decode
    ++previewGeneration;
    engine.onPreviewRecordingSaved = nullptr;   // widgets die before the engine
    engine.stopPreviewRecording();
    engine.saveInstrumentState();
    stopJam ("Host closed", true);
    voice.stop();          // before server.stop(): its sender uses the server
    server.stop();
    musiciansList.setModel (nullptr);
}

//==============================================================================
void HostView::paint (juce::Graphics& g)
{
    g.fillAll (style::background());

    auto area = getLocalBounds().reduced (16, 10);
    area.removeFromTop (30 + 6);   // menu row
    style::drawPanel (g, area.removeFromTop (40).toFloat());
}

void HostView::resized()
{
    auto area = getLocalBounds().reduced (16, 10);

    // Menu row: menus on the left, Change Role/Disconnect pinned top-right so
    // they stay reachable no matter what is going on.
    auto menuRow = area.removeFromTop (30);
    leaveButton.setBounds (menuRow.removeFromRight (110).reduced (0, 2));
    menuRow.removeFromRight (8);
    disconnectButton.setBounds (menuRow.removeFromRight (110).reduced (0, 2));
    menuRow.removeFromRight (12);
    menuBar.setBounds (menuRow.removeFromLeft (juce::jmin (260, menuRow.getWidth())));
    area.removeFromTop (6);

    // Status strip: role + server status + room code copy.
    auto status = area.removeFromTop (40).reduced (12, 4);
    headerLabel.setBounds (status.removeFromLeft (80));
    status.removeFromLeft (8);
    copyCodeButton.setBounds (status.removeFromRight (92).reduced (0, 3));
    status.removeFromRight (8);
    serverStatusLabel.setBounds (status);
    area.removeFromTop (8);

    sessionPage.setBounds (area);
}

void HostView::layoutPortTestPage()
{
    auto area = portTestPage.getLocalBounds().reduced (14, 12);

    portTestHintLabel.setBounds (area.removeFromTop (64));
    area.removeFromTop (8);

    auto ipRow = area.removeFromTop (30);
    showIpsButton.setBounds (ipRow.removeFromLeft (90).reduced (0, 2));
    ipRow.removeFromLeft (12);
    ipLabel.setBounds (ipRow);
    area.removeFromTop (10);

    auto testRow = area.removeFromTop (30);
    portTestButton.setBounds (testRow.removeFromLeft (110).reduced (0, 1));
    testRow.removeFromLeft (12);
    portTestResultLabel.setBounds (testRow);
    area.removeFromTop (10);

    portTestExplainLabel.setBounds (area);
}

void HostView::layoutSessionPage()
{
    auto area = sessionPage.getLocalBounds().reduced (0, 8);

    // Columns
    auto left  = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * 0.28f));
    area.removeFromLeft (12);
    auto right = area.removeFromRight (juce::roundToInt ((float) area.getWidth() * 0.34f));
    area.removeFromRight (12);
    auto mid = area;

    // Left: library
    libraryCaption.setBounds (left.removeFromTop (24));
    left.removeFromTop (4);
    auto libButtons = left.removeFromBottom (30);
    addSongButton.setBounds (libButtons.removeFromLeft (juce::jmin (170, libButtons.getWidth() * 2 / 3)));
    libButtons.removeFromLeft (6);
    removeSongButton.setBounds (libButtons);
    left.removeFromBottom (6);
    stemInfoLabel.setBounds (left.removeFromBottom (96));
    left.removeFromBottom (4);
    songsList.setBounds (left);

    // Mid: jam + mixer
    jamCaption.setBounds (mid.removeFromTop (24));
    mid.removeFromTop (4);
    auto jamButtons = mid.removeFromTop (32);
    prepareJamButton.setBounds (jamButtons.removeFromLeft (120).reduced (0, 1));
    jamButtons.removeFromLeft (6);
    startJamButton.setBounds (jamButtons.removeFromLeft (110).reduced (0, 1));
    jamButtons.removeFromLeft (6);
    stopJamButton.setBounds (jamButtons.removeFromLeft (110).reduced (0, 1));
    jamButtons.removeFromLeft (10);
    recordToggle.setBounds (jamButtons.removeFromLeft (130));
    mid.removeFromTop (6);
    jamStatusLabel.setBounds (mid.removeFromTop (24));
    mid.removeFromTop (6);

    // Preview player row (load & listen to the selected song anytime).
    auto previewRow = mid.removeFromTop (28);
    previewLoadButton.setBounds (previewRow.removeFromLeft (110));
    previewRow.removeFromLeft (6);
    previewPlayButton.setBounds (previewRow.removeFromLeft (80));
    previewRow.removeFromLeft (6);
    previewStopButton.setBounds (previewRow.removeFromLeft (80));
    previewRow.removeFromLeft (10);
    previewTimeLabel.setBounds (previewRow.removeFromRight (100));
    previewSlider.setBounds (previewRow);
    mid.removeFromTop (8);

    // Master meter pinned above the bottom edge (height matches content).
    auto meterRow = mid.removeFromBottom (24);
    masterMeterLabel.setBounds (meterRow.removeFromLeft (70));
    masterMeter.setBounds (meterRow.reduced (0, 3));
    mid.removeFromBottom (6);

    mixerCaption.setBounds (mid.removeFromTop (22));
    mid.removeFromTop (4);
    mixerViewport.setBounds (mid);
    countdownLabel.setBounds (mid);
    layoutMixerStrips();

    // Right: musicians + chat + log
    musiciansCaption.setBounds (right.removeFromTop (24));
    right.removeFromTop (4);
    musiciansList.setBounds (right.removeFromTop (juce::jmin (140, right.getHeight() / 4)));
    right.removeFromTop (8);

    auto logArea = right.removeFromBottom (juce::jmin (150, right.getHeight() / 3));
    logCaption.setBounds (logArea.removeFromTop (22));
    logArea.removeFromTop (4);
    logView.setBounds (logArea);

    right.removeFromBottom (8);
    chatPanel.setBounds (right);
}

void HostView::layoutAudioPage()
{
    auto area = audioPage.getLocalBounds().reduced (0, 8);

    // Left: the host's audio device settings. Right: the talk mic.
    auto left = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * 0.55f));
    area.removeFromLeft (16);
    auto right = area;

    audioCaption.setBounds (left.removeFromTop (24));
    left.removeFromTop (4);
    deviceSelector->setBounds (left);

    right.removeFromTop (28);   // align below the caption
    auto talkRow = right.removeFromTop (26);
    talkMicCaption.setBounds (talkRow.removeFromLeft (80));
    talkMicBox.setBounds (talkRow);
    right.removeFromTop (4);
    talkMicHintLabel.setBounds (right.removeFromTop (34));
    right.removeFromTop (8);
    muteTalkOnPlayToggle.setBounds (right.removeFromTop (24));
    right.removeFromTop (2);
    muteTalkOnJamToggle.setBounds (right.removeFromTop (24));

    // Instrument (VST3)
    right.removeFromTop (16);
    instCaption.setBounds (right.removeFromTop (24));
    right.removeFromTop (4);
    auto instRow = right.removeFromTop (28);
    instLoadButton.setBounds (instRow.removeFromLeft (110));
    instRow.removeFromLeft (6);
    instUiButton.setBounds (instRow.removeFromLeft (90));
    instRow.removeFromLeft (6);
    instRemoveButton.setBounds (instRow.removeFromLeft (90));
    right.removeFromTop (6);
    loadVstOnStartToggle.setBounds (right.removeFromTop (24));
    right.removeFromTop (6);
    auto midiRow = right.removeFromTop (26);
    midiCaption.setBounds (midiRow.removeFromLeft (70));
    midiRescanButton.setBounds (midiRow.removeFromRight (80));
    midiRow.removeFromRight (6);
    midiInputBox.setBounds (midiRow);
    right.removeFromTop (4);
    instStatusLabel.setBounds (right.removeFromTop (20));
    midiActivityLabel.setBounds (right.removeFromTop (20));

    // Recording
    right.removeFromTop (16);
    recSettingsCaption.setBounds (right.removeFromTop (24));
    right.removeFromTop (4);
    autoRecordPlayToggle.setBounds (right.removeFromTop (24));
    right.removeFromTop (2);
    autoRecordJamToggle.setBounds (right.removeFromTop (24));
    right.removeFromTop (6);
    auto folderRow = right.removeFromTop (28);
    recFolderButton.setBounds (folderRow.removeFromLeft (110));
    folderRow.removeFromLeft (8);
    recFolderLabel.setBounds (folderRow);
    right.removeFromTop (6);
    auto patternRow = right.removeFromTop (26);
    recPatternCaption.setBounds (patternRow.removeFromLeft (80));
    recPatternEditor.setBounds (patternRow.reduced (0, 1));
    right.removeFromTop (2);
    recPatternHintLabel.setBounds (right.removeFromTop (20));
}

void HostView::layoutStreamPage()
{
    streamPanel->setBounds (streamPage.getLocalBounds());
}

void HostView::initTalkMicControls()
{
    talkMicCaption.setText ("Talk mic:", juce::dontSendNotification);
    talkMicCaption.setColour (juce::Label::textColourId, style::textDim());
    audioPage.addAndMakeVisible (talkMicCaption);

    talkMicBox.onChange = [this]
    {
        const int index = talkMicBox.getSelectedItemIndex();
        const auto device = juce::isPositiveAndBelow (index - 1, talkMicDevices.size())
                                ? talkMicDevices[index - 1] : juce::String();
        settings::set ("hostStreamTalkDevice", device);

        if (streamPanel != nullptr)
            streamPanel->talkDeviceChanged();

        if (voice.isRunning())   // pick up the new mic without dropping the voice chat
        {
            const bool wasTalking = voice.isTalking();
            startVoice();
            voice.setTalking (wasTalking && voice.getTalkMicName().isNotEmpty());
        }
    };
    audioPage.addAndMakeVisible (talkMicBox);

    talkMicHintLabel.setText ("Used for voice chat and the VRChat mic - your interface "
                              "input above stays separate.", juce::dontSendNotification);
    talkMicHintLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    talkMicHintLabel.setColour (juce::Label::textColourId, style::textDim());
    talkMicHintLabel.setJustificationType (juce::Justification::topLeft);
    audioPage.addAndMakeVisible (talkMicHintLabel);

    // Auto-mute: keeps the talk mic out of the room while music plays.
    muteTalkOnPlay = (bool) settings::get ("muteTalkOnPreview", true);
    muteTalkOnJam  = (bool) settings::get ("muteTalkOnJam", true);

    muteTalkOnPlayToggle.setButtonText ("Auto-mute talk mic while a song plays");
    muteTalkOnPlayToggle.setToggleState (muteTalkOnPlay, juce::dontSendNotification);
    muteTalkOnPlayToggle.onClick = [this]
    {
        muteTalkOnPlay = muteTalkOnPlayToggle.getToggleState();
        settings::set ("muteTalkOnPreview", muteTalkOnPlay);
        updateTalkAutoMute();
    };
    audioPage.addAndMakeVisible (muteTalkOnPlayToggle);

    muteTalkOnJamToggle.setButtonText ("Auto-mute talk mic during a jam");
    muteTalkOnJamToggle.setToggleState (muteTalkOnJam, juce::dontSendNotification);
    muteTalkOnJamToggle.onClick = [this]
    {
        muteTalkOnJam = muteTalkOnJamToggle.getToggleState();
        settings::set ("muteTalkOnJam", muteTalkOnJam);
        updateTalkAutoMute();
    };
    audioPage.addAndMakeVisible (muteTalkOnJamToggle);

    refreshTalkMicDevices();
}

void HostView::refreshTalkMicDevices()
{
    talkMicDevices = StreamOutput::getInputDeviceNames();

    talkMicBox.clear (juce::dontSendNotification);
    talkMicBox.addItem ("(none)", 1);
    for (int i = 0; i < talkMicDevices.size(); ++i)
        talkMicBox.addItem (talkMicDevices[i], i + 2);

    const auto saved = settings::get ("hostStreamTalkDevice", "").toString();
    const int index = talkMicDevices.indexOf (saved);
    talkMicBox.setSelectedItemIndex (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
}

//==============================================================================
void HostView::initInstrumentControls()
{
    style::styleSectionLabel (instCaption, "Instrument (VST3)");
    audioPage.addAndMakeVisible (instCaption);

    instLoadButton.setButtonText ("Load VST3...");
    instLoadButton.onClick = [this] { loadInstrumentClicked(); };
    audioPage.addAndMakeVisible (instLoadButton);

    instUiButton.setButtonText ("Open UI");
    instUiButton.onClick = [this] { engine.showInstrumentEditor(); };
    audioPage.addAndMakeVisible (instUiButton);

    instRemoveButton.setButtonText ("Remove");
    instRemoveButton.onClick = [this]
    {
        engine.unloadInstrument();
        refreshInstrumentUi();
    };
    audioPage.addAndMakeVisible (instRemoveButton);

    loadVstOnStartToggle.setButtonText ("Load VST on startup");
    loadVstOnStartToggle.setToggleState ((bool) settings::get ("hostLoadVstOnStartup", false),
                                         juce::dontSendNotification);
    loadVstOnStartToggle.onClick = [this]
    {
        settings::set ("hostLoadVstOnStartup", loadVstOnStartToggle.getToggleState());
    };
    audioPage.addAndMakeVisible (loadVstOnStartToggle);

    midiCaption.setText ("MIDI in:", juce::dontSendNotification);
    midiCaption.setColour (juce::Label::textColourId, style::textDim());
    audioPage.addAndMakeVisible (midiCaption);

    midiInputBox.onChange = [this]
    {
        const int index = midiInputBox.getSelectedItemIndex();
        engine.setMidiInput (juce::isPositiveAndBelow (index - 1, midiInputs.size())
                                 ? midiInputs.getReference (index - 1).identifier
                                 : juce::String());
    };
    audioPage.addAndMakeVisible (midiInputBox);

    midiRescanButton.setButtonText ("Rescan");
    midiRescanButton.onClick = [this] { refreshMidiInputs(); };
    audioPage.addAndMakeVisible (midiRescanButton);

    instStatusLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    instStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    audioPage.addAndMakeVisible (instStatusLabel);

    midiActivityLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    midiActivityLabel.setColour (juce::Label::textColourId, style::textDim());
    midiActivityLabel.setJustificationType (juce::Justification::centredRight);
    audioPage.addAndMakeVisible (midiActivityLabel);

    // Only restore the saved plugin when the user opted in.
    if ((bool) settings::get ("hostLoadVstOnStartup", false))
        if (const auto saved = engine.getSavedInstrumentFile(); saved.exists())
        {
            juce::String error;
            engine.loadInstrument (saved, error);
        }

    refreshMidiInputs();
    refreshInstrumentUi();
}

void HostView::refreshInstrumentUi()
{
    const bool loaded = engine.hasInstrument();

    instStatusLabel.setText (loaded
                                 ? "Loaded: " + engine.getInstrumentName()
                                   + " (mixed into your output, stream and recordings)"
                                 : "No plugin - only the hardware input is used.",
                             juce::dontSendNotification);
    instStatusLabel.setColour (juce::Label::textColourId, loaded ? style::good() : style::textDim());
    instUiButton.setEnabled (loaded);
    instRemoveButton.setEnabled (loaded);
}

void HostView::refreshMidiInputs()
{
    midiInputs = HostMixEngine::getMidiInputs();

    midiInputBox.clear (juce::dontSendNotification);
    midiInputBox.addItem ("(none)", 1);
    for (int i = 0; i < midiInputs.size(); ++i)
        midiInputBox.addItem (midiInputs.getReference (i).name, i + 2);

    const auto saved = engine.getMidiInputIdentifier().isNotEmpty()
                           ? engine.getMidiInputIdentifier()
                           : settings::get ("host_midiInput", "").toString();

    int selected = 0;
    for (int i = 0; i < midiInputs.size(); ++i)
        if (midiInputs.getReference (i).identifier == saved)
            { selected = i + 1; break; }

    midiInputBox.setSelectedItemIndex (selected, juce::dontSendNotification);

    // Always (re)open the port: a rescan is also the retry after another
    // program released it.
    engine.setMidiInput (selected > 0 ? saved : juce::String());
}

void HostView::loadInstrumentClicked()
{
    const auto defaultFolder = juce::File ("C:\\Program Files\\Common Files\\VST3");

    instChooser = std::make_unique<juce::FileChooser> (
        "Choose a VST3 instrument (e.g. Superior Drummer 3)",
        defaultFolder.isDirectory() ? defaultFolder
                                    : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.vst3");

    juce::Component::SafePointer<HostView> safe (this);
    instChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& chooser)
    {
        if (safe == nullptr)
            return;

        const auto file = chooser.getResult();
        if (file == juce::File())
            return;

        safe->instStatusLabel.setText ("Loading plugin...", juce::dontSendNotification);

        juce::String error;
        if (! safe->engine.loadInstrument (file, error))
        {
            safe->refreshInstrumentUi();
            safe->instStatusLabel.setText (error, juce::dontSendNotification);
            safe->instStatusLabel.setColour (juce::Label::textColourId, style::warn());
            return;
        }

        safe->refreshMidiInputs();
        safe->refreshInstrumentUi();
        safe->engine.showInstrumentEditor();
    });
}

//==============================================================================
void HostView::initRecordingControls()
{
    style::styleSectionLabel (recSettingsCaption, "Recording");
    audioPage.addAndMakeVisible (recSettingsCaption);

    autoRecordOnPlay = (bool) settings::get ("autoRecordOnPreview", false);
    autoRecordOnJam  = (bool) settings::get ("autoRecordOnJam", false);

    autoRecordPlayToggle.setButtonText ("Auto-record while a song plays");
    autoRecordPlayToggle.setToggleState (autoRecordOnPlay, juce::dontSendNotification);
    autoRecordPlayToggle.onClick = [this]
    {
        autoRecordOnPlay = autoRecordPlayToggle.getToggleState();
        settings::set ("autoRecordOnPreview", autoRecordOnPlay);
        updatePreviewAutoRecord();
    };
    audioPage.addAndMakeVisible (autoRecordPlayToggle);

    autoRecordJamToggle.setButtonText ("Auto-record during a jam");
    autoRecordJamToggle.setToggleState (autoRecordOnJam, juce::dontSendNotification);
    autoRecordJamToggle.onClick = [this]
    {
        autoRecordOnJam = autoRecordJamToggle.getToggleState();
        settings::set ("autoRecordOnJam", autoRecordOnJam);
    };
    audioPage.addAndMakeVisible (autoRecordJamToggle);

    recFolderButton.setButtonText ("Save to...");
    recFolderButton.setTooltip ("Choose the folder where recordings are saved "
                                "(the Recordings window shows this folder).");
    recFolderButton.onClick = [this]
    {
        recFolderChooser = std::make_unique<juce::FileChooser> (
            "Choose the recordings folder", settings::recordingsFolder());

        juce::Component::SafePointer<HostView> safe (this);
        recFolderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                           | juce::FileBrowserComponent::canSelectDirectories,
                                       [safe] (const juce::FileChooser& chooser)
        {
            if (safe == nullptr)
                return;

            const auto folder = chooser.getResult();
            if (folder == juce::File() || ! folder.isDirectory())
                return;

            settings::set ("recordingsFolder", folder.getFullPathName());
            safe->refreshRecordingFolderLabel();
            if (safe->recordingsPanel != nullptr)
                safe->recordingsPanel->refreshList();
        });
    };
    audioPage.addAndMakeVisible (recFolderButton);

    recFolderLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    recFolderLabel.setColour (juce::Label::textColourId, style::textDim());
    recFolderLabel.setMinimumHorizontalScale (0.7f);
    audioPage.addAndMakeVisible (recFolderLabel);
    refreshRecordingFolderLabel();

    recPatternCaption.setText ("File name:", juce::dontSendNotification);
    recPatternCaption.setColour (juce::Label::textColourId, style::textDim());
    audioPage.addAndMakeVisible (recPatternCaption);

    recPatternEditor.setText (settings::get ("recordingNamePattern", "{song}_{date}_{time}").toString(),
                              juce::dontSendNotification);
    recPatternEditor.onTextChange = [this]
    {
        settings::set ("recordingNamePattern", recPatternEditor.getText());
    };
    audioPage.addAndMakeVisible (recPatternEditor);

    recPatternHintLabel.setText ("Placeholders: {song}  {date}  {time}", juce::dontSendNotification);
    recPatternHintLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    recPatternHintLabel.setColour (juce::Label::textColourId, style::textDim());
    audioPage.addAndMakeVisible (recPatternHintLabel);
}

void HostView::refreshRecordingFolderLabel()
{
    recFolderLabel.setText (settings::recordingsFolder().getFullPathName(), juce::dontSendNotification);
}

void HostView::updatePreviewAutoRecord()
{
    // Start when the preview starts playing, stop when it pauses/stops or
    // the toggle goes off (fires from the UI timer).
    const bool playing = phase == Phase::idle && engine.isPreviewPlaying();

    if (autoRecordOnPlay && playing && ! engine.isPreviewRecording())
    {
        if (previewRecFailed)
            return;   // don't retry (and spam the log) until playback restarts

        juce::String error;
        if (engine.startPreviewRecording (error))
        {
            appendLog ("Auto-record: recording \"" + engine.getPreviewName() + "\"");
        }
        else
        {
            previewRecFailed = true;
            appendLog ("Auto-record failed: " + error);
        }
    }
    else if (engine.isPreviewRecording() && (! playing || ! autoRecordOnPlay))
    {
        engine.stopPreviewRecording();
    }

    if (! playing)
        previewRecFailed = false;
}

//==============================================================================
// songs list
int HostView::getNumRows() { return library.getSongs().size(); }

void HostView::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto& songs = library.getSongs();
    if (! juce::isPositiveAndBelow (row, songs.size()))
        return;

    if (selected)
    {
        g.setColour (style::accentDark().withAlpha (0.5f));
        g.fillRect (0, 0, width, height);
    }

    const auto& song = songs.getReference (row);
    g.setColour (style::textPrimary());
    g.setFont (style::normalFont());
    g.drawText (song.name, 8, 0, width - 90, height, juce::Justification::centredLeft);

    g.setColour (style::textDim());
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (juce::String (song.stems.size()) + " stems", width - 86, 0, 80, height,
                juce::Justification::centredRight);
}

void HostView::selectedRowsChanged (int)
{
    updateStemInfo();
    updateJamButtons();
}

void HostView::updateStemInfo()
{
    const int row = songsList.getSelectedRow();
    const auto& songs = library.getSongs();

    if (! juce::isPositiveAndBelow (row, songs.size()))
    {
        stemInfoLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    const auto& song = songs.getReference (row);
    juce::StringArray lines;
    lines.add (juce::String (song.sampleRate / 1000.0, 1) + " kHz, "
               + juce::String ((double) song.lengthSamples / song.sampleRate / 60.0, 1) + " min");
    for (const auto& stem : song.stems)
        lines.add ("- " + stem.name);
    stemInfoLabel.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
}

//==============================================================================
// Connect / Settings menus
juce::StringArray HostView::getMenuBarNames()
{
    return { "Connect", "Settings" };
}

juce::PopupMenu HostView::getMenuForIndex (int menuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (menuIndex == 0)   // Connect
    {
        const bool idle = ! server.isRunning();
        menu.addItem (1, "Host Server...", idle);
        menu.addItem (2, "Host Downbeat Server...", idle);
        menu.addSeparator();
        menu.addItem (3, "Test Port...");
    }
    else if (menuIndex == 1)   // Settings
    {
        menu.addItem (11, "Audio");
        menu.addItem (12, "Audio Stream");
        menu.addItem (13, "Recordings");
    }
    return menu;
}

void HostView::menuItemSelected (int menuItemID, int)
{
    switch (menuItemID)
    {
        case 1:  showHostServerDialog();      break;
        case 2:  showRoomServerDialog();      break;
        case 3:  portTestWindow->open();      break;
        case 11: audioWindow->open();         break;
        case 12: streamWindow->open();        break;
        case 13: recordingsWindow->open();    break;
        default: break;
    }
}

void HostView::showHostServerDialog()
{
    if (server.isRunning())
        return;

    auto* window = new juce::AlertWindow ("Host Server",
                                          "Runs the server on your own PC. Musicians connect directly "
                                          "to your IP and port (port forwarding may be needed - "
                                          "use Connect > Test Port to check).",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", settings::get ("hostName", "").toString(), "Your name:");
    window->addTextEditor ("port", settings::get ("hostPort", kDefaultPort).toString(), "TCP port:");
    window->getTextEditor ("name")->setInputRestrictions (24);
    window->getTextEditor ("port")->setInputRestrictions (5, "0123456789");
    window->addButton ("Start", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HostView> safe (this);
    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([safe, window] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            const auto name = window->getTextEditorContents ("name").trim();
            const int  port = juce::jlimit (1, 65535, window->getTextEditorContents ("port").getIntValue());
            safe->startOwnServer (name, port);
        }),
        true);
}

void HostView::showRoomServerDialog()
{
    if (server.isRunning())
        return;

    auto* window = new juce::AlertWindow ("Host Downbeat Server",
                                          "Opens a room on the Downbeat relay server. Musicians join "
                                          "with the room code - no port forwarding, no IP sharing "
                                          "on either side.",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", settings::get ("hostName", "").toString(), "Your name:");
    window->getTextEditor ("name")->setInputRestrictions (24);
    window->addButton ("Open room", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HostView> safe (this);
    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([safe, window] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            safe->startRoomServer (window->getTextEditorContents ("name").trim());
        }),
        true);
}

void HostView::startOwnServer (const juce::String& name, int port)
{
    if (server.isRunning() || ! applyHostName (name))
        return;

    juce::String error;
    if (server.start (port, error))
    {
        settings::set ("hostPort", port);
        serverStatusLabel.setText ("Running on port " + juce::String (port), juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::good());
        onHostingStarted();
    }
    else
    {
        serverStatusLabel.setText (error, juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::bad());
    }
    updateConnectUi();
    updateJamButtons();
}

void HostView::startRoomServer (const juce::String& name)
{
    if (server.isRunning() || ! applyHostName (name))
        return;

    relayMode = true;
    server.startVirtual();
    serverStatusLabel.setText ("Opening a room on the relay server...", juce::dontSendNotification);
    serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());

    juce::String relayHost;
    int relayPort = relay::kDefaultPort;
    relay::parseAddress (settings::get ("relayAddress", juce::String (relay::kDefaultAddress)).toString(),
                         relayHost, relayPort);
    relayLink.open (relayHost, relayPort, server.getHostName());
    updateConnectUi();
    updateJamButtons();
}

bool HostView::applyHostName (const juce::String& name)
{
    if (name.isEmpty())
    {
        serverStatusLabel.setText ("Please enter a name first.", juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::warn());
        return false;
    }

    settings::set ("hostName", name);
    server.setHostName (name);
    return true;
}

void HostView::stopHosting (bool showOff)
{
    stopJam ("Server stopped", true);
    stopVoice();
    relayLink.close();
    server.stop();
    relayMode = false;

    if (showOff)
    {
        serverStatusLabel.setText ("Server is off - use the Connect menu to start hosting",
                                   juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    }

    chatPanel.setTalkAvailable (false);
    chatPanel.setStatusText ("Start a server (Connect menu) to chat with the band.");
    updateConnectUi();
    updateJamButtons();
}

void HostView::onHostingStarted()
{
    chatPanel.setTalkAvailable (true);
    chatPanel.setStatusText ("Talk uses the talk mic from the Audio settings.");
    startVoice();   // hear the band's voice chat right away
    updateConnectUi();
}

void HostView::updateConnectUi()
{
    const bool running = server.isRunning();
    disconnectButton.setVisible (running);
    copyCodeButton.setVisible (running && relayMode && relayLink.getRoomCode().isNotEmpty());
    portTestButton.setEnabled (running && ! relayMode && ! portTestRunning);
}

void HostView::startVoice()
{
    const auto talkMic = settings::get ("hostStreamTalkDevice", "").toString();
    if (voice.isRunning() && voice.getTalkMicName() == talkMic)
        return;

    juce::String error;
    if (! voice.start (talkMic, error) && talkMic.isNotEmpty())
        voice.start ({}, error);   // mic busy/gone - at least keep listening
}

void HostView::stopVoice()
{
    voice.setTalking (false);
    voice.stop();
    chatPanel.setTalkActive (false);
}

//==============================================================================
juce::String HostView::pickLocalIp()
{
    juce::IPAddress fallback;
    for (const auto& addr : juce::IPAddress::getAllAddresses())
    {
        if (addr.isIPv6 || addr == juce::IPAddress::local())
            continue;

        const auto text = addr.toString();
        if (text.startsWith ("192.168.") || text.startsWith ("10."))
            return text;                      // typical LAN address - best pick
        if (fallback == juce::IPAddress())
            fallback = addr;
    }
    return fallback == juce::IPAddress() ? juce::String ("unknown") : fallback.toString();
}

void HostView::refreshIpLabel()
{
    if (! ipsVisible)
    {
        ipLabel.setText ("IPs hidden", juce::dontSendNotification);
        return;
    }

    ipLabel.setText ("Local IP: " + pickLocalIp()
                     + "    |    Public IP: "
                     + (publicIp.isEmpty() ? "detecting..." : publicIp),
                     juce::dontSendNotification);
}

void HostView::fetchPublicIp()
{
    juce::Component::SafePointer<HostView> safe (this);
    juce::Thread::launch ([safe]
    {
        auto text = juce::URL ("https://api.ipify.org").readEntireTextStream().trim();
        if (text.length() > 45 || text.containsAnyOf (" <>\n\t"))
            text.clear();   // got an error page, not an address

        juce::MessageManager::callAsync ([safe, text]
        {
            if (safe == nullptr) return;
            safe->publicIp = text.isNotEmpty() ? text : "not detectable";
            safe->refreshIpLabel();
        });
    });
}

namespace
{
    /** Blocking; returns "open", "closed", "timeout" or "offline" (service
        unreachable). Uses check-host.net, which probes the port from the
        outside - exactly what the router/port-forwarding check needs. */
    juce::String runExternalPortTest (const juce::String& ip, int port)
    {
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (5000)
                                 .withExtraHeaders ("Accept: application/json\r\nUser-Agent: BandJam\r\n");

        juce::URL requestUrl ("https://check-host.net/check-tcp?host=" + ip + ":" + juce::String (port)
                              + "&max_nodes=1");
        auto stream = requestUrl.createInputStream (options);
        if (stream == nullptr)
            return "offline";

        const auto response  = juce::JSON::parse (stream->readEntireStreamAsString());
        const auto requestId = response.getProperty ("request_id", juce::String()).toString();
        if (requestId.isEmpty())
            return "offline";

        for (int attempt = 0; attempt < 12; ++attempt)
        {
            juce::Thread::sleep (1000);

            juce::URL resultUrl ("https://check-host.net/check-result/" + requestId);
            auto resultStream = resultUrl.createInputStream (options);
            if (resultStream == nullptr)
                continue;

            const auto parsed = juce::JSON::parse (resultStream->readEntireStreamAsString());
            auto* object = parsed.getDynamicObject();
            if (object == nullptr)
                continue;

            for (const auto& property : object->getProperties())
            {
                auto* results = property.value.getArray();
                if (results == nullptr || results->isEmpty())
                    continue;   // still pending

                const auto& entry = results->getReference (0);
                if (entry.hasProperty ("error"))
                    return "closed";
                if (entry.hasProperty ("time") || entry.hasProperty ("address"))
                    return "open";
            }
        }
        return "timeout";
    }
}

void HostView::testPortClicked()
{
    if (portTestRunning)
        return;

    if (! server.isRunning() || relayMode)
    {
        portTestResultLabel.setText ("Server is not running.", juce::dontSendNotification);
        portTestResultLabel.setColour (juce::Label::textColourId, style::warn());
        portTestExplainLabel.setText ("Nothing is listening on your port right now, so a test would "
                                      "always fail. Start \"Host Server\" from the Connect menu first, "
                                      "then run the test.",
                                      juce::dontSendNotification);
        return;
    }

    if (publicIp.isEmpty() || publicIp == "not detectable")
    {
        portTestResultLabel.setText ("Public IP unknown - can't run the test.",
                                     juce::dontSendNotification);
        portTestResultLabel.setColour (juce::Label::textColourId, style::warn());
        portTestExplainLabel.setText ("Your public IP address could not be detected yet. Check that "
                                      "your internet connection works, wait a moment and try again.",
                                      juce::dontSendNotification);
        fetchPublicIp();
        return;
    }

    portTestRunning = true;
    portTestButton.setEnabled (false);
    portTestResultLabel.setText ("Testing... (up to 15 s)", juce::dontSendNotification);
    portTestResultLabel.setColour (juce::Label::textColourId, style::textDim());
    portTestExplainLabel.setText ("An external service (check-host.net) is trying to open a TCP "
                                  "connection to your public IP on port "
                                  + juce::String (server.getPort()) + " - exactly what a musician's "
                                  "app would do.",
                                  juce::dontSendNotification);

    juce::Component::SafePointer<HostView> safe (this);
    juce::Thread::launch ([safe, ip = publicIp, port = server.getPort()]
    {
        const auto verdict = runExternalPortTest (ip, port);

        juce::MessageManager::callAsync ([safe, verdict, port]
        {
            if (safe == nullptr) return;

            safe->portTestRunning = false;
            safe->updateConnectUi();

            juce::String text, why;
            juce::Colour colour;
            if (verdict == "open")
            {
                text   = "Port open - musicians can connect!";
                colour = style::good();
                why    = "The test service reached your server from the internet. Your router "
                         "forwards TCP port " + juce::String (port) + " correctly and no firewall "
                         "is blocking it. Give musicians your public IP and this port.";
            }
            else if (verdict == "closed")
            {
                text   = "Port closed - connection was refused.";
                colour = style::bad();
                why    = "The test reached your network, but nothing answered on port "
                         + juce::String (port) + ". Most likely the router has no port forwarding "
                         "rule for this port, or it points to the wrong PC.\n\n"
                         "How to fix:\n"
                         "1. In your router, forward TCP port " + juce::String (port) + " to this PC's "
                         "local IP (see above, \"Show IPs\").\n"
                         "2. Allow VRC Downbeat in the Windows Firewall.\n"
                         "3. Alternatively skip all of this and use \"Host Downbeat Server\" - "
                         "it needs no open port.";
            }
            else if (verdict == "timeout")
            {
                text   = "No answer - the connection attempt timed out.";
                colour = style::bad();
                why    = "The connection attempt was silently dropped. Typical causes:\n"
                         "- No port forwarding rule, and the router drops unknown traffic.\n"
                         "- Your ISP uses CGNAT / DS-Lite (shared public IP): incoming connections "
                         "are impossible, no router setting can fix that.\n\n"
                         "If port forwarding is set up correctly and it still times out, you are "
                         "probably behind CGNAT - use \"Host Downbeat Server\" instead, it works "
                         "without any open port.";
            }
            else
            {
                text   = "Test service not reachable.";
                colour = style::warn();
                why    = "The external test service (check-host.net) could not be contacted. This "
                         "says nothing about your port - your own internet connection may be down, "
                         "or the service is temporarily offline. Try again in a minute.";
            }
            safe->portTestResultLabel.setText (text, juce::dontSendNotification);
            safe->portTestResultLabel.setColour (juce::Label::textColourId, colour);
            safe->portTestExplainLabel.setText (why, juce::dontSendNotification);
        });
    });
}

//==============================================================================
void HostView::addSongClicked()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Choose the audio files (stems) for the song",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.mp3;*.flac;*.aiff;*.aif;*.ogg");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
        [this] (const juce::FileChooser& chooser)
        {
            juce::Array<juce::File> files = chooser.getResults();
            if (files.isEmpty())
                return;

            const auto defaultName = files[0].getParentDirectory().getFileName();

            auto* window = new juce::AlertWindow ("Add song", "Song name:",
                                                  juce::MessageBoxIconType::NoIcon);
            window->addTextEditor ("name", defaultName);
            window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<HostView> safe (this);
            window->enterModalState (true,
                juce::ModalCallbackFunction::create ([safe, window, files] (int result)
                {
                    if (safe == nullptr || result != 1)
                        return;

                    const auto name = window->getTextEditorContents ("name");
                    juce::String error;
                    if (safe->library.addSong (name, files, error).isEmpty())
                        safe->appendLog ("Import failed: " + error);
                    else
                        safe->appendLog ("Song imported: " + name);
                }),
                true);
        });
}

void HostView::removeSongClicked()
{
    const int row = songsList.getSelectedRow();
    const auto& songs = library.getSongs();
    if (juce::isPositiveAndBelow (row, songs.size()))
    {
        appendLog ("Song removed: " + songs.getReference (row).name);
        library.removeSong (songs.getReference (row).id);
    }
}

//==============================================================================
void HostView::updateJamButtons()
{
    const bool songSelected = juce::isPositiveAndBelow (songsList.getSelectedRow(), library.getSongs().size());
    prepareJamButton.setEnabled (phase == Phase::idle && server.isRunning() && songSelected);
    startJamButton.setEnabled (phase == Phase::preparing && allParticipantsReady());
    stopJamButton.setEnabled (phase != Phase::idle);
    recordToggle.setEnabled (phase == Phase::idle || phase == Phase::preparing);
    removeSongButton.setEnabled (phase == Phase::idle && songSelected);

    previewLoadButton.setEnabled (phase == Phase::idle && songSelected);
    updatePreviewButtons();
}

void HostView::updatePreviewButtons()
{
    const bool usable = phase == Phase::idle && engine.isPreviewLoaded();
    previewPlayButton.setEnabled (usable);
    previewStopButton.setEnabled (usable);
    previewSlider.setEnabled (usable);
    previewPlayButton.setButtonText (engine.isPreviewPlaying() ? "Pause" : "Play");
}

void HostView::previewLoadClicked()
{
    if (phase != Phase::idle)
        return;

    const int row = songsList.getSelectedRow();
    const auto& songs = library.getSongs();
    if (! juce::isPositiveAndBelow (row, songs.size()))
        return;

    const auto song = songs.getReference (row);   // copy: async completion outlives the list

    double targetRate = 0.0;
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        targetRate = device->getCurrentSampleRate();
    if (targetRate <= 0.0)
    {
        jamStatusLabel.setText ("No audio device active - check Settings > Audio.", juce::dontSendNotification);
        return;
    }

    jamStatusLabel.setText ("Loading \"" + song.name + "\" for preview...", juce::dontSendNotification);
    previewLoadButton.setEnabled (false);

    std::vector<songloader::StemRequest> requests;
    for (const auto& stem : song.stems)
        requests.push_back ({ song.folder.getChildFile (stem.fileName), stem.name, 0.0f, false });

    const int generation = ++previewGeneration;
    juce::Component::SafePointer<HostView> safe (this);

    songloader::decodeAsync (std::move (requests), targetRate,
                             [safe, generation, song] (std::shared_ptr<songloader::Result> result)
    {
        if (safe == nullptr || generation != safe->previewGeneration || safe->phase != Phase::idle)
            return;

        juce::String error = result->ok ? juce::String() : result->error;
        if (error.isEmpty())
            safe->engine.adoptPreview (song.name, std::move (result->stems), error);

        if (error.isEmpty())
        {
            safe->previewSlider.setRange (0.0, juce::jmax (0.1, safe->engine.getPreviewLengthSeconds()), 0.01);
            safe->rebuildMixerStrips();   // per-stem volume/mute for the preview
            safe->jamStatusLabel.setText ("Loaded \"" + song.name + "\" - press Play.",
                                          juce::dontSendNotification);
        }
        else
        {
            safe->jamStatusLabel.setText ("Preview failed: " + error, juce::dontSendNotification);
        }
        safe->updateJamButtons();
    });
}

juce::String HostView::formatTime (double seconds)
{
    if (seconds < 0.0 || std::isnan (seconds))
        seconds = 0.0;
    const int total = (int) seconds;
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

void HostView::prepareJamClicked()
{
    if (phase != Phase::idle)
        return;

    const int row = songsList.getSelectedRow();
    const auto& songs = library.getSongs();
    if (! juce::isPositiveAndBelow (row, songs.size()))
        return;

    const auto song = songs.getReference (row);   // copy: async completion outlives the list

    participants = server.getClientNames();
    if (participants.isEmpty())
    {
        jamStatusLabel.setText ("No musician is connected.", juce::dontSendNotification);
        return;
    }

    // The jam takes over the audio device - stop and drop any preview song.
    ++previewGeneration;   // orphan an in-flight preview decode
    engine.unloadPreview();

    phase = Phase::loading;
    prepareStates.clear();
    jamStatusLabel.setText ("Loading \"" + song.name + "\" ...", juce::dontSendNotification);
    updateJamButtons();

    std::vector<songloader::StemRequest> requests;
    for (const auto& stem : song.stems)
        requests.push_back ({ song.folder.getChildFile (stem.fileName), stem.name, 0.0f, false });

    const int generation = ++prepareGeneration;
    juce::Component::SafePointer<HostView> safe (this);

    songloader::decodeAsync (std::move (requests), song.sampleRate,
                             [safe, generation, song] (std::shared_ptr<songloader::Result> result)
    {
        if (safe == nullptr || generation != safe->prepareGeneration
            || safe->phase != Phase::loading)
            return;

        juce::String error = result->ok ? juce::String() : result->error;
        if (error.isEmpty()
            && ! safe->engine.prepareMonitor (song, std::move (result->stems),
                                              safe->participants, 3.0, error))
        {
            // error already set by prepareMonitor
        }

        if (error.isNotEmpty())
        {
            safe->phase = Phase::idle;
            safe->jamStatusLabel.setText (error, juce::dontSendNotification);
            safe->updateJamButtons();
            return;
        }

        safe->phase       = Phase::preparing;
        safe->jamId       = juce::Uuid().toString();
        safe->jamSongId   = song.id;
        safe->jamSongName = song.name;

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("jamId",       safe->jamId);
        obj->setProperty ("songId",      safe->jamSongId);
        obj->setProperty ("songName",    safe->jamSongName);
        obj->setProperty ("leadSeconds", safe->engine.getLeadSeconds());
        safe->server.broadcastJson (MsgType::jamPrepare, juce::var (obj));

        safe->rebuildMixerStrips();
        safe->broadcastJamState();
        safe->appendLog ("Prepare Jam: " + safe->jamSongName + " ("
                         + safe->participants.joinIntoString (", ") + ")");
        safe->updatePrepareStatusText();
        safe->musiciansList.repaint();
        safe->updateJamButtons();
    });
}

void HostView::handlePrepareStatus (const juce::String& name, const juce::String& id,
                                    const juce::String& state, int percent, const juce::String& error)
{
    if (id != jamId || phase != Phase::preparing)
        return;

    prepareStates[name] = { state, percent, error };
    if (state == "error")
        appendLog (name + ": preparation failed - " + (error.isNotEmpty() ? error : "unknown"));

    musiciansList.repaint();
    updatePrepareStatusText();
    broadcastJamState();
    updateJamButtons();
}

bool HostView::allParticipantsReady() const
{
    if (participants.isEmpty())
        return false;

    for (const auto& p : participants)
    {
        const auto it = prepareStates.find (p);
        if (it == prepareStates.end() || it->second.state != "ready")
            return false;
    }
    return true;
}

void HostView::updatePrepareStatusText()
{
    if (phase != Phase::preparing)
        return;

    int ready = 0;
    for (const auto& p : participants)
    {
        const auto it = prepareStates.find (p);
        if (it != prepareStates.end() && it->second.state == "ready")
            ++ready;
    }

    if (allParticipantsReady())
        jamStatusLabel.setText ("All musicians ready - soundcheck running. Hit \"Start Jam\"!",
                                juce::dontSendNotification);
    else
        jamStatusLabel.setText ("Distributing song... " + juce::String (ready) + "/"
                                + juce::String (participants.size()) + " ready",
                                juce::dontSendNotification);
}

void HostView::handleClientsChanged()
{
    musiciansModel->names = server.getClientNames();
    musiciansList.updateContent();
    musiciansList.repaint();

    if (phase == Phase::preparing || phase == Phase::loading)
    {
        // Drop participants that disconnected while preparing.
        juce::StringArray still;
        for (const auto& p : participants)
            if (musiciansModel->names.contains (p))
                still.add (p);

        if (still.size() != participants.size())
        {
            participants = still;
            if (participants.isEmpty())
            {
                stopJam ("All musicians disconnected", true);
                return;
            }
            broadcastJamState();
            updatePrepareStatusText();
            updateJamButtons();
        }
    }
}

void HostView::startJamClicked()
{
    if (phase != Phase::preparing || ! allParticipantsReady())
        return;

    beginCountdown();
}

void HostView::beginCountdown()
{
    phase = Phase::countdown;
    countdownDeadlineMs = juce::Time::getMillisecondCounter() + 3000;

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("seconds", 3);
    server.broadcastJson (MsgType::jamCountdown, juce::var (obj));

    countdownLabel.setVisible (true);
    appendLog ("Start Jam - Countdown!");
    updateJamButtons();
}

void HostView::goLive()
{
    phase = Phase::running;
    countdownLabel.setVisible (false);

    const bool record = recordToggle.getToggleState() || autoRecordOnJam;
    engine.startJamPlayback (record);
    if (record)
        appendLog (juce::String (autoRecordOnJam && ! recordToggle.getToggleState() ? "Auto-record: " : "")
                   + "Recording stems into: " + engine.getRecordingFolder().getFullPathName());

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("jamId", jamId);
    server.broadcastJson (MsgType::jamGo, juce::var (obj));

    appendLog ("GO! Waiting for " + juce::String (engine.getLeadSeconds(), 1) + " s of buffer from everyone ...");
    updateJamButtons();
}

void HostView::stopJam (const juce::String& reason, bool broadcast)
{
    if (phase == Phase::idle)
        return;

    ++prepareGeneration;   // orphan an in-flight decode, if any

    if (broadcast)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("reason", reason);
        server.broadcastJson (MsgType::jamStop, juce::var (obj));
    }

    engine.stopAndClose();
    phase = Phase::idle;
    jamId.clear();
    prepareStates.clear();
    countdownLabel.setVisible (false);
    stemStrips.clear();
    performerStrips.clear();
    performerStripNames.clear();
    layoutMixerStrips();
    masterMeter.setLevel (0.0f);

    jamStatusLabel.setText ("Jam ended: " + reason, juce::dontSendNotification);
    appendLog ("Jam ended: " + reason);
    musiciansList.repaint();
    updateJamButtons();
}

void HostView::broadcastJamState()
{
    juce::String phaseName = phase == Phase::preparing ? "preparing"
                           : phase == Phase::countdown ? "countdown"
                           : phase == Phase::running   ? "running" : "idle";

    juce::Array<juce::var> parts;
    for (const auto& p : participants)
    {
        auto* po = new juce::DynamicObject();
        po->setProperty ("name", p);
        const auto it = prepareStates.find (p);
        po->setProperty ("ready", it != prepareStates.end() && it->second.state == "ready");
        po->setProperty ("buffered", engine.getBufferedSeconds (p));
        parts.add (juce::var (po));
    }

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("phase", phaseName);
    obj->setProperty ("songName", jamSongName);
    obj->setProperty ("participants", parts);
    server.broadcastJson (MsgType::jamState, juce::var (obj));
}

//==============================================================================
void HostView::rebuildMixerStrips()
{
    stemStrips.clear();
    performerStrips.clear();
    performerStripNames.clear();

    // Outside a jam the mixer belongs to the preview player.
    if (phase == Phase::idle && engine.isPreviewLoaded())
    {
        for (int i = 0; i < engine.getNumPreviewStems(); ++i)
        {
            auto* strip = new ChannelStrip ("Stem: " + engine.getPreviewStemName (i));
            strip->onGain = [this, i] (float db)  { engine.setPreviewStemGainDb (i, db); };
            strip->onMute = [this, i] (bool mute) { engine.setPreviewStemMute (i, mute); };
            mixerContainer.addAndMakeVisible (strip);
            stemStrips.add (strip);
        }

        layoutMixerStrips();
        return;
    }

    for (int i = 0; i < engine.getNumStems(); ++i)
    {
        auto* strip = new ChannelStrip ("Stem: " + engine.getStemName (i));
        strip->onGain = [this, i] (float db)  { engine.setStemGainDb (i, db); };
        strip->onMute = [this, i] (bool mute) { engine.setStemMute (i, mute); };
        mixerContainer.addAndMakeVisible (strip);
        stemStrips.add (strip);
    }

    for (const auto& name : engine.getPerformerNames())
    {
        auto* strip = new ChannelStrip ("Musician: " + name);
        strip->onGain = [this, name] (float db)  { engine.setPerformerGainDb (name, db); };
        strip->onMute = [this, name] (bool mute) { engine.setPerformerMute (name, mute); };
        mixerContainer.addAndMakeVisible (strip);
        performerStrips.add (strip);
        performerStripNames.add (name);
    }

    layoutMixerStrips();
}

void HostView::layoutMixerStrips()
{
    const int rowHeight = 34;
    const int total = 1 + stemStrips.size() + performerStrips.size();   // +1: "My Instrument"
    const int width = mixerViewport.getWidth() - (total * rowHeight > mixerViewport.getHeight() ? 12 : 0);

    mixerContainer.setSize (juce::jmax (0, width), juce::jmax (0, total * rowHeight));

    int y = 0;
    hostInputStrip.setBounds (0, y, mixerContainer.getWidth(), rowHeight - 4);
    y += rowHeight;

    for (auto* strip : stemStrips)
    {
        strip->setBounds (0, y, mixerContainer.getWidth(), rowHeight - 4);
        y += rowHeight;
    }
    for (auto* strip : performerStrips)
    {
        strip->setBounds (0, y, mixerContainer.getWidth(), rowHeight - 4);
        y += rowHeight;
    }
}

//==============================================================================
void HostView::timerCallback()
{
    // Countdown -> Go
    if (phase == Phase::countdown)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (now >= countdownDeadlineMs)
        {
            goLive();
        }
        else
        {
            const int remaining = (int) ((countdownDeadlineMs - now + 999) / 1000);
            countdownLabel.setText (juce::String (juce::jmax (1, remaining)), juce::dontSendNotification);
        }
    }

    // Live levels for the strips during soundcheck and jam.
    if (phase == Phase::preparing || phase == Phase::running || phase == Phase::countdown)
    {
        for (int i = 0; i < performerStrips.size(); ++i)
        {
            const auto& name = performerStripNames.getReference (i);
            performerStrips.getUnchecked (i)->setLevel (engine.getPerformerLevel (name));

            if (phase == Phase::running)
                performerStrips.getUnchecked (i)->setInfoText (
                    juce::String (engine.getBufferedSeconds (name), 1) + " s");
            else
                performerStrips.getUnchecked (i)->setInfoText ("Soundcheck");
        }
        masterMeter.setLevel (engine.getOutputLevel());
        musiciansList.repaint();
    }

    // The host's own input is live in every phase.
    hostInputStrip.setLevel (engine.getHostInputLevel());

    // Status text while running
    if (phase == Phase::running)
    {
        if (! engine.hasStarted())
        {
            const auto waiting = engine.getWaitingFor();
            jamStatusLabel.setText ("Collecting buffer... waiting for: "
                                    + (waiting.isEmpty() ? juce::String ("-") : waiting.joinIntoString (", ")),
                                    juce::dontSendNotification);
        }
        else
        {
            const auto fmt = [] (double seconds)
            {
                const int s = juce::jmax (0, (int) seconds);
                return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
            };
            jamStatusLabel.setText ("JAM RUNNING - " + fmt (engine.getPositionSeconds())
                                    + " / " + fmt (engine.getSongLengthSeconds())
                                    + (engine.getRecordingFolder() != juce::File() ? "   (recording)" : ""),
                                    juce::dontSendNotification);
        }
    }

    // Preview player position (only meaningful while no jam is active).
    if (phase == Phase::idle && engine.isPreviewLoaded())
    {
        if (! previewSlider.isMouseButtonDown())
            previewSlider.setValue (engine.getPreviewPositionSeconds(), juce::dontSendNotification);
        previewTimeLabel.setText (formatTime (engine.getPreviewPositionSeconds()) + " / "
                                  + formatTime (engine.getPreviewLengthSeconds()),
                                  juce::dontSendNotification);
        previewPlayButton.setButtonText (engine.isPreviewPlaying() ? "Pause" : "Play");
        masterMeter.setLevel (engine.getOutputLevel());
    }

    // Periodic state broadcast (roughly 1 Hz at the 30 Hz timer)
    if (phase == Phase::preparing || phase == Phase::running)
        if (++stateBroadcastTick % 30 == 0)
            broadcastJamState();

    // MIDI activity for the VST instrument (Audio settings).
    if (engine.hasInstrument())
    {
        juce::String text;
        juce::Colour colour;

        if (engine.getMidiInputIdentifier().isEmpty())
        {
            text   = "no MIDI port selected";
            colour = style::warn();
        }
        else if (engine.midiOpenFailed())
        {
            text   = "MIDI port busy - close other app, Rescan";
            colour = style::bad();
        }
        else if (engine.getMidiEventCount() == 0)
        {
            text   = "MIDI: no events yet";
            colour = style::textDim();
        }
        else
        {
            text = "MIDI ok - " + juce::String (engine.getMidiEventCount()) + " events";
            if (engine.getLastMidiNote() >= 0)
                text += ", last note " + juce::String (engine.getLastMidiNote());
            colour = style::good();
        }

        midiActivityLabel.setText (text, juce::dontSendNotification);
        midiActivityLabel.setColour (juce::Label::textColourId, colour);
    }
    else if (midiActivityLabel.getText().isNotEmpty())
    {
        midiActivityLabel.setText ({}, juce::dontSendNotification);
    }

    updatePreviewAutoRecord();
    updateTalkAutoMute();
}

void HostView::updateTalkAutoMute()
{
    const bool songPlaying = engine.isPreviewPlaying();
    const bool jamActive   = phase == Phase::countdown || phase == Phase::running;
    const bool shouldMute  = (muteTalkOnPlay && songPlaying) || (muteTalkOnJam && jamActive);

    if (shouldMute == talkAutoMuted)
        return;
    talkAutoMuted = shouldMute;

    if (shouldMute)
    {
        // Remember what was on so we can restore it afterwards.
        talkResumeVoice  = voice.isTalking();
        talkResumeStream = engine.getStreamOutput().isTalkEnabled();

        if (talkResumeVoice)
        {
            voice.setTalking (false);
            chatPanel.setTalkActive (false);
        }
        if (talkResumeStream)
            engine.getStreamOutput().setTalkEnabled (false);

        if (talkResumeVoice || talkResumeStream)
            chatPanel.addSystemMessage (songPlaying ? "Talk mic auto-muted while the song plays"
                                                    : "Talk mic auto-muted during the jam");
    }
    else
    {
        if (talkResumeStream)
            engine.getStreamOutput().setTalkEnabled (true);
        if (talkResumeVoice && voice.isRunning() && voice.getTalkMicName().isNotEmpty())
        {
            voice.setTalking (true);
            chatPanel.setTalkActive (true);
        }
        if (talkResumeVoice || talkResumeStream)
            chatPanel.addSystemMessage ("Talk mic re-enabled");

        talkResumeVoice = talkResumeStream = false;
    }
}

void HostView::appendLog (const juce::String& line)
{
    const auto stamped = juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + line + "\n";
    logView.moveCaretToEnd();
    logView.insertTextAtCaret (stamped);
}

void HostView::showSendRecordingMenu (const juce::File& folder, const juce::String& song,
                                      juce::Component& anchor)
{
    const auto clients = server.getClientNames();
    if (clients.isEmpty())
    {
        if (recordingsPanel != nullptr)
            recordingsPanel->showStatus ("No musicians connected - start the server and wait for them to join.", true);
        return;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader ("Send \"" + song + "\" to");
    for (int i = 0; i < clients.size(); ++i)
        menu.addItem (i + 1, clients[i]);
    if (clients.size() > 1)
    {
        menu.addSeparator();
        menu.addItem (1000, "All musicians");
    }

    juce::Component::SafePointer<HostView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor),
        [safe, folder, song, clients] (int choice)
    {
        if (choice == 0 || safe == nullptr)
            return;

        const auto target = choice == 1000 ? juce::String() : clients[choice - 1];
        const int offered = safe->server.offerRecording (target, folder);

        const auto who = target.isEmpty() ? juce::String ("all musicians") : target;
        if (offered > 0)
        {
            safe->appendLog ("Recording \"" + song + "\" offered to " + who + ".");
            if (safe->recordingsPanel != nullptr)
                safe->recordingsPanel->showStatus ("Offered \"" + song + "\" to " + who
                                                   + " - they get asked before the transfer starts.");
        }
        else
        {
            if (safe->recordingsPanel != nullptr)
                safe->recordingsPanel->showStatus ("Could not offer the recording (musician disconnected?).", true);
        }
    });
}

//==============================================================================
void HostView::handleSongOffer (const juce::String& clientName, const juce::var& offer)
{
    const auto offerId  = offer.getProperty ("offerId", juce::String()).toString();
    const auto songName = offer.getProperty ("name", juce::String()).toString();
    const int  numFiles = (int) offer.getProperty ("numFiles", 0);
    const auto bytes    = (juce::int64) offer.getProperty ("totalBytes", 0);
    if (offerId.isEmpty() || songName.isEmpty())
        return;

    const auto sizeText = juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
    appendLog (clientName + " wants to send the song \"" + songName + "\" (" + sizeText + ")");

    juce::Component::SafePointer<HostView> safe (this);
    juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
        "Receive song?",
        clientName + " wants to send you the song\n\"" + songName + "\"\n("
            + juce::String (numFiles) + (numFiles == 1 ? " file, " : " files, ") + sizeText
            + ").\n\nAccept it and add it to your library?",
        this, juce::ModalCallbackFunction::create ([safe, clientName, offerId, songName] (int okPressed)
    {
        if (safe == nullptr)
            return;

        if (! safe->server.answerSongOffer (clientName, offerId, okPressed != 0))
            safe->appendLog ("Could not answer the song offer (musician disconnected?)");
        else if (okPressed != 0)
            safe->appendLog ("Receiving \"" + songName + "\" from " + clientName + "...");
        else
            safe->appendLog ("Song \"" + songName + "\" from " + clientName + " declined");
    }));
}

void HostView::handleSongReceived (const juce::String& clientName, const juce::String& songName,
                                   const juce::File& folder, bool ok, const juce::String& error)
{
    if (! ok)
    {
        appendLog ("Receiving \"" + songName + "\" from " + clientName + " failed: "
                   + (error.isNotEmpty() ? error : "unknown error"));
        return;
    }

    const auto files = folder.findChildFiles (juce::File::findFiles, false);
    juce::String importError;
    if (library.addSong (songName, files, importError).isEmpty())
        appendLog ("Import of \"" + songName + "\" failed: " + importError);
    else
        appendLog ("Song \"" + songName + "\" received from " + clientName
                   + " and added to the library.");   // onChanged broadcasts the new list

    folder.deleteRecursively();
}

} // namespace bandjam
