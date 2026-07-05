#include "AudioStreamPanel.h"
#include "ui/Style.h"
#include "common/Settings.h"
#include "common/VbCableInstaller.h"

namespace bandjam
{
namespace
{
    /** Destination pill: a small independent toggle that lights up when the
        source goes to that destination. */
    class DestButton : public juce::Button
    {
    public:
        explicit DestButton (const juce::String& text, juce::Colour onColourToUse)
            : juce::Button (text), onColour (onColourToUse) {}

        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            const bool on = getToggleState();

            g.setColour (on ? onColour.withAlpha (0.28f)
                            : style::field().brighter (highlighted && isEnabled() ? 0.10f : 0.0f));
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (on ? onColour : style::panelOutline());
            g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, on ? 1.6f : 1.0f);

            g.setColour (! isEnabled() ? style::textDim().withAlpha (0.55f)
                                       : on ? style::textPrimary() : style::textDim());
            g.setFont (juce::Font (juce::FontOptions (12.5f, on ? juce::Font::bold : juce::Font::plain)));
            g.drawText (getButtonText(), r, juce::Justification::centred);
        }

    private:
        juce::Colour onColour;
    };
}

//==============================================================================
/** One row of the routing board: a sound source with meter (apps also get
    volume + mute) and the two destination toggles ("You" / "VRChat mic"). */
class AudioStreamPanel::SourceRow : public juce::Component
{
public:
    // Builtin (BandJam) source row.
    SourceRow (AudioStreamPanel& ownerToUse, int builtinIndexToUse)
        : owner (ownerToUse), builtinIndex (builtinIndexToUse)
    {
        const auto& source = owner.options.sources[(size_t) builtinIndex];
        init (source.name, source.subtitle);

        volumeSlider.setVisible (false);
        muteButton.setVisible (false);

        micButton.setToggleState (source.getToMic(), juce::dontSendNotification);
        micButton.onClick = [this]
        {
            auto& src = owner.options.sources[(size_t) builtinIndex];
            const bool newState = ! micButton.getToggleState();
            micButton.setToggleState (newState, juce::dontSendNotification);
            src.setToMic (newState);
        };

        if (source.youControlsWindowsListen)
        {
            youButton.setToggleState (owner.talkListenEnabled, juce::dontSendNotification);
            youButton.onClick = [this]
            {
                owner.setTalkListen (! youButton.getToggleState());
                youButton.setToggleState (owner.talkListenEnabled, juce::dontSendNotification);
            };
        }
        else if (source.getToYou && source.setToYou)
        {
            youButton.setToggleState (source.getToYou(), juce::dontSendNotification);
            youButton.onClick = [this]
            {
                auto& src = owner.options.sources[(size_t) builtinIndex];
                const bool newState = ! youButton.getToggleState();
                youButton.setToggleState (newState, juce::dontSendNotification);
                src.setToYou (newState);
            };
        }
        else
        {
            youButton.setToggleState (source.youFixedValue, juce::dontSendNotification);
            youButton.setEnabled (false);
            if (source.youFixedHint.isNotEmpty())
                youButton.setTooltip (source.youFixedHint);
        }
    }

    // Windows app row.
    SourceRow (AudioStreamPanel& ownerToUse, const AudioAppInfo& app,
               const juce::String& subtitle, bool youOn, bool micOn, bool controlsEnabled)
        : owner (ownerToUse), builtinIndex (-1), pid (app.pid), exeName (app.name)
    {
        init (app.name, subtitle);

        volumeSlider.setValue (app.volume, juce::dontSendNotification);
        volumeSlider.onValueChange = [this]
        {
            owner.router.setVolume (pid, (float) volumeSlider.getValue());
        };

        muteButton.setToggleState (app.mute, juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            owner.router.setMute (pid, muteButton.getToggleState());
        };

        youButton.setToggleState (youOn, juce::dontSendNotification);
        micButton.setToggleState (micOn, juce::dontSendNotification);
        youButton.setEnabled (controlsEnabled);
        micButton.setEnabled (controlsEnabled);

        youButton.onClick = [this]
        {
            owner.appYouToggled (pid, exeName, ! youButton.getToggleState(),
                                 micButton.getToggleState());
        };
        micButton.onClick = [this]
        {
            owner.appMicToggled (pid, exeName, ! micButton.getToggleState(),
                                 youButton.getToggleState());
        };
    }

    void updateLive()
    {
        if (builtinIndex >= 0)
        {
            const auto& source = owner.options.sources[(size_t) builtinIndex];
            if (source.getLevel)
                meter.setLevel (source.getLevel());
            if (source.makeSubtitle)
            {
                auto text = source.makeSubtitle();
                if (text != subtitleLabel.getText())
                    subtitleLabel.setText (text, juce::dontSendNotification);
            }

            // The engine state can change elsewhere (e.g. the "My Instrument"
            // strip in the Session tab) - keep the switches in sync.
            micButton.setToggleState (source.getToMic(), juce::dontSendNotification);
            if (source.youControlsWindowsListen)
                youButton.setToggleState (owner.talkListenEnabled, juce::dontSendNotification);
            else if (source.getToYou)
                youButton.setToggleState (source.getToYou(), juce::dontSendNotification);
        }
        else
        {
            meter.setLevel (owner.router.getPeak (pid));
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();

        micButton.setBounds (r.removeFromRight (110).reduced (4, 5));
        youButton.setBounds (r.removeFromRight (80).reduced (4, 5));
        r.removeFromRight (8);
        muteButton.setBounds (r.removeFromRight (30).reduced (0, 4));
        r.removeFromRight (4);
        volumeSlider.setBounds (r.removeFromRight (juce::jmin (150, r.getWidth() / 3)).reduced (0, 2));
        r.removeFromRight (8);
        meter.setBounds (r.removeFromRight (70).reduced (0, 10));
        r.removeFromRight (8);

        nameLabel.setBounds (r.removeFromTop (r.getHeight() * 55 / 100));
        subtitleLabel.setBounds (r);
    }

private:
    void init (const juce::String& name, const juce::String& subtitle)
    {
        nameLabel.setText (name, juce::dontSendNotification);
        nameLabel.setFont (style::normalFont());
        nameLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (nameLabel);

        subtitleLabel.setText (subtitle, juce::dontSendNotification);
        subtitleLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
        subtitleLabel.setColour (juce::Label::textColourId, style::textDim());
        subtitleLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (subtitleLabel);

        addAndMakeVisible (meter);

        volumeSlider.setRange (0.0, 1.0, 0.01);
        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible (volumeSlider);
        addAndMakeVisible (muteButton);
        addAndMakeVisible (youButton);
        addAndMakeVisible (micButton);
    }

    AudioStreamPanel& owner;
    const int          builtinIndex;   ///< -1 for app rows
    const juce::uint32 pid { 0 };
    const juce::String exeName;

    juce::Label  nameLabel, subtitleLabel;
    LevelMeter   meter;
    juce::Slider volumeSlider;
    MuteButton   muteButton;
    DestButton   youButton { "You", style::accent() },
                 micButton { "VRChat mic", style::good() };
};

//==============================================================================
AudioStreamPanel::AudioStreamPanel (StreamOutput& streamOutput, Options optionsToUse)
    : streamOut (streamOutput), options (std::move (optionsToUse))
{
    auto initCaption = [this] (juce::Label& label, const juce::String& text, float size = 13.0f)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, style::textDim());
        label.setFont (juce::Font (juce::FontOptions (size)));
        addAndMakeVisible (label);
    };

    // -- destination card: You ---------------------------------------------------------
    style::styleSectionLabel (youCardTitle, "What you hear");
    addAndMakeVisible (youCardTitle);

    youDeviceLabel.setFont (style::normalFont());
    youDeviceLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (youDeviceLabel);

    youHintLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    youHintLabel.setColour (juce::Label::textColourId, style::textDim());
    youHintLabel.setJustificationType (juce::Justification::topLeft);
    youHintLabel.setText ("Every source with \"You\" switched on plays here. Sources can go to "
                          "you AND the VRChat mic at the same time - your own talk mic never "
                          "comes back to you.", juce::dontSendNotification);
    addAndMakeVisible (youHintLabel);

    // -- destination card: VRChat mic ----------------------------------------------------
    style::styleSectionLabel (micCardTitle, "VRChat mic (virtual cable)");
    addAndMakeVisible (micCardTitle);

    initCaption (micDeviceCaption, "Cable:");

    micDeviceBox.onChange = [this] { applyStreamState(); };
    addAndMakeVisible (micDeviceBox);

    micTalkInfoLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    micTalkInfoLabel.setColour (juce::Label::textColourId, style::textDim());
    micTalkInfoLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (micTalkInfoLabel);

    cableInstallButton.setButtonText ("Install VB-CABLE");
    cableInstallButton.onClick = [this] { installCableClicked(); };
    addAndMakeVisible (cableInstallButton);

    addAndMakeVisible (micMeter);

    micStatusLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    micStatusLabel.setColour (juce::Label::textColourId, style::textDim());
    micStatusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (micStatusLabel);

    // -- source table --------------------------------------------------------------------
    style::styleSectionLabel (tableCaption, "Sound sources - pick who hears what");
    addAndMakeVisible (tableCaption);

    refreshButton.setButtonText ("Refresh");
    refreshButton.onClick = [this] { refreshAppTable (true); };
    addAndMakeVisible (refreshButton);

    soundSettingsButton.setButtonText ("Windows sound settings");
    soundSettingsButton.onClick = []
    {
        juce::Process::openDocument ("ms-settings:apps-volume", {});
    };
    addAndMakeVisible (soundSettingsButton);

    initCaption (headerSource, "Source", 12.0f);
    initCaption (headerLevel,  "Level", 12.0f);
    initCaption (headerVolume, "Volume", 12.0f);
    initCaption (headerYou,    "You", 12.0f);
    initCaption (headerMic,    "VRChat mic", 12.0f);

    routingWarnLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    routingWarnLabel.setColour (juce::Label::textColourId, style::warn());
    routingWarnLabel.setText ("Per-app routing is not available on this Windows version - "
                              "use the Windows sound settings instead.",
                              juce::dontSendNotification);
    addChildComponent (routingWarnLabel);
    routingWarnLabel.setVisible (! router.isRoutingSupported());

    rowsViewport.setViewedComponent (&rowsContainer, false);
    rowsViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (rowsViewport);

    // The old cable-wide monitor loopback ("Listen to this device") is
    // superseded by per-app captures; switch it off if this app enabled it
    // before, otherwise the user would hear their talk mic with no UI for it.
    for (const auto& endpoint : router.getCaptureEndpoints())
        if (endpoint.name.containsIgnoreCase ("CABLE Output"))
        {
            ListenState state;
            if (router.getListen (endpoint.id, state) && state.enabled)
            {
                state.enabled = false;
                juce::String ignored;
                router.setListen (endpoint.id, state, ignored);
            }
        }

    refreshStreamDevices (false);
    refreshTalkListen();
    refreshAppTable (true);
    syncCaptures();
    startTimerHz (30);   // meters run at full speed; slow work is divided down below
}

AudioStreamPanel::~AudioStreamPanel()
{
    stopTimer();
    captures.clear();   // stops the capture threads before streamOut users go away
}

//==============================================================================
void AudioStreamPanel::paint (juce::Graphics& g)
{
    style::drawPanel (g, youCardArea.toFloat());
    style::drawPanel (g, micCardArea.toFloat());
}

void AudioStreamPanel::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    // -- destination cards side by side ---------------------------------------------
    auto cardsArea = area.removeFromTop (118);
    auto youArea = cardsArea.removeFromLeft ((cardsArea.getWidth() - 12) * 2 / 5);
    cardsArea.removeFromLeft (12);
    youCardArea = youArea;
    micCardArea = cardsArea;

    {
        auto c = youArea.reduced (14, 10);
        youCardTitle.setBounds (c.removeFromTop (22));
        c.removeFromTop (4);
        youDeviceLabel.setBounds (c.removeFromTop (22));
        c.removeFromTop (4);
        youHintLabel.setBounds (c);
    }

    {
        auto c = micCardArea.reduced (14, 10);
        auto titleRow = c.removeFromTop (22);
        if (cableInstallButton.isVisible())
            cableInstallButton.setBounds (titleRow.removeFromRight (130).reduced (0, 1));
        titleRow.removeFromRight (6);
        micMeter.setBounds (titleRow.removeFromRight (90).reduced (0, 5));
        micCardTitle.setBounds (titleRow);
        c.removeFromTop (6);

        auto row = c.removeFromTop (24);
        micDeviceCaption.setBounds (row.removeFromLeft (46));
        micDeviceBox.setBounds (row.removeFromLeft (juce::jmin (280, row.getWidth() / 2 - 30)));
        row.removeFromLeft (12);
        micTalkInfoLabel.setBounds (row);
        c.removeFromTop (4);

        micStatusLabel.setBounds (c);
    }

    area.removeFromTop (12);

    // -- table caption + header --------------------------------------------------------
    auto captionRow = area.removeFromTop (26);
    soundSettingsButton.setBounds (captionRow.removeFromRight (180).reduced (0, 1));
    captionRow.removeFromRight (6);
    refreshButton.setBounds (captionRow.removeFromRight (86).reduced (0, 1));
    tableCaption.setBounds (captionRow);

    if (routingWarnLabel.isVisible())
        routingWarnLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (2);

    // Header labels mirror the row layout.
    auto header = area.removeFromTop (18);
    {
        auto h = header;
        headerMic.setBounds (h.removeFromRight (110).withTrimmedLeft (4));
        headerYou.setBounds (h.removeFromRight (80).withTrimmedLeft (4));
        h.removeFromRight (8 + 30 + 4);
        headerVolume.setBounds (h.removeFromRight (juce::jmin (150, h.getWidth() / 3)));
        h.removeFromRight (8);
        headerLevel.setBounds (h.removeFromRight (70));
        h.removeFromRight (8);
        headerSource.setBounds (h);
    }
    area.removeFromTop (2);

    rowsViewport.setBounds (area);

    const int rowHeight = 42;
    const int width = rowsViewport.getWidth()
                      - (rows.size() * rowHeight > rowsViewport.getHeight() ? 12 : 0);
    rowsContainer.setSize (juce::jmax (0, width), juce::jmax (0, rows.size() * rowHeight));

    int y = 0;
    for (auto* row : rows)
    {
        row->setBounds (0, y, rowsContainer.getWidth(), rowHeight - 6);
        y += rowHeight;
    }
}

//==============================================================================
void AudioStreamPanel::timerCallback()
{
    ++pollTick;

    for (auto* row : rows)
        row->updateLive();
    micMeter.setLevel (streamOut.getLevel());

    // App list refresh + capture re-arming every ~2 s.
    if (pollTick % 60 == 0)
    {
        refreshAppTable (false);
        syncCaptures();
    }

    // VB-CABLE installer: detect the new device and auto-select it.
    if (awaitingCableInstall && pollTick % 60 == 0)
    {
        if (vbcable::isInstalled())
        {
            awaitingCableInstall = false;
            refreshStreamDevices (true);
            setStatus ("VB-CABLE installed and selected! In VRChat, choose 'CABLE Output' "
                       "as your microphone.", style::good());
        }
        else if (juce::Time::getMillisecondCounter() > cableDeadlineMs)
        {
            awaitingCableInstall = false;
        }
    }

    // Newly installed/plugged devices must show up without an app restart:
    // rescan every ~5 s; also retry a selected-but-not-running stream
    // (transient open failure, e.g. right after a driver install).
    if (! awaitingCableInstall && pollTick % 150 == 0)
    {
        if (StreamOutput::getOutputDeviceNames() != streamDevices)
            refreshStreamDevices (false);
        else if (micDeviceBox.getSelectedItemIndex() > 0 && ! streamOut.isRunning())
            applyStreamState();

        refreshTalkListen();   // picks up changes made in the Windows sound panel
    }
}

//==============================================================================
juce::String AudioStreamPanel::selectedCableName() const
{
    const int index = micDeviceBox.getSelectedItemIndex();
    return juce::isPositiveAndBelow (index - 1, streamDevices.size())
               ? streamDevices[index - 1] : juce::String();
}

juce::String AudioStreamPanel::selectedCableEndpointId() const
{
    const auto cable = selectedCableName();
    for (const auto& endpoint : renderEndpoints)
        if (endpoint.name == cable)
            return endpoint.id;
    return {};
}

void AudioStreamPanel::refreshStreamDevices (bool autoSelectCable)
{
    streamDevices   = StreamOutput::getOutputDeviceNames();
    renderEndpoints = router.getRenderEndpoints();

    micDeviceBox.clear (juce::dontSendNotification);
    micDeviceBox.addItem ("(none)", 1);
    for (int i = 0; i < streamDevices.size(); ++i)
        micDeviceBox.addItem (streamDevices[i], i + 2);

    const auto savedOut = settings::get (options.outDeviceSettingsKey, "").toString();
    int selected = 0;

    if (savedOut.isNotEmpty())
    {
        const int index = streamDevices.indexOf (savedOut);
        if (index >= 0)
            selected = index + 1;
    }

    // No (valid) saved device: auto-pick the cable so it works out of the box.
    if (selected == 0 || autoSelectCable)
        for (int i = 0; i < streamDevices.size(); ++i)
            if (streamDevices[i].containsIgnoreCase ("CABLE Input"))
                { selected = i + 1; break; }

    micDeviceBox.setSelectedItemIndex (selected, juce::dontSendNotification);
    applyStreamState();

    cableInstallButton.setVisible (! vbcable::isInstalled());
    resized();
}

void AudioStreamPanel::applyStreamState()
{
    const auto device = selectedCableName();
    const auto talk   = settings::get (options.talkDeviceSettingsKey, "").toString();

    settings::set (options.outDeviceSettingsKey, device);

    micTalkInfoLabel.setText ("Talk mic: " + (talk.isNotEmpty() ? talk : juce::String ("(none)"))
                              + "  -  change it in the Audio tab",
                              juce::dontSendNotification);

    if (device.isEmpty())
    {
        streamOut.stop();
        setStatus ("Pick 'CABLE Input' (free VB-CABLE driver) as the cable. In VRChat, "
                   "select 'CABLE Output' as your microphone.", style::textDim());
    }
    else
    {
        juce::String error;
        if (streamOut.start (device, talk, error))
        {
            setStatus ("Streaming to \"" + device + "\". In VRChat, select 'CABLE Output' as "
                       "your mic. Use the switches below to pick what goes in.", style::good());
        }
        else
        {
            setStatus ("Could not open the device: " + error, style::warn());
        }
    }

    refreshAppTable (true);
    youDeviceLabel.setText (router.getDefaultRenderEndpoint().name, juce::dontSendNotification);
}

void AudioStreamPanel::talkDeviceChanged()
{
    applyStreamState();
    refreshTalkListen();
    refreshAppTable (true);   // the talk row's subtitle/toggles changed
}

//==============================================================================
// Talk-mic self monitoring: "You" on the talk row = Windows' "Listen to this
// device" on the talk mic, played on the default output. BandJam itself never
// mixes the talk mic into anything the user hears.

juce::String AudioStreamPanel::findTalkCaptureId() const
{
    const auto talk = settings::get (options.talkDeviceSettingsKey, "").toString();
    if (talk.isEmpty())
        return {};

    for (const auto& endpoint : captureEndpoints)
        if (endpoint.name == talk)
            return endpoint.id;

    return {};
}

void AudioStreamPanel::refreshTalkListen()
{
    captureEndpoints = router.getCaptureEndpoints();

    ListenState state;
    const auto captureId = findTalkCaptureId();
    talkListenEnabled = captureId.isNotEmpty()
                        && router.getListen (captureId, state) && state.enabled;
}

void AudioStreamPanel::setTalkListen (bool enabled)
{
    const auto captureId = findTalkCaptureId();
    if (captureId.isEmpty())
    {
        setStatus ("Select a talk mic in the Audio tab first.", style::textDim());
        return;
    }

    // Keep an already-chosen playback target; empty = default device.
    ListenState state;
    router.getListen (captureId, state);
    state.enabled = enabled;

    juce::String error;
    if (router.setListen (captureId, state, error))
        talkListenEnabled = enabled;
    else
        setStatus (error, style::warn());
}

void AudioStreamPanel::installCableClicked()
{
    if (vbcable::isInstalled())
    {
        refreshStreamDevices (true);
        return;
    }

    cableInstallButton.setEnabled (false);
    juce::Component::SafePointer<AudioStreamPanel> safe (this);

    vbcable::installAsync ([safe] (const juce::String& status, bool finished, bool ok)
    {
        if (safe == nullptr)
            return;

        safe->setStatus (status, ok ? style::textDim() : style::warn());

        if (finished)
        {
            safe->cableInstallButton.setEnabled (true);
            if (ok)
            {
                safe->awaitingCableInstall = true;
                safe->cableDeadlineMs = juce::Time::getMillisecondCounter() + 5 * 60 * 1000;
            }
        }
    });
}

//==============================================================================
// Loopback captures ("You + VRChat mic" apps)

juce::StringArray AudioStreamPanel::getCapturedAppNames() const
{
    juce::StringArray names;
    names.addTokens (settings::get (options.capturedAppsSettingsKey, "").toString(), "\n", "");
    names.removeEmptyStrings();
    return names;
}

void AudioStreamPanel::setCapturedAppNames (const juce::StringArray& names)
{
    settings::set (options.capturedAppsSettingsKey, names.joinIntoString ("\n"));
}

bool AudioStreamPanel::isCaptured (const juce::String& exeName) const
{
    return getCapturedAppNames().contains (exeName, true);
}

void AudioStreamPanel::startCapture (const juce::String& exeName, juce::uint32 pid)
{
    auto names = getCapturedAppNames();
    if (! names.contains (exeName, true))
    {
        names.add (exeName);
        setCapturedAppNames (names);
    }

    const int existing = captureNames.indexOf (exeName, true);
    if (existing >= 0)
    {
        if (captures[existing]->getPid() == pid)
            return;
        captures.remove (existing);
        captureNames.remove (existing);
    }

    captures.add (new AppCapture (pid, streamOut));
    captureNames.add (exeName);
}

void AudioStreamPanel::stopCapture (const juce::String& exeName)
{
    auto names = getCapturedAppNames();
    names.removeString (exeName, true);
    setCapturedAppNames (names);

    const int index = captureNames.indexOf (exeName, true);
    if (index >= 0)
    {
        captures.remove (index);
        captureNames.remove (index);
    }
}

void AudioStreamPanel::syncCaptures()
{
    const auto wanted = getCapturedAppNames();

    // Drop captures that are no longer wanted or whose app vanished/restarted.
    for (int i = captures.size(); --i >= 0;)
    {
        const auto* app = findAppByName (captureNames[i]);
        if (! wanted.contains (captureNames[i], true)
            || app == nullptr || app->pid != captures[i]->getPid())
        {
            captures.remove (i);
            captureNames.remove (i);
        }
    }

    // (Re-)arm captures for apps that are present again.
    for (const auto& name : wanted)
        if (captureNames.indexOf (name, true) < 0)
            if (const auto* app = findAppByName (name))
            {
                captures.add (new AppCapture (app->pid, streamOut));
                captureNames.add (name);
            }
}

const AudioAppInfo* AudioStreamPanel::findApp (juce::uint32 pid) const
{
    for (const auto& app : router.getApps())
        if (app.pid == pid)
            return &app;
    return nullptr;
}

const AudioAppInfo* AudioStreamPanel::findAppByName (const juce::String& exeName) const
{
    for (const auto& app : router.getApps())
        if (app.name.equalsIgnoreCase (exeName))
            return &app;
    return nullptr;
}

//==============================================================================
// App row clicks

bool AudioStreamPanel::routeApp (juce::uint32 pid, bool toCable)
{
    const auto cableId = selectedCableEndpointId();
    if (toCable && cableId.isEmpty())
    {
        setStatus ("Pick a cable device first (or install VB-CABLE).", style::warn());
        return false;
    }

    juce::String targetId = toCable ? cableId : juce::String();

    if (! toCable)
    {
        // If the program picked the cable itself (no per-app route set),
        // clearing the route wouldn't move it - route it to the current
        // default device explicitly instead.
        if (const auto* app = findApp (pid))
            if (app->routedDeviceId.isEmpty())
                targetId = router.getDefaultRenderEndpoint().id;
    }

    juce::String error;
    if (! router.routeAppToDevice (pid, targetId, error))
    {
        setStatus (error, style::warn());
        return false;
    }
    return true;
}

void AudioStreamPanel::appMicToggled (juce::uint32 pid, const juce::String& exeName,
                                      bool toMic, bool youOn)
{
    if (toMic)
    {
        if (youOn && AppCapture::isSupported())
        {
            // Keep it audible AND tap a copy into the cable.
            startCapture (exeName, pid);
        }
        else
        {
            // Only the mic should get it (or captures are unavailable on this
            // Windows version) - route the app into the cable.
            if (! AppCapture::isSupported() && youOn)
                setStatus ("This Windows version can't tap an app's audio - the app was "
                           "routed into the cable instead (you won't hear it).", style::warn());
            stopCapture (exeName);
            routeApp (pid, true);
        }
    }
    else
    {
        stopCapture (exeName);
        if (! youOn)
            routeApp (pid, false);   // was routed into the cable - bring it back
    }

    deferredRefresh();
}

void AudioStreamPanel::appYouToggled (juce::uint32 pid, const juce::String& exeName,
                                      bool toYou, bool micOn)
{
    if (toYou)
    {
        if (micOn)
        {
            // Routed into the cable -> back to the speakers, keep feeding the
            // mic via a loopback capture.
            if (routeApp (pid, false))
            {
                if (AppCapture::isSupported())
                    startCapture (exeName, pid);
                else
                    setStatus ("This Windows version can't tap an app's audio - the app is "
                               "audible again but no longer reaches the mic.", style::warn());
            }
        }
        // micOn == false can't happen (You off requires Mic on).
    }
    else
    {
        if (! micOn)
        {
            setStatus ("Switch on \"VRChat mic\" first, or use the mute button to silence "
                       "the app.", style::textDim());
            deferredRefresh();
            return;
        }

        // You off + Mic on = route the app into the cable (no capture needed).
        stopCapture (exeName);
        routeApp (pid, true);
    }

    deferredRefresh();
}

void AudioStreamPanel::deferredRefresh()
{
    // Rebuilding the rows would delete the clicked button while it is still
    // notifying - defer the refresh to the next message loop run.
    juce::Component::SafePointer<AudioStreamPanel> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->refreshAppTable (true);
    });
}

//==============================================================================
juce::String AudioStreamPanel::tableSignature() const
{
    juce::String signature;
    signature << selectedCableEndpointId() << "//" << getCapturedAppNames().joinIntoString (",") << "//";
    for (const auto& app : router.getApps())
        signature << app.pid << ":" << app.name << ":" << app.sessionDevices
                  << ":" << app.activeDevices << ":" << app.routedDeviceId << "|";
    return signature;
}

void AudioStreamPanel::refreshAppTable (bool force)
{
    renderEndpoints = router.getRenderEndpoints();
    router.refreshSessions();

    const auto signature = tableSignature();
    if (! force && signature == lastSignature)
        return;

    lastSignature = signature;
    rebuildRows();
}

void AudioStreamPanel::rebuildRows()
{
    rows.clear();

    // BandJam's own sources first.
    for (int i = 0; i < (int) options.sources.size(); ++i)
    {
        auto* row = new SourceRow (*this, i);
        rowsContainer.addAndMakeVisible (row);
        rows.add (row);
    }

    const auto cableId   = selectedCableEndpointId();
    const auto cableName = selectedCableName();
    const bool controlsEnabled = router.isRoutingSupported() && cableId.isNotEmpty();

    // Stable, alphabetical order - the rows shouldn't jump when a route changes.
    auto sortedApps = router.getApps();
    std::sort (sortedApps.begin(), sortedApps.end(),
               [] (const AudioAppInfo& a, const AudioAppInfo& b)
               { return a.name.compareIgnoreCase (b.name) < 0; });

    for (const auto& app : sortedApps)
    {
        // Routed into the cable via our per-app route, or actively playing on
        // it (the program picked the cable in its own output settings). Idle
        // leftover cable sessions don't count - without a route, the next
        // sound goes to the default device.
        const bool routedToCable = (cableId.isNotEmpty() && app.routedDeviceId == cableId)
                                   || (app.routedDeviceId.isEmpty() && cableName.isNotEmpty()
                                       && app.activeDevices.contains (cableName));
        const bool captured = isCaptured (app.name);

        const bool youOn = ! routedToCable;
        const bool micOn = routedToCable || captured;

        const auto subtitle = routedToCable ? juce::String ("goes to the VRChat mic only")
                            : captured      ? juce::String ("you hear it + it goes to the VRChat mic")
                            : app.activeDevices.isNotEmpty() ? "plays on " + app.activeDevices
                            : juce::String ("not playing right now");

        auto* row = new SourceRow (*this, app, subtitle, youOn, micOn, controlsEnabled);
        rowsContainer.addAndMakeVisible (row);
        rows.add (row);
    }

    resized();
}

void AudioStreamPanel::setStatus (const juce::String& text, juce::Colour colour)
{
    micStatusLabel.setColour (juce::Label::textColourId, colour);
    micStatusLabel.setText (text, juce::dontSendNotification);
}

} // namespace bandjam
