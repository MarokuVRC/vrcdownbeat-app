#include "MusicianView.h"
#include "ui/Style.h"
#include "common/Settings.h"

namespace bandjam
{
MusicianView::MusicianView()
{
    // -- header ------------------------------------------------------------------
    headerLabel.setText ("Musician", juce::dontSendNotification);
    headerLabel.setFont (style::titleFont());
    addAndMakeVisible (headerLabel);

    leaveButton.setButtonText ("Leave");
    leaveButton.onClick = [this]
    {
        voice.stop();
        engine.stopJam();
        engine.stopMonitor();
        connection.disconnect();
        if (onLeave) onLeave();
    };
    addAndMakeVisible (leaveButton);

    // -- connect bar ----------------------------------------------------------------
    auto initCaption = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, style::textDim());
        addAndMakeVisible (label);
    };

    initCaption (nameCaption, "Name:");
    initCaption (hostCaption, "Host:");
    initCaption (portCaption, "Port:");
    initCaption (roomCaption, "Room:");

    nameEditor.setText (settings::get ("musicianName", "").toString());
    addAndMakeVisible (nameEditor);

    hostEditor.setText (settings::get ("lastHost", "127.0.0.1").toString());
    addAndMakeVisible (hostEditor);

    portEditor.setText (settings::get ("lastPort", kDefaultPort).toString());
    portEditor.setInputRestrictions (5, "0123456789");
    addAndMakeVisible (portEditor);

    // Room code (relay). When filled in, it wins over Host/Port - no port
    // forwarding or IP sharing needed on either side.
    roomEditor.setText (settings::get ("lastRoom", "").toString());
    roomEditor.setInputRestrictions (6, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    roomEditor.setJustification (juce::Justification::centred);
    roomEditor.setTextToShowWhenEmpty ("code", style::textDim());
    addAndMakeVisible (roomEditor);

    connectButton.setButtonText ("Connect");
    connectButton.onClick = [this] { toggleConnect(); };
    addAndMakeVisible (connectButton);

    connStatusLabel.setText ("Not connected", juce::dontSendNotification);
    connStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    addAndMakeVisible (connStatusLabel);

    // -- tabs -------------------------------------------------------------------
    // "Session" holds the jam workflow; "VRChat Stream" the virtual-mic and
    // per-app routing tools (kept separate so the main view stays uncluttered).
    sessionPage.onLayout = [this] { layoutSessionPage(); };

    auto initSessionCaption = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, style::textDim());
        sessionPage.addAndMakeVisible (label);
    };

    // -- songs -------------------------------------------------------------------
    style::styleSectionLabel (songsCaption, "Songs from host");
    sessionPage.addAndMakeVisible (songsCaption);

    songsList.setRowHeight (30);
    sessionPage.addAndMakeVisible (songsList);

    downloadButton.setButtonText ("Download");
    downloadButton.setEnabled (false);
    downloadButton.onClick = [this] { downloadClicked(); };
    sessionPage.addAndMakeVisible (downloadButton);

    deleteButton.setButtonText ("Delete");
    deleteButton.setEnabled (false);
    deleteButton.onClick = [this] { deleteClicked(); };
    sessionPage.addAndMakeVisible (deleteButton);

    downloadStatusLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    downloadStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    sessionPage.addAndMakeVisible (downloadStatusLabel);

    // -- player / stems -------------------------------------------------------------
    style::styleSectionLabel (playerCaption, "Player & stem mix");
    sessionPage.addAndMakeVisible (playerCaption);

    songTitleLabel.setText ("No song selected", juce::dontSendNotification);
    songTitleLabel.setFont (style::sectionFont());
    sessionPage.addAndMakeVisible (songTitleLabel);

    loadButton.setButtonText ("Load");
    loadButton.setEnabled (false);
    loadButton.onClick = [this] { loadSelectedSong(); };
    sessionPage.addAndMakeVisible (loadButton);

    playButton.setButtonText ("Play");
    playButton.setEnabled (false);
    playButton.onClick = [this]
    {
        if (engine.isPreviewPlaying()) engine.previewPause();
        else                           engine.previewPlay();
        updateTransportButtons();
    };
    sessionPage.addAndMakeVisible (playButton);

    stopButton.setButtonText ("Stop");
    stopButton.setEnabled (false);
    stopButton.onClick = [this] { engine.previewStop(); updateTransportButtons(); };
    sessionPage.addAndMakeVisible (stopButton);

    positionSlider.setRange (0.0, 1.0, 0.01);
    positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    positionSlider.onDragEnd = [this] { engine.previewSeekSeconds (positionSlider.getValue()); };
    sessionPage.addAndMakeVisible (positionSlider);

    timeLabel.setText ("0:00 / 0:00", juce::dontSendNotification);
    timeLabel.setColour (juce::Label::textColourId, style::textDim());
    timeLabel.setJustificationType (juce::Justification::centredRight);
    sessionPage.addAndMakeVisible (timeLabel);

    stemsViewport.setViewedComponent (&stemsContainer, false);
    stemsViewport.setScrollBarsShown (true, false);
    sessionPage.addAndMakeVisible (stemsViewport);

    // -- jam ------------------------------------------------------------------------
    style::styleSectionLabel (jamCaption, "Jam");
    sessionPage.addAndMakeVisible (jamCaption);

    jamStatusLabel.setText ("No jam active.", juce::dontSendNotification);
    jamStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    sessionPage.addAndMakeVisible (jamStatusLabel);

    countdownLabel.setFont (juce::Font (juce::FontOptions (64.0f, juce::Font::bold)));
    countdownLabel.setColour (juce::Label::textColourId, style::accent());
    countdownLabel.setJustificationType (juce::Justification::centred);
    // Overlays the stem strips: must never eat their mouse events, and
    // addChildComponent (NOT addAndMakeVisible) keeps it hidden until the
    // countdown actually runs.
    countdownLabel.setInterceptsMouseClicks (false, false);
    sessionPage.addChildComponent (countdownLabel);

    initSessionCaption (backingMeterCaption, "Backing:");
    sessionPage.addAndMakeVisible (backingMeter);
    initSessionCaption (micMeterCaption, "Input:");
    sessionPage.addAndMakeVisible (micMeter);

    // -- band chat (Session tab, right column) -----------------------------------
    sessionPage.addAndMakeVisible (chatPanel);
    chatPanel.setStatusText ("Connect to a host to chat.");
    chatPanel.setTalkAvailable (false);
    chatPanel.getTalkLevel = [this] { return voice.getTalkLevel(); };
    chatPanel.onSendText = [this] (const juce::String& text)
    {
        if (! connection.sendChat (text))
            chatPanel.addSystemMessage ("Not connected - message not sent");
        // The server echoes the message back to everyone including us.
    };
    chatPanel.onTalkToggled = [this] (bool talk)
    {
        if (talk && ! voice.isRunning())
            startVoice();
        voice.setTalking (talk && voice.isRunning() && voice.getTalkMicName().isNotEmpty());
        if (talk && voice.getTalkMicName().isEmpty())
        {
            chatPanel.setTalkActive (false);
            chatPanel.addSystemMessage ("Select a talk mic in the Audio tab first");
        }
    };

    // -- Audio tab: device settings ---------------------------------------------------
    auto initAudioCaption = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, style::textDim());
        audioPage.addAndMakeVisible (label);
    };

    style::styleSectionLabel (audioCaption, "Audio settings");
    audioPage.addAndMakeVisible (audioCaption);

    engine.initialiseDevice();

    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(), 1, 1, 2, 2, false, false, false, false);
    audioPage.addAndMakeVisible (*deviceSelector);

    latencyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    latencyLabel.setJustificationType (juce::Justification::topLeft);
    audioPage.addAndMakeVisible (latencyLabel);

    initAudioCaption (offsetCaption, "Manual offset (ms):");
    offsetSlider.setRange (-250.0, 250.0, 0.5);
    offsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    offsetSlider.setValue (PlayCaptureEngine::getManualOffsetMs(), juce::dontSendNotification);
    offsetSlider.onValueChange = [this]
    {
        PlayCaptureEngine::setManualOffsetMs (offsetSlider.getValue());
        refreshLatencyLabels();
    };
    audioPage.addAndMakeVisible (offsetSlider);

    asioHintLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    asioHintLabel.setJustificationType (juce::Justification::topLeft);
    audioPage.addAndMakeVisible (asioHintLabel);

    // -- Audio tab: talk mic (second input, separate from the instrument) ------------
    initAudioCaption (talkMicCaption, "Talk mic:");
    talkMicBox.onChange = [this]
    {
        const int index = talkMicBox.getSelectedItemIndex();
        const auto device = juce::isPositiveAndBelow (index - 1, talkMicDevices.size())
                                ? talkMicDevices[index - 1] : juce::String();
        settings::set ("streamTalkDevice", device);

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

    talkMicHintLabel.setText ("Used for voice chat and the VRChat mic - your instrument "
                              "input above stays separate.", juce::dontSendNotification);
    talkMicHintLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    talkMicHintLabel.setColour (juce::Label::textColourId, style::textDim());
    talkMicHintLabel.setJustificationType (juce::Justification::topLeft);
    audioPage.addAndMakeVisible (talkMicHintLabel);

    refreshTalkMicDevices();

    // -- Audio tab: instrument (VST3) -----------------------------------------------
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

    initAudioCaption (midiCaption, "MIDI in:");
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

    refreshMidiInputs();

    // -- Audio Stream tab: routing board ------------------------------------------------
    {
        // Persisted include flags go straight into the engine before the
        // panel reads them back for the toggles.
        engine.setStreamIncludeSong ((bool) settings::get ("streamIncludeSong", true));
        engine.setStreamIncludeInput ((bool) settings::get ("streamIncludeInput", true));
        engine.getStreamOutput().setTalkEnabled ((bool) settings::get ("streamTalkEnabled", true));

        AudioStreamPanel::Options streamOptions;
        streamOptions.outDeviceSettingsKey    = "streamOutDevice";
        streamOptions.talkDeviceSettingsKey   = "streamTalkDevice";
        streamOptions.capturedAppsSettingsKey = "streamCapturedApps";

        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - song playback";
            source.subtitle      = "the backing track / local player";
            source.getLevel      = [this] { return engine.getBackingLevel(); };
            source.getToMic      = [this] { return engine.getStreamIncludeSong(); };
            source.setToMic      = [this] (bool on) { engine.setStreamIncludeSong (on);
                                                      settings::set ("streamIncludeSong", on); };
            source.youFixedValue = true;
            source.youFixedHint  = "Always plays on your audio device";
            streamOptions.sources.push_back (std::move (source));
        }
        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - your instrument";
            source.subtitle      = "mic/instrument input or VST3";
            source.makeSubtitle  = [this]
            {
                return engine.hasInstrument() ? "VST3: " + engine.getInstrumentName()
                                              : juce::String ("your instrument input");
            };
            source.getLevel      = [this] { return engine.getMicLevel(); };
            source.getToMic      = [this] { return engine.getStreamIncludeInput(); };
            source.setToMic      = [this] (bool on) { engine.setStreamIncludeInput (on);
                                                      settings::set ("streamIncludeInput", on); };
            source.youFixedValue = true;
            source.youFixedHint  = "You monitor it through your audio device";
            streamOptions.sources.push_back (std::move (source));
        }
        {
            AudioStreamPanel::BuiltinSource source;
            source.name          = "BandJam - talk mic";
            source.subtitle      = "set the device in the Audio tab";
            source.makeSubtitle  = []
            {
                auto device = settings::get ("streamTalkDevice", "").toString();
                return device.isNotEmpty() ? device : juce::String ("no talk mic - set one in the Audio tab");
            };
            source.getLevel      = [this] { return engine.getStreamOutput().getTalkLevel(); };
            source.getToMic      = [this] { return engine.getStreamOutput().isTalkEnabled(); };
            source.setToMic      = [this] (bool on) { engine.getStreamOutput().setTalkEnabled (on);
                                                      settings::set ("streamTalkEnabled", on); };
            source.youControlsWindowsListen = true;   // hear yourself via Windows "Listen"
            streamOptions.sources.push_back (std::move (source));
        }

        streamPanel = std::make_unique<AudioStreamPanel> (engine.getStreamOutput(), streamOptions);
        streamPage.addAndMakeVisible (*streamPanel);
    }

    // -- "Recordings" tab: jam recordings received from the host --------------------
    {
        RecordingsPanel::Options recOptions;
        auto& player = recOptions.player;

        player.canLoad = [this] { return jamPhase == JamPhase::idle; };
        player.getDeviceSampleRate = [this] { return engine.getDeviceSampleRate(); };
        player.adopt = [this] (const juce::String& id, const juce::String& title, double sampleRate,
                               juce::int64 lengthSamples, std::vector<songloader::DecodedStem>&& stems,
                               juce::String& error)
        {
            LocalSong meta;
            meta.id            = id;
            meta.name          = title;
            meta.sampleRate    = sampleRate;
            meta.lengthSamples = lengthSamples;

            ++loadGeneration;   // cancel an in-flight Session-tab song decode
            if (! engine.adoptSong (meta, std::move (stems), error))
                return false;

            songTitleLabel.setText (title + " (recording)", juce::dontSendNotification);
            positionSlider.setRange (0.0, juce::jmax (0.1, engine.getLengthSeconds()), 0.01);
            return true;
        };
        player.isCurrent = [this] (const juce::String& id)
        {
            return engine.isLoaded() && engine.getLoadedSongId() == id;
        };
        player.unload = [this]
        {
            engine.unload();
            songTitleLabel.setText ("No song loaded.", juce::dontSendNotification);
        };
        player.play      = [this] { engine.previewPlay(); };
        player.pause     = [this] { engine.previewPause(); };
        player.stop      = [this] { engine.previewStop(); };
        player.isPlaying = [this] { return engine.isPreviewPlaying(); };
        player.seek      = [this] (double seconds) { engine.previewSeekSeconds (seconds); };
        player.getPositionSeconds = [this] { return engine.getPositionSeconds(); };
        player.getLengthSeconds   = [this] { return engine.getLengthSeconds(); };
        player.setStemGainDb = [this] (int i, float db) { engine.setStemGainDb (i, db); };
        player.setStemMute   = [this] (int i, bool m)   { engine.setStemMute (i, m); };
        player.getStemLevel  = [this] (int i)           { return engine.getStemLevel (i); };

        recOptions.showAutoReceiveToggle = true;
        recOptions.getAutoReceive = [] { return (bool) settings::get ("autoReceiveRecordings", false); };
        recOptions.onAutoReceiveChanged = [this] (bool on)
        {
            settings::set ("autoReceiveRecordings", on);
            if (connection.isConnected())
                connection.sendRecordingPrefs (on);
        };

        recordingsPanel = std::make_unique<RecordingsPanel> (std::move (recOptions));
        recordingsPanel->onPreviewChanged = [this]
        {
            // A recording (un)loaded the shared player - keep the Session
            // tab's transport/strips in sync.
            rebuildStemStrips();
            updateSongButtons();
            updateTransportButtons();
        };
        recordingsPage.addAndMakeVisible (*recordingsPanel);
    }

    audioPage.onLayout      = [this] { layoutAudioPage(); };
    streamPage.onLayout     = [this] { layoutStreamPage(); };
    recordingsPage.onLayout = [this] { recordingsPanel->setBounds (recordingsPage.getLocalBounds()); };

    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    tabs.addTab ("Session",      juce::Colours::transparentBlack, &sessionPage, false);
    tabs.addTab ("Audio",        juce::Colours::transparentBlack, &audioPage, false);
    tabs.addTab ("Audio Stream", juce::Colours::transparentBlack, &streamPage, false);
    tabs.addTab ("Recordings",   juce::Colours::transparentBlack, &recordingsPage, false);
    addAndMakeVisible (tabs);

    // Restore the last used plugin (state included) so e.g. Superior Drummer
    // is ready to play right after startup.
    if (const auto saved = engine.getSavedInstrumentFile(); saved.exists())
    {
        juce::String error;
        engine.loadInstrument (saved, error);
    }
    refreshInstrumentUi();

    // -- wiring ---------------------------------------------------------------------
    juce::Component::SafePointer<MusicianView> safe (this);

    connection.onConnected = [safe] (const juce::String& hostName)
    {
        if (safe == nullptr) return;
        safe->connStatusLabel.setText ("Connected to " + hostName, juce::dontSendNotification);
        safe->connStatusLabel.setColour (juce::Label::textColourId, style::good());
        safe->connectButton.setButtonText ("Disconnect");
        settings::set ("musicianName", safe->nameEditor.getText().trim());
        settings::set ("lastHost", safe->hostEditor.getText().trim());
        settings::set ("lastPort", safe->portEditor.getText().getIntValue());
        settings::set ("lastRoom", safe->roomEditor.getText().trim().toUpperCase());

        safe->chatPanel.addSystemMessage ("Connected to " + hostName);
        safe->chatPanel.setTalkAvailable (true);
        safe->chatPanel.setStatusText ("Talk uses the talk mic from the Audio tab.");
        safe->startVoice();   // listen to the band even before talking

        // Tell the host whether we want jam recordings sent automatically.
        safe->connection.sendRecordingPrefs ((bool) settings::get ("autoReceiveRecordings", false));
    };
    connection.onChat = [safe] (const juce::String& name, const juce::String& text)
    {
        if (safe != nullptr) safe->chatPanel.addMessage (name, text);
    };
    connection.onRecordingOffer = [safe] (juce::var offer)
    {
        if (safe != nullptr) safe->handleRecordingOffer (offer);
    };
    connection.onRecordingProgress = [safe] (const juce::String& recId, juce::int64 got, juce::int64 total)
    {
        if (safe == nullptr || safe->recordingsPanel == nullptr)
            return;
        auto mb = [] (juce::int64 bytes) { return juce::String ((double) bytes / (1024.0 * 1024.0), 1); };
        safe->recordingsPanel->showStatus ("Receiving \"" + recId + "\"... " + mb (got)
                                           + (total > 0 ? " / " + mb (total) : juce::String()) + " MB");
    };
    connection.onRecordingEnd = [safe] (const juce::String& recId, bool ok,
                                        const juce::String& error, const juce::File&)
    {
        if (safe == nullptr)
            return;
        if (safe->recordingsPanel != nullptr)
        {
            safe->recordingsPanel->refreshList();
            safe->recordingsPanel->showStatus (
                ok ? "Recording \"" + recId + "\" received - load it to listen, remix and export."
                   : "Receiving \"" + recId + "\" failed: " + error,
                ! ok);
        }
        safe->chatPanel.addSystemMessage (ok ? "Recording received: " + recId
                                             : "Recording transfer failed: " + error);
    };
    connection.onVoiceBlock = [this] (const juce::String& speaker, const juce::int16* samples, int numSamples)
    {
        voice.receiveVoice (speaker, samples, numSamples);   // reader thread - VoiceChat locks internally
    };
    voice.onVoiceBlock = [this] (juce::int64 startSample, const float* mono, int numSamples)
    {
        connection.sendVoiceBlock (startSample, mono, numSamples);
    };
    connection.onDisconnected = [safe] (const juce::String& reason)
    {
        if (safe != nullptr) safe->handleDisconnected (reason);
    };
    connection.onSongList = [safe] (juce::var json)
    {
        if (safe == nullptr) return;
        safe->lastSongListJson = json;
        safe->downloads.syncFromHostList (json);
        safe->songsList.updateContent();
        safe->songsList.repaint();
        safe->selectedRowsChanged (0);
    };
    connection.onStemProgress = [safe] (const juce::String&, const juce::String&,
                                        juce::int64 received, juce::int64 total)
    {
        if (safe == nullptr || total <= 0) return;
        const double fraction = (double) received / (double) total;

        if (safe->jamPhase == JamPhase::downloading)
        {
            const int percent = safe->jamDownloadPercent (fraction);
            safe->downloadStatusLabel.setText ("Jam download... " + juce::String (percent) + " %",
                                               juce::dontSendNotification);
            safe->jamStatusLabel.setText ("Downloading song: " + juce::String (percent) + " %",
                                          juce::dontSendNotification);

            const auto now = juce::Time::getMillisecondCounter();
            if (now - safe->lastStatusSentMs > 400)
            {
                safe->lastStatusSentMs = now;
                safe->sendPrepareStatus ("downloading", percent, {});
            }
        }
        else
        {
            const int percent = (int) (fraction * 100.0);
            safe->downloadStatusLabel.setText ("Downloading... " + juce::String (percent) + " %  ("
                                               + juce::String (safe->downloadQueue.size()) + " more queued)",
                                               juce::dontSendNotification);
        }
    };
    connection.onStemEnd = [safe] (const juce::String& songId, const juce::String& stemId,
                                   bool ok, const juce::String& error)
    {
        if (safe != nullptr) safe->handleStemEnd (songId, stemId, ok, error);
    };
    connection.onJamPrepare = [safe] (juce::var json)
    {
        if (safe != nullptr) safe->handleJamPrepare (json);
    };
    connection.onJamState = [safe] (juce::var json)
    {
        if (safe == nullptr || safe->jamPhase != JamPhase::ready) return;

        int ready = 0, total = 0;
        if (auto* parts = json.getProperty ("participants", juce::var()).getArray())
        {
            total = parts->size();
            for (const auto& p : *parts)
                if ((bool) p.getProperty ("ready", false))
                    ++ready;
        }
        if (total > 0)
            safe->jamStatusLabel.setText ("Ready! Preview & set your mix ("
                                          + juce::String (ready) + "/" + juce::String (total)
                                          + " ready) - the host starts the jam.",
                                          juce::dontSendNotification);
    };
    connection.onJamCountdown = [safe] (int seconds)
    {
        if (safe == nullptr) return;
        safe->jamPhase = JamPhase::countdown;
        safe->engine.previewStop();
        safe->countdownDeadlineMs = juce::Time::getMillisecondCounter() + (juce::uint32) (seconds * 1000);
        safe->countdownLabel.setVisible (true);
        safe->jamStatusLabel.setText ("Here we go!", juce::dontSendNotification);
        safe->updateTransportButtons();
    };
    connection.onJamGo = [safe] (juce::var)
    {
        if (safe != nullptr) safe->handleJamGo();
    };
    connection.onJamStop = [safe] (const juce::String& reason)
    {
        if (safe != nullptr) safe->handleJamStop (reason);
    };

    // Engine sender thread -> network. connection outlives engine (declared first).
    engine.onCaptureBlock = [this] (juce::int64 startSample, const float* mono, int numSamples)
    {
        connection.sendAudioBlock (startSample, mono, numSamples);
    };
    engine.onMonitorBlock = [this] (juce::int64 startSample, const float* mono, int numSamples)
    {
        connection.sendMonitorBlock (startSample, mono, numSamples);
    };
    engine.onDeviceChanged = [safe]
    {
        if (safe != nullptr) safe->refreshLatencyLabels();
    };

    refreshLatencyLabels();
    startTimerHz (30);   // fast enough for live-feeling meters
}

MusicianView::~MusicianView()
{
    stopTimer();
    ++loadGeneration;      // orphan any in-flight decode
    voice.stop();
    engine.stopJam();
    engine.stopMonitor();
    engine.saveInstrumentState();
    connection.disconnect();
    songsList.setModel (nullptr);
}

//==============================================================================
void MusicianView::paint (juce::Graphics& g)
{
    g.fillAll (style::background());

    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (44);
    style::drawPanel (g, area.removeFromTop (56).toFloat());
}

void MusicianView::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto header = area.removeFromTop (36);
    leaveButton.setBounds (header.removeFromRight (110).reduced (0, 4));
    headerLabel.setBounds (header);
    area.removeFromTop (8);

    // Connect bar
    auto row = area.removeFromTop (56).reduced (12, 12);
    nameCaption.setBounds (row.removeFromLeft (48));
    nameEditor.setBounds (row.removeFromLeft (118).reduced (0, 2));
    row.removeFromLeft (8);
    hostCaption.setBounds (row.removeFromLeft (42));
    hostEditor.setBounds (row.removeFromLeft (126).reduced (0, 2));
    row.removeFromLeft (8);
    portCaption.setBounds (row.removeFromLeft (38));
    portEditor.setBounds (row.removeFromLeft (62).reduced (0, 2));
    row.removeFromLeft (8);
    roomCaption.setBounds (row.removeFromLeft (44));
    roomEditor.setBounds (row.removeFromLeft (74).reduced (0, 2));
    row.removeFromLeft (10);
    connectButton.setBounds (row.removeFromLeft (110).reduced (0, 1));
    row.removeFromLeft (10);
    connStatusLabel.setBounds (row);
    area.removeFromTop (8);

    tabs.setBounds (area);
}

void MusicianView::layoutSessionPage()
{
    auto area = sessionPage.getLocalBounds().reduced (0, 8);

    // Columns
    auto left  = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * 0.26f));
    area.removeFromLeft (12);
    auto right = area.removeFromRight (juce::roundToInt ((float) area.getWidth() * 0.34f));
    area.removeFromRight (12);
    auto mid = area;

    // Left: songs
    songsCaption.setBounds (left.removeFromTop (24));
    left.removeFromTop (4);
    downloadStatusLabel.setBounds (left.removeFromBottom (22));
    auto dlRow = left.removeFromBottom (30);
    downloadButton.setBounds (dlRow.removeFromLeft ((dlRow.getWidth() - 6) / 2));
    dlRow.removeFromLeft (6);
    deleteButton.setBounds (dlRow);
    left.removeFromBottom (6);
    songsList.setBounds (left);

    // Mid: player + jam
    playerCaption.setBounds (mid.removeFromTop (24));
    mid.removeFromTop (2);
    songTitleLabel.setBounds (mid.removeFromTop (24));
    mid.removeFromTop (4);
    auto transport = mid.removeFromTop (30);
    loadButton.setBounds (transport.removeFromLeft (86));
    transport.removeFromLeft (6);
    playButton.setBounds (transport.removeFromLeft (86));
    transport.removeFromLeft (6);
    stopButton.setBounds (transport.removeFromLeft (86));
    mid.removeFromTop (6);
    auto posRow = mid.removeFromTop (26);
    timeLabel.setBounds (posRow.removeFromRight (110));
    positionSlider.setBounds (posRow);
    mid.removeFromTop (8);

    // Jam block at the bottom of mid. Height must match the content exactly,
    // otherwise the meters get clipped: 24+2+24+6+20+4+20 = 100.
    auto jamArea = mid.removeFromBottom (100);
    jamCaption.setBounds (jamArea.removeFromTop (24));
    jamArea.removeFromTop (2);
    jamStatusLabel.setBounds (jamArea.removeFromTop (24));
    jamArea.removeFromTop (6);
    auto meterRow1 = jamArea.removeFromTop (20);
    backingMeterCaption.setBounds (meterRow1.removeFromLeft (70));
    backingMeter.setBounds (meterRow1.reduced (0, 2));
    jamArea.removeFromTop (4);
    auto meterRow2 = jamArea.removeFromTop (20);
    micMeterCaption.setBounds (meterRow2.removeFromLeft (70));
    micMeter.setBounds (meterRow2.reduced (0, 2));

    mid.removeFromBottom (8);
    stemsViewport.setBounds (mid);
    countdownLabel.setBounds (mid);
    layoutStemStrips();

    // Right: band chat.
    chatPanel.setBounds (right);
}

void MusicianView::layoutAudioPage()
{
    auto area = audioPage.getLocalBounds().reduced (0, 8);

    // Left column: device settings + latency + offset.
    auto left = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * 0.5f));
    area.removeFromLeft (16);
    auto right = area;

    audioCaption.setBounds (left.removeFromTop (24));
    left.removeFromTop (4);

    // Fixed-height selector (its content doesn't scroll); reserve enough room
    // below it for latency + offset + ASIO hint + the talk mic selector.
    deviceSelector->setBounds (left.removeFromTop (juce::jmax (230, left.getHeight() - 215)));
    left.removeFromTop (4);
    latencyLabel.setBounds (left.removeFromTop (54));
    left.removeFromTop (4);
    auto offsetRow = left.removeFromTop (26);
    offsetCaption.setBounds (offsetRow.removeFromLeft (150));
    offsetSlider.setBounds (offsetRow);
    left.removeFromTop (4);
    asioHintLabel.setBounds (left.removeFromTop (44));
    left.removeFromTop (8);

    auto talkRow = left.removeFromTop (26);
    talkMicCaption.setBounds (talkRow.removeFromLeft (80));
    talkMicBox.setBounds (talkRow);
    left.removeFromTop (4);
    talkMicHintLabel.setBounds (left.removeFromTop (34));

    // Right column: instrument (VST3) + MIDI.
    auto inst = right;
    instCaption.setBounds (inst.removeFromTop (24));
    inst.removeFromTop (4);
    auto instButtons = inst.removeFromTop (28);
    const int buttonWidth = (instButtons.getWidth() - 12) / 3;
    instLoadButton.setBounds (instButtons.removeFromLeft (buttonWidth));
    instButtons.removeFromLeft (6);
    instUiButton.setBounds (instButtons.removeFromLeft (buttonWidth));
    instButtons.removeFromLeft (6);
    instRemoveButton.setBounds (instButtons);
    inst.removeFromTop (6);
    auto midiRow = inst.removeFromTop (26);
    midiCaption.setBounds (midiRow.removeFromLeft (64));
    midiRescanButton.setBounds (midiRow.removeFromRight (70));
    midiRow.removeFromRight (6);
    midiInputBox.setBounds (midiRow);
    inst.removeFromTop (4);
    auto statusRow = inst.removeFromTop (20);
    midiActivityLabel.setBounds (statusRow.removeFromRight (statusRow.getWidth() * 2 / 5));
    instStatusLabel.setBounds (statusRow);
}

void MusicianView::layoutStreamPage()
{
    streamPanel->setBounds (streamPage.getLocalBounds());
}

void MusicianView::refreshTalkMicDevices()
{
    talkMicDevices = StreamOutput::getInputDeviceNames();

    talkMicBox.clear (juce::dontSendNotification);
    talkMicBox.addItem ("(none)", 1);
    for (int i = 0; i < talkMicDevices.size(); ++i)
        talkMicBox.addItem (talkMicDevices[i], i + 2);

    const auto saved = settings::get ("streamTalkDevice", "").toString();
    const int index = talkMicDevices.indexOf (saved);
    talkMicBox.setSelectedItemIndex (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
}

//==============================================================================
// songs list
int MusicianView::getNumRows() { return downloads.getSongs().size(); }

void MusicianView::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto& songs = downloads.getSongs();
    if (! juce::isPositiveAndBelow (row, songs.size()))
        return;

    if (selected)
    {
        g.setColour (style::accentDark().withAlpha (0.5f));
        g.fillRect (0, 0, width, height);
    }

    const auto& song = songs.getReference (row);
    const int have = song.numDownloaded();
    const int total = song.stems.size();

    g.setColour (style::textPrimary());
    g.setFont (style::normalFont());
    g.drawText (song.name, 8, 0, width - 78, height, juce::Justification::centredLeft);

    g.setColour (song.isComplete() ? style::good() : style::warn());
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (juce::String (have) + "/" + juce::String (total),
                width - 70, 0, 62, height, juce::Justification::centredRight);
}

void MusicianView::selectedRowsChanged (int)
{
    // While a song is loaded the player (title + strips) belongs to it -
    // selecting another row must not steal the mix controls of what is playing.
    if (const auto* loadedSong = downloads.findSong (engine.getLoadedSongId()))
        songTitleLabel.setText (loadedSong->name, juce::dontSendNotification);
    else if (const auto* song = selectedSong())
        songTitleLabel.setText (song->name, juce::dontSendNotification);
    else
        songTitleLabel.setText ("No song selected", juce::dontSendNotification);

    updateSongButtons();
    rebuildStemStrips();
}

void MusicianView::updateSongButtons()
{
    const auto* song = selectedSong();
    const bool idle  = jamPhase == JamPhase::idle && ! downloadActive;

    downloadButton.setEnabled (song != nullptr && connection.isConnected()
                               && idle && ! song->isComplete());
    deleteButton.setEnabled (song != nullptr && idle && song->numDownloaded() > 0);
    loadButton.setEnabled (song != nullptr && song->isComplete() && idle);
}

const LocalSong* MusicianView::selectedSong() const
{
    const int row = songsList.getSelectedRow();
    const auto& songs = downloads.getSongs();
    return juce::isPositiveAndBelow (row, songs.size()) ? &songs.getReference (row) : nullptr;
}

std::vector<songloader::StemRequest> MusicianView::requestsFor (const LocalSong& song)
{
    std::vector<songloader::StemRequest> requests;
    for (const auto& stem : song.stems)
        requests.push_back ({ song.folder.getChildFile (stem.fileName), stem.name, stem.gainDb, stem.mute });
    return requests;
}

//==============================================================================
void MusicianView::toggleConnect()
{
    if (connection.isConnected())
    {
        connection.disconnect();
        handleDisconnected ("Disconnected.");
        return;
    }

    const auto name = nameEditor.getText().trim();
    if (name.isEmpty())
    {
        connStatusLabel.setText ("Please enter a name first.", juce::dontSendNotification);
        connStatusLabel.setColour (juce::Label::textColourId, style::warn());
        return;
    }

    const auto room = roomEditor.getText().trim().toUpperCase();

    connStatusLabel.setText (room.isNotEmpty() ? "Connecting via room code..." : "Connecting...",
                             juce::dontSendNotification);
    connStatusLabel.setColour (juce::Label::textColourId, style::textDim());

    if (room.isNotEmpty())
    {
        juce::String relayHost;
        int relayPort = relay::kDefaultPort;
        relay::parseAddress (settings::get ("relayAddress", juce::String (relay::kDefaultAddress)).toString(),
                             relayHost, relayPort);
        connection.connectViaRelay (relayHost, relayPort, room, name);
    }
    else
    {
        connection.connectTo (hostEditor.getText().trim(),
                              juce::jlimit (1, 65535, portEditor.getText().getIntValue()),
                              name);
    }
}

void MusicianView::handleDisconnected (const juce::String& reason)
{
    ++loadGeneration;
    stopVoice();
    chatPanel.setTalkAvailable (false);
    chatPanel.setStatusText ("Connect to a host to chat.");
    chatPanel.addSystemMessage (reason);
    engine.stopJam();
    engine.stopMonitor();
    jamPhase = JamPhase::idle;
    countdownLabel.setVisible (false);
    downloadQueue.clear();
    downloadActive = false;

    connStatusLabel.setText (reason, juce::dontSendNotification);
    connStatusLabel.setColour (juce::Label::textColourId, style::warn());
    connectButton.setButtonText ("Connect");
    downloadStatusLabel.setText ({}, juce::dontSendNotification);
    jamStatusLabel.setText ("No jam active.", juce::dontSendNotification);
    updateTransportButtons();
    selectedRowsChanged (0);
}

//==============================================================================
void MusicianView::startVoice()
{
    const auto talkMic = settings::get ("streamTalkDevice", "").toString();
    if (voice.isRunning() && voice.getTalkMicName() == talkMic)
        return;

    juce::String error;
    if (! voice.start (talkMic, error) && talkMic.isNotEmpty())
        voice.start ({}, error);   // mic busy/gone - at least keep listening
}

void MusicianView::stopVoice()
{
    voice.setTalking (false);
    voice.stop();
    chatPanel.setTalkActive (false);
}

void MusicianView::handleRecordingOffer (const juce::var& offer)
{
    const auto recId = offer.getProperty ("recId", juce::String()).toString();
    const auto song  = offer.getProperty ("song", recId).toString();
    const auto bytes = (juce::int64) offer.getProperty ("totalBytes", 0);
    if (recId.isEmpty())
        return;

    // With the auto-receive checkbox on, accept silently.
    if ((bool) settings::get ("autoReceiveRecordings", false))
    {
        connection.sendRecordingAnswer (recId, true);
        if (recordingsPanel != nullptr)
            recordingsPanel->showStatus ("Receiving \"" + song + "\"...");
        return;
    }

    const auto sizeText = juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
    juce::Component::SafePointer<MusicianView> safe (this);

    juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
        "Receive recording?",
        "The host wants to send you the jam recording\n\""
            + song + "\" (" + sizeText + ", all stems).\n\nReceive it now?",
        this, juce::ModalCallbackFunction::create ([safe, recId, song] (int okPressed)
    {
        if (safe == nullptr)
            return;

        safe->connection.sendRecordingAnswer (recId, okPressed != 0);
        if (okPressed != 0 && safe->recordingsPanel != nullptr)
            safe->recordingsPanel->showStatus ("Receiving \"" + song + "\"...");
    }));
}

//==============================================================================
void MusicianView::downloadClicked()
{
    const auto* song = selectedSong();
    if (song == nullptr || downloadActive)
        return;

    downloadQueue.clear();
    for (const auto& stem : song->stems)
        if (! stem.isDownloaded (song->folder))
            downloadQueue.add ({ song->id, stem.id, stem.name });

    if (downloadQueue.isEmpty())
        return;

    downloadActive = true;
    updateSongButtons();
    startNextDownload();
}

void MusicianView::deleteClicked()
{
    const auto* song = selectedSong();
    if (song == nullptr || downloadActive || jamPhase != JamPhase::idle)
        return;

    const auto songId = song->id;
    const auto name   = song->name;

    if (engine.getLoadedSongId() == songId)
    {
        engine.unload();
        songTitleLabel.setText ("No song selected", juce::dontSendNotification);
    }

    downloads.deleteLocal (songId);
    if (! lastSongListJson.isVoid())
        downloads.syncFromHostList (lastSongListJson);   // keep the host's entry (files gone)

    downloadStatusLabel.setText ("\"" + name + "\" deleted locally.", juce::dontSendNotification);
    songsList.updateContent();
    songsList.repaint();
    selectedRowsChanged (0);
    updateTransportButtons();
}

void MusicianView::startNextDownload()
{
    if (downloadQueue.isEmpty())
    {
        downloadActive = false;
        downloadStatusLabel.setText ("Download finished.", juce::dontSendNotification);
        songsList.repaint();
        updateSongButtons();

        if (jamPhase == JamPhase::downloading)
            continueJamPreparation();
        return;
    }

    const auto next = downloadQueue.removeAndReturn (0);
    downloadStatusLabel.setText ("Downloading " + next.stemName + " ...", juce::dontSendNotification);

    const auto dest = downloads.stemDestFile (next.songId, next.stemId);
    if (dest == juce::File() || ! connection.requestStem (next.songId, next.stemId, dest))
    {
        downloadActive = false;
        downloadQueue.clear();
        downloadStatusLabel.setText ("Download failed.", juce::dontSendNotification);
        if (jamPhase == JamPhase::downloading)
            failJamPreparation ("Download failed");
    }
}

void MusicianView::handleStemEnd (const juce::String&, const juce::String&,
                                  bool ok, const juce::String& error)
{
    if (! ok)
    {
        downloadActive = false;
        downloadQueue.clear();
        downloadStatusLabel.setText ("Error: " + error, juce::dontSendNotification);
        songsList.repaint();
        updateSongButtons();
        if (jamPhase == JamPhase::downloading)
            failJamPreparation ("Download error: " + error);
        return;
    }

    songsList.repaint();
    startNextDownload();
}

//==============================================================================
void MusicianView::rebuildStemStrips()
{
    stemStrips.clear();
    stemStripIds.clear();

    // The strips control what you hear: the loaded song while one is loaded
    // (regardless of the list selection), otherwise the selected song.
    const auto* song = downloads.findSong (engine.getLoadedSongId());
    if (song == nullptr)
        song = selectedSong();

    if (song == nullptr)
    {
        layoutStemStrips();
        return;
    }

    const auto songId = song->id;
    for (int i = 0; i < song->stems.size(); ++i)
    {
        const auto& stem = song->stems.getReference (i);
        auto* strip = new ChannelStrip (stem.name);
        strip->setValues (stem.gainDb, stem.mute);
        strip->setInfoText (stem.isDownloaded (song->folder) ? "ok" : "missing");

        const auto stemId = stem.id;
        const int index = i;

        auto currentMix = [this, songId, stemId]() -> LocalStem
        {
            if (const auto* s = downloads.findSong (songId))
                for (const auto& st : s->stems)
                    if (st.id == stemId)
                        return st;
            return {};
        };

        strip->onGain = [this, songId, stemId, index, currentMix] (float db)
        {
            downloads.setStemMix (songId, stemId, db, currentMix().mute);
            if (engine.getLoadedSongId() == songId)
                engine.setStemGainDb (index, db);
        };
        strip->onMute = [this, songId, stemId, index, currentMix] (bool mute)
        {
            downloads.setStemMix (songId, stemId, currentMix().gainDb, mute);
            if (engine.getLoadedSongId() == songId)
                engine.setStemMute (index, mute);
        };

        stemsContainer.addAndMakeVisible (strip);
        stemStrips.add (strip);
        stemStripIds.add (stem.id);
    }

    layoutStemStrips();
}

void MusicianView::layoutStemStrips()
{
    const int rowHeight = 34;
    const int width = stemsViewport.getWidth()
                      - (stemStrips.size() * rowHeight > stemsViewport.getHeight() ? 12 : 0);

    stemsContainer.setSize (juce::jmax (0, width), juce::jmax (0, stemStrips.size() * rowHeight));

    int y = 0;
    for (auto* strip : stemStrips)
    {
        strip->setBounds (0, y, stemsContainer.getWidth(), rowHeight - 4);
        y += rowHeight;
    }
}

void MusicianView::loadSelectedSong()
{
    const auto* song = selectedSong();
    if (song == nullptr || jamPhase != JamPhase::idle)
        return;

    const double targetRate = engine.getDeviceSampleRate();
    if (targetRate <= 0.0)
    {
        songTitleLabel.setText ("No audio device active.", juce::dontSendNotification);
        return;
    }

    songTitleLabel.setText ("Loading \"" + song->name + "\" ...", juce::dontSendNotification);
    loadButton.setEnabled (false);

    const int generation = ++loadGeneration;
    const auto meta = *song;
    juce::Component::SafePointer<MusicianView> safe (this);

    songloader::decodeAsync (requestsFor (*song), targetRate,
                             [safe, generation, meta] (std::shared_ptr<songloader::Result> result)
    {
        if (safe == nullptr || generation != safe->loadGeneration)
            return;

        juce::String error = result->ok ? juce::String() : result->error;
        if (error.isEmpty())
            safe->engine.adoptSong (meta, std::move (result->stems), error);

        if (error.isEmpty())
        {
            safe->songTitleLabel.setText (meta.name, juce::dontSendNotification);
            safe->positionSlider.setRange (0.0, juce::jmax (0.1, safe->engine.getLengthSeconds()), 0.01);
        }
        else
        {
            safe->songTitleLabel.setText (error, juce::dontSendNotification);
        }
        safe->rebuildStemStrips();   // the strips now control the loaded song
        safe->updateSongButtons();
        safe->updateTransportButtons();
    });
}

void MusicianView::updateTransportButtons()
{
    const bool loaded  = engine.isLoaded();
    const bool preview = loaded
                         && jamPhase != JamPhase::countdown
                         && jamPhase != JamPhase::running;
    playButton.setButtonText (engine.isPreviewPlaying() ? "Pause" : "Play");
    playButton.setEnabled (preview);
    stopButton.setEnabled (preview);
    positionSlider.setEnabled (preview);
}

//==============================================================================
void MusicianView::handleJamPrepare (const juce::var& json)
{
    jamId       = json.getProperty ("jamId", juce::String()).toString();
    jamSongId   = json.getProperty ("songId", juce::String()).toString();
    jamSongName = json.getProperty ("songName", "Song").toString();

    ++loadGeneration;
    engine.previewStop();
    engine.stopMonitor();
    engine.stopJam();
    countdownLabel.setVisible (false);
    jamPhase = JamPhase::downloading;
    lastStatusSentMs = 0;

    // Select the jam song so the strips/preview show the right stems.
    const auto& songs = downloads.getSongs();
    for (int i = 0; i < songs.size(); ++i)
        if (songs.getReference (i).id == jamSongId)
            { songsList.selectRow (i); break; }

    jamStatusLabel.setText ("Jam \"" + jamSongName + "\": checking song...", juce::dontSendNotification);
    updateTransportButtons();
    updateSongButtons();
    continueJamPreparation();
}

void MusicianView::continueJamPreparation()
{
    if (jamPhase != JamPhase::downloading)
        return;
    if (downloadActive)
        return;   // startNextDownload() re-calls us when the queue is empty

    const auto* song = downloads.findSong (jamSongId);
    if (song == nullptr)
    {
        failJamPreparation ("Song not in the list (song list out of date?)");
        return;
    }

    if (song->isComplete())
    {
        beginJamRamLoad();
        return;
    }

    // Auto-download the missing stems.
    downloadQueue.clear();
    for (const auto& stem : song->stems)
        if (! stem.isDownloaded (song->folder))
            downloadQueue.add ({ song->id, stem.id, stem.name });

    if (downloadQueue.isEmpty())
    {
        failJamPreparation ("Stems incomplete");
        return;
    }

    downloadActive = true;
    sendPrepareStatus ("downloading", jamDownloadPercent (0.0), {});
    updateSongButtons();
    startNextDownload();
}

void MusicianView::beginJamRamLoad()
{
    jamPhase = JamPhase::loading;
    sendPrepareStatus ("loading", 100, {});
    jamStatusLabel.setText ("Loading \"" + jamSongName + "\" into memory...", juce::dontSendNotification);

    const auto* song = downloads.findSong (jamSongId);
    if (song == nullptr)
    {
        failJamPreparation ("Song disappeared");
        return;
    }

    const double targetRate = engine.getDeviceSampleRate();
    if (targetRate <= 0.0)
    {
        failJamPreparation ("No audio device active");
        return;
    }

    const int generation = ++loadGeneration;
    const auto meta = *song;
    juce::Component::SafePointer<MusicianView> safe (this);

    songloader::decodeAsync (requestsFor (*song), targetRate,
                             [safe, generation, meta] (std::shared_ptr<songloader::Result> result)
    {
        if (safe == nullptr || generation != safe->loadGeneration
            || safe->jamPhase != JamPhase::loading)
            return;

        juce::String error = result->ok ? juce::String() : result->error;
        if (error.isEmpty())
            safe->engine.adoptSong (meta, std::move (result->stems), error);

        if (error.isNotEmpty())
        {
            safe->failJamPreparation (error);
            return;
        }

        safe->jamPhase = JamPhase::ready;
        safe->sendPrepareStatus ("ready", 100, {});

        // Soundcheck: stream the input to the host so it can set our level.
        juce::String monitorError;
        safe->engine.startMonitor (monitorError);

        safe->songTitleLabel.setText (meta.name, juce::dontSendNotification);
        safe->positionSlider.setRange (0.0, juce::jmax (0.1, safe->engine.getLengthSeconds()), 0.01);
        safe->jamStatusLabel.setText ("Ready! Preview & set your mix - the host starts the jam.",
                                      juce::dontSendNotification);
        safe->rebuildStemStrips();
        safe->updateTransportButtons();
        safe->updateSongButtons();
    });
}

void MusicianView::failJamPreparation (const juce::String& error)
{
    sendPrepareStatus ("error", 0, error);
    jamPhase = JamPhase::idle;
    jamStatusLabel.setText ("Jam preparation failed: " + error, juce::dontSendNotification);
    updateTransportButtons();
    updateSongButtons();
}

int MusicianView::jamDownloadPercent (double currentStemFraction) const
{
    const auto* song = downloads.findSong (jamSongId);
    if (song == nullptr || song->stems.isEmpty())
        return 0;

    const double done = (double) song->numDownloaded() + juce::jlimit (0.0, 1.0, currentStemFraction);
    return (int) juce::jlimit (0.0, 100.0, done * 100.0 / (double) song->stems.size());
}

void MusicianView::sendPrepareStatus (const juce::String& state, int percent, const juce::String& error)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("jamId", jamId);
    obj->setProperty ("state", state);
    obj->setProperty ("percent", percent);
    if (error.isNotEmpty()) obj->setProperty ("error", error);
    connection.sendJson (MsgType::prepareStatus, juce::var (obj));
}

void MusicianView::handleJamGo()
{
    countdownLabel.setVisible (false);

    juce::String error;
    if (engine.startJam (error))   // stops the monitor internally
    {
        jamPhase = JamPhase::running;
        jamStatusLabel.setText ("JAM RUNNING - play along to \"" + jamSongName + "\"!", juce::dontSendNotification);
    }
    else
    {
        jamPhase = JamPhase::idle;
        jamStatusLabel.setText ("Start failed: " + error, juce::dontSendNotification);
    }
    updateTransportButtons();
    updateSongButtons();
}

void MusicianView::handleJamStop (const juce::String& reason)
{
    ++loadGeneration;
    engine.stopJam();
    engine.stopMonitor();
    jamPhase = JamPhase::idle;
    countdownLabel.setVisible (false);
    backingMeter.setLevel (0.0f);
    micMeter.setLevel (0.0f);

    jamStatusLabel.setText (reason.isNotEmpty() ? "Jam ended: " + reason : "Jam ended.",
                            juce::dontSendNotification);
    updateTransportButtons();
    updateSongButtons();
}

//==============================================================================
void MusicianView::refreshLatencyLabels()
{
    const auto type = engine.getDeviceTypeName();
    const auto name = engine.getDeviceName();

    juce::StringArray lines;
    lines.add ("Driver: " + (type.isEmpty() ? "-" : type)
               + (name.isNotEmpty() ? "  (" + name + ")" : juce::String()));
    lines.add ("Reported latency (in+out): " + juce::String (engine.getReportedLatencySamples())
               + " samples = " + juce::String (engine.getReportedLatencyMs(), 1) + " ms");
    lines.add ("Manual offset: " + juce::String (PlayCaptureEngine::getManualOffsetMs(), 1) + " ms");
    latencyLabel.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);

    if (engine.isUsingAsio())
    {
        asioHintLabel.setText ("ASIO active - exact latency values, perfect.", juce::dontSendNotification);
        asioHintLabel.setColour (juce::Label::textColourId, style::good());
    }
    else
    {
        asioHintLabel.setText ("Tip: an ASIO driver reports exact latency values - important so your "
                               "recording lands perfectly in time at the host. Without ASIO, use the "
                               "manual offset if needed.",
                               juce::dontSendNotification);
        asioHintLabel.setColour (juce::Label::textColourId, style::warn());
    }
}

//==============================================================================
void MusicianView::refreshInstrumentUi()
{
    const bool loaded = engine.hasInstrument();

    instStatusLabel.setText (loaded
                                 ? "Loaded: " + engine.getInstrumentName()
                                   + " (replaces the hardware input as your signal)"
                                 : "No plugin - the hardware input is used.",
                             juce::dontSendNotification);
    instStatusLabel.setColour (juce::Label::textColourId, loaded ? style::good() : style::textDim());
    instUiButton.setEnabled (loaded);
    instRemoveButton.setEnabled (loaded);
}

void MusicianView::refreshMidiInputs()
{
    midiInputs = PlayCaptureEngine::getMidiInputs();

    midiInputBox.clear (juce::dontSendNotification);
    midiInputBox.addItem ("(none)", 1);
    for (int i = 0; i < midiInputs.size(); ++i)
        midiInputBox.addItem (midiInputs.getReference (i).name, i + 2);

    const auto saved = engine.getMidiInputIdentifier().isNotEmpty()
                           ? engine.getMidiInputIdentifier()
                           : settings::get ("midiInput", "").toString();

    int selected = 0;
    for (int i = 0; i < midiInputs.size(); ++i)
        if (midiInputs.getReference (i).identifier == saved)
            { selected = i + 1; break; }

    midiInputBox.setSelectedItemIndex (selected, juce::dontSendNotification);

    // Always (re)open the port: a rescan is also the retry after another
    // program released it.
    engine.setMidiInput (selected > 0 ? saved : juce::String());
}

void MusicianView::loadInstrumentClicked()
{
    const auto defaultFolder = juce::File ("C:\\Program Files\\Common Files\\VST3");

    instChooser = std::make_unique<juce::FileChooser> (
        "Choose a VST3 instrument (e.g. Superior Drummer 3)",
        defaultFolder.isDirectory() ? defaultFolder
                                    : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.vst3");

    juce::Component::SafePointer<MusicianView> safe (this);
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
void MusicianView::timerCallback()
{
    if (jamPhase == JamPhase::countdown)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const int remaining = now < countdownDeadlineMs
                                  ? (int) ((countdownDeadlineMs - now + 999) / 1000) : 0;
        countdownLabel.setText (remaining > 0 ? juce::String (remaining) : juce::String ("GO!"),
                                juce::dontSendNotification);
    }

    if (engine.hasInstrument() || engine.isLoaded())
        micMeter.setLevel (engine.getMicLevel());

    // Live MIDI diagnostics so a dead port is visible immediately.
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

    if (! engine.isLoaded())
        return;

    backingMeter.setLevel (engine.getBackingLevel());

    if (jamPhase == JamPhase::running)
    {
        jamStatusLabel.setText ("JAM RUNNING - " + formatTime (engine.getPositionSeconds())
                                + " / " + formatTime (engine.getLengthSeconds()),
                                juce::dontSendNotification);
    }
    else if (jamPhase == JamPhase::idle || jamPhase == JamPhase::ready)
    {
        if (! positionSlider.isMouseButtonDown())
            positionSlider.setValue (engine.getPositionSeconds(), juce::dontSendNotification);
        timeLabel.setText (formatTime (engine.getPositionSeconds()) + " / "
                           + formatTime (engine.getLengthSeconds()), juce::dontSendNotification);
        playButton.setButtonText (engine.isPreviewPlaying() ? "Pause" : "Play");
    }
}

juce::String MusicianView::formatTime (double seconds)
{
    if (seconds < 0.0 || std::isnan (seconds))
        seconds = 0.0;
    const int total = (int) seconds;
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

} // namespace bandjam
