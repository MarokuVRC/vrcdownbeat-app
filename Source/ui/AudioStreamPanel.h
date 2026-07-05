#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "common/StreamOutput.h"
#include "common/AppAudioRouter.h"
#include "common/AppCapture.h"
#include "ui/Meters.h"
#include <vector>

namespace bandjam
{
/** The "Audio Stream" tab, shared by host and musician: a routing board that
    shows every sound source as a row with two independent switches - "You"
    (do I hear it?) and "VRChat mic" (does it go into the virtual cable?).

    BandJam's own sources (song playback, instrument, talk mic) are supplied
    by the owning view as BuiltinSource descriptors; their switches drive the
    engine's include flags directly - the talk mic is never played back to
    the user, so there is no echo.

    Windows programs (Spotify, browsers, ...) appear automatically:
      - You only:            normal playback on the default device.
      - VRChat mic only:     the app is routed into the cable (Windows
                             per-app routing) - you no longer hear it.
      - You + VRChat mic:    the app keeps playing on your device and a
                             per-process loopback capture (Win10 2004+) taps
                             a copy into the cable - no monitor loopback, no
                             talk-mic echo.

    The panel owns all polling: device rescans, stream auto-retry, VB-CABLE
    install detection, capture re-arming and the app list refresh. */
class AudioStreamPanel : public juce::Component,
                         private juce::Timer
{
public:
    /** One of BandJam's own sound sources (a row in the table). */
    struct BuiltinSource
    {
        juce::String name;
        juce::String subtitle;                     ///< may be refreshed via makeSubtitle
        std::function<juce::String()> makeSubtitle; ///< optional live subtitle
        std::function<float()> getLevel;           ///< live meter (0..1)

        std::function<bool()>      getToMic;       ///< required
        std::function<void (bool)> setToMic;       ///< required (persists itself)

        // "You" column: leave the callbacks empty for a fixed state.
        std::function<bool()>      getToYou;
        std::function<void (bool)> setToYou;
        bool youFixedValue { true };
        juce::String youFixedHint;                 ///< e.g. "you never hear your own talk mic"

        /** Talk-mic rows: "You" toggles Windows' "Listen to this device" on
            the talk mic, so users can hear themselves if they want to. */
        bool youControlsWindowsListen { false };
    };

    struct Options
    {
        juce::String outDeviceSettingsKey;       ///< e.g. "hostStreamOutDevice"
        juce::String talkDeviceSettingsKey;      ///< read here; the combo lives in the Audio tab
        juce::String capturedAppsSettingsKey;    ///< e.g. "hostStreamCapturedApps"
        std::vector<BuiltinSource> sources;
    };

    AudioStreamPanel (StreamOutput& streamOutput, Options options);
    ~AudioStreamPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** The view changed the talk mic (Audio tab): reopen the stream device. */
    void talkDeviceChanged();

private:
    class SourceRow;
    friend class SourceRow;

    void timerCallback() override;

    // Stream (BandJam -> cable)
    void refreshStreamDevices (bool autoSelectCable);
    void applyStreamState();
    void installCableClicked();
    juce::String selectedCableName() const;
    juce::String selectedCableEndpointId() const;

    // Talk-mic self monitoring (Windows "Listen to this device")
    juce::String findTalkCaptureId() const;
    void refreshTalkListen();
    void setTalkListen (bool enabled);

    // App table
    void refreshAppTable (bool force);
    void rebuildRows();
    juce::String tableSignature() const;
    const AudioAppInfo* findApp (juce::uint32 pid) const;
    const AudioAppInfo* findAppByName (const juce::String& exeName) const;

    // App row clicks
    void appMicToggled (juce::uint32 pid, const juce::String& exeName, bool toMic, bool youOn);
    void appYouToggled (juce::uint32 pid, const juce::String& exeName, bool toYou, bool micOn);
    bool routeApp (juce::uint32 pid, bool toCable);
    void deferredRefresh();

    // Loopback captures ("You + VRChat mic" apps)
    bool isCaptured (const juce::String& exeName) const;
    void startCapture (const juce::String& exeName, juce::uint32 pid);
    void stopCapture (const juce::String& exeName);
    void syncCaptures();
    juce::StringArray getCapturedAppNames() const;
    void setCapturedAppNames (const juce::StringArray& names);

    void setStatus (const juce::String& text, juce::Colour colour);

    StreamOutput& streamOut;
    const Options options;
    AppAudioRouter router;

    // -- destination cards --------------------------------------------------------
    juce::Rectangle<int> youCardArea, micCardArea;   // painted panels
    juce::Label youCardTitle, youDeviceLabel, youHintLabel;

    juce::Label      micCardTitle, micDeviceCaption, micStatusLabel, micTalkInfoLabel;
    juce::ComboBox   micDeviceBox;
    juce::TextButton cableInstallButton;
    LevelMeter       micMeter;

    juce::StringArray streamDevices;
    juce::Array<AudioAppEndpoint> renderEndpoints, captureEndpoints;
    bool         talkListenEnabled { false };
    bool         awaitingCableInstall { false };
    juce::uint32 cableDeadlineMs { 0 };
    int          pollTick { 0 };

    // -- source table -------------------------------------------------------------
    juce::Label      tableCaption, headerSource, headerLevel, headerVolume, headerYou, headerMic;
    juce::Label      routingWarnLabel;
    juce::TextButton refreshButton, soundSettingsButton;
    juce::Viewport   rowsViewport;
    juce::Component  rowsContainer;
    juce::OwnedArray<SourceRow> rows;
    juce::String     lastSignature;

    // exe name -> running capture (pid can change between sessions)
    juce::OwnedArray<AppCapture> captures;
    juce::StringArray captureNames;   ///< parallel to 'captures'

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioStreamPanel)
};

} // namespace bandjam
