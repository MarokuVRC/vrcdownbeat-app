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
    // -- header ------------------------------------------------------------------
    headerLabel.setText ("Host", juce::dontSendNotification);
    headerLabel.setFont (style::titleFont());
    addAndMakeVisible (headerLabel);

    leaveButton.setButtonText ("Leave");
    leaveButton.onClick = [this]
    {
        stopJam ("Host closed", true);
        voice.stop();
        server.stop();
        if (onLeave) onLeave();
    };
    addAndMakeVisible (leaveButton);

    // -- server panel ---------------------------------------------------------------
    nameCaption.setText ("Name:", juce::dontSendNotification);
    nameCaption.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (nameCaption);

    nameEditor.setText (settings::get ("hostName", "").toString());
    nameEditor.setInputRestrictions (24);
    nameEditor.setTextToShowWhenEmpty ("your name", style::textDim());
    addAndMakeVisible (nameEditor);

    portCaption.setText ("Port:", juce::dontSendNotification);
    portCaption.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (portCaption);

    portEditor.setText (settings::get ("hostPort", kDefaultPort).toString());
    portEditor.setInputRestrictions (5, "0123456789");
    portEditor.setJustification (juce::Justification::centred);
    addAndMakeVisible (portEditor);

    serverButton.setButtonText ("Start own Server");
    serverButton.onClick = [this] { toggleServer(); };
    addAndMakeVisible (serverButton);

    roomButton.setButtonText ("Host via Downbeat Server");
    roomButton.setTooltip ("Opens a room on the relay server. Musicians join with the room code - "
                           "no port forwarding, no IP sharing on either side.");
    roomButton.onClick = [this] { toggleRoom(); };
    addAndMakeVisible (roomButton);

    copyCodeButton.setButtonText ("Copy code");
    copyCodeButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (relayLink.getRoomCode());
    };
    addChildComponent (copyCodeButton);   // only visible while a room is open

    serverStatusLabel.setText ("Server is off", juce::dontSendNotification);
    serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (serverStatusLabel);

    ipLabel.setColour (juce::Label::textColourId, style::textDim());
    ipLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (ipLabel);

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
    addAndMakeVisible (showIpsButton);

    portTestButton.setButtonText ("Test port");
    portTestButton.setEnabled (false);
    portTestButton.onClick = [this] { testPortClicked(); };
    addAndMakeVisible (portTestButton);

    portTestResultLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    portTestResultLabel.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (portTestResultLabel);

    refreshIpLabel();
    fetchPublicIp();

    // -- tabs -------------------------------------------------------------------
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

    jamStatusLabel.setText ("Start the server, pick a song, let musicians connect - then \"Prepare Jam\".",
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
            source.subtitle      = "set the device in the Audio tab";
            source.makeSubtitle  = []
            {
                auto device = settings::get ("hostStreamTalkDevice", "").toString();
                return device.isNotEmpty() ? device : juce::String ("no talk mic - set one in the Audio tab");
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
    chatPanel.setStatusText ("Start the server to chat with the band.");
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
            chatPanel.addSystemMessage ("Talk mic is auto-muted right now - turn the toggle off in the Audio tab to talk");
            return;
        }
        if (talk && ! voice.isRunning())
            startVoice();
        voice.setTalking (talk && voice.isRunning() && voice.getTalkMicName().isNotEmpty());
        if (talk && voice.getTalkMicName().isEmpty())
        {
            chatPanel.setTalkActive (false);
            chatPanel.addSystemMessage ("Select a talk mic in the Audio tab first");
        }
    };

    style::styleSectionLabel (logCaption, "Log");
    sessionPage.addAndMakeVisible (logCaption);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setCaretVisible (false);
    logView.setFont (juce::Font (juce::FontOptions (12.5f)));
    sessionPage.addAndMakeVisible (logView);

    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    tabs.addTab ("Session",      juce::Colours::transparentBlack, &sessionPage, false);
    tabs.addTab ("Audio",        juce::Colours::transparentBlack, &audioPage, false);
    tabs.addTab ("Audio Stream", juce::Colours::transparentBlack, &streamPage, false);
    tabs.addTab ("Recordings",   juce::Colours::transparentBlack, &recordingsPage, false);
    addAndMakeVisible (tabs);

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

    engine.onRecordingSaved = [safe] (juce::File folder)
    {
        if (safe != nullptr)
        {
            safe->appendLog ("Recording saved (all stems): " + folder.getFullPathName());
            safe->jamStatusLabel.setText ("Recording saved - open the Recordings tab to remix and export it.",
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
    stopJam ("Host closed", true);
    voice.stop();          // before server.stop(): its sender uses the server
    server.stop();
    musiciansList.setModel (nullptr);
}

//==============================================================================
void HostView::paint (juce::Graphics& g)
{
    g.fillAll (style::background());

    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (44);
    style::drawPanel (g, area.removeFromTop (92).toFloat());
}

void HostView::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto header = area.removeFromTop (36);
    leaveButton.setBounds (header.removeFromRight (110).reduced (0, 4));
    headerLabel.setBounds (header);
    area.removeFromTop (8);

    // Server panel (two rows)
    auto serverPanel = area.removeFromTop (92).reduced (12, 10);
    auto row1 = serverPanel.removeFromTop (32);
    nameCaption.setBounds (row1.removeFromLeft (50));
    nameEditor.setBounds (row1.removeFromLeft (130).reduced (0, 2));
    row1.removeFromLeft (10);
    portCaption.setBounds (row1.removeFromLeft (44));
    portEditor.setBounds (row1.removeFromLeft (70).reduced (0, 2));
    row1.removeFromLeft (10);
    serverButton.setBounds (row1.removeFromLeft (128).reduced (0, 1));
    row1.removeFromLeft (8);
    roomButton.setBounds (row1.removeFromLeft (190).reduced (0, 1));
    row1.removeFromLeft (8);
    copyCodeButton.setBounds (row1.removeFromLeft (92).reduced (0, 1));
    row1.removeFromLeft (10);
    serverStatusLabel.setBounds (row1);

    serverPanel.removeFromTop (8);
    auto row2 = serverPanel.removeFromTop (32);
    portTestResultLabel.setBounds (row2.removeFromRight (juce::jmin (260, row2.getWidth() / 3)));
    row2.removeFromRight (8);
    portTestButton.setBounds (row2.removeFromRight (110).reduced (0, 3));
    row2.removeFromRight (12);
    showIpsButton.setBounds (row2.removeFromLeft (90).reduced (0, 3));
    row2.removeFromLeft (12);
    ipLabel.setBounds (row2);
    area.removeFromTop (8);

    tabs.setBounds (area);
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
void HostView::toggleServer()
{
    if (server.isRunning())
    {
        stopHosting();
        return;
    }

    if (! applyHostName())
        return;

    const int port = juce::jlimit (1, 65535, portEditor.getText().getIntValue());
    juce::String error;
    if (server.start (port, error))
    {
        settings::set ("hostPort", port);
        serverButton.setButtonText ("Stop own Server");
        roomButton.setEnabled (false);
        serverStatusLabel.setText ("Running on port " + juce::String (port), juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::good());
        portTestButton.setEnabled (true);
        onHostingStarted();
    }
    else
    {
        serverStatusLabel.setText (error, juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::bad());
    }
    updateJamButtons();
}

void HostView::toggleRoom()
{
    if (server.isRunning())
    {
        stopHosting();
        return;
    }

    if (! applyHostName())
        return;

    relayMode = true;
    server.startVirtual();
    roomButton.setButtonText ("Close room");
    serverButton.setEnabled (false);
    serverStatusLabel.setText ("Opening a room on the relay server...", juce::dontSendNotification);
    serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());

    juce::String relayHost;
    int relayPort = relay::kDefaultPort;
    relay::parseAddress (settings::get ("relayAddress", juce::String (relay::kDefaultAddress)).toString(),
                         relayHost, relayPort);
    relayLink.open (relayHost, relayPort, server.getHostName());
    updateJamButtons();
}

bool HostView::applyHostName()
{
    const auto name = nameEditor.getText().trim();
    if (name.isEmpty())
    {
        serverStatusLabel.setText ("Please enter a name first.", juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::warn());
        nameEditor.grabKeyboardFocus();
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

    serverButton.setButtonText ("Start own Server");
    serverButton.setEnabled (true);
    roomButton.setButtonText ("Host via Downbeat Server");
    roomButton.setEnabled (true);
    copyCodeButton.setVisible (false);
    portTestButton.setEnabled (false);

    if (showOff)
    {
        serverStatusLabel.setText ("Server is off", juce::dontSendNotification);
        serverStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    }

    chatPanel.setTalkAvailable (false);
    chatPanel.setStatusText ("Start the server to chat with the band.");
    updateJamButtons();
}

void HostView::onHostingStarted()
{
    chatPanel.setTalkAvailable (true);
    chatPanel.setStatusText ("Talk uses the talk mic from the Audio tab.");
    startVoice();   // hear the band's voice chat right away
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
    if (portTestRunning || ! server.isRunning())
        return;

    if (publicIp.isEmpty() || publicIp == "not detectable")
    {
        portTestResultLabel.setText ("Public IP unknown - can't run the test.",
                                     juce::dontSendNotification);
        portTestResultLabel.setColour (juce::Label::textColourId, style::warn());
        fetchPublicIp();
        return;
    }

    portTestRunning = true;
    portTestButton.setEnabled (false);
    portTestResultLabel.setText ("Testing... (up to 15 s)", juce::dontSendNotification);
    portTestResultLabel.setColour (juce::Label::textColourId, style::textDim());

    juce::Component::SafePointer<HostView> safe (this);
    juce::Thread::launch ([safe, ip = publicIp, port = server.getPort()]
    {
        const auto verdict = runExternalPortTest (ip, port);

        juce::MessageManager::callAsync ([safe, verdict]
        {
            if (safe == nullptr) return;

            safe->portTestRunning = false;
            safe->portTestButton.setEnabled (safe->server.isRunning());

            juce::String text;
            juce::Colour colour;
            if (verdict == "open")
            {
                text   = "Port open - reachable from outside!";
                colour = style::good();
            }
            else if (verdict == "closed")
            {
                text   = "Port closed! Check the port forwarding in your router.";
                colour = style::bad();
            }
            else if (verdict == "timeout")
            {
                text   = "No answer from the test service.";
                colour = style::warn();
            }
            else
            {
                text   = "Test service not reachable.";
                colour = style::warn();
            }
            safe->portTestResultLabel.setText (text, juce::dontSendNotification);
            safe->portTestResultLabel.setColour (juce::Label::textColourId, colour);
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
        jamStatusLabel.setText ("No audio device active - check the Audio tab.", juce::dontSendNotification);
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

    engine.startJamPlayback (recordToggle.getToggleState());
    if (recordToggle.getToggleState())
        appendLog ("Recording stems into: " + engine.getRecordingFolder().getFullPathName());

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
