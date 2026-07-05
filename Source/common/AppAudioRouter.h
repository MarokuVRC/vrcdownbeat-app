#pragma once

#include <juce_core/juce_core.h>
#include <memory>

namespace bandjam
{
/** A render (output) endpoint as Windows sees it. */
struct AudioAppEndpoint
{
    juce::String id;     ///< raw MMDevice id, e.g. "{0.0.0.00000000}.{guid}"
    juce::String name;   ///< friendly name, e.g. "CABLE Input (VB-Audio Virtual Cable)"
};

/** Windows "Listen to this device" state of a capture endpoint
    (mic properties -> Listen tab). */
struct ListenState
{
    bool enabled { false };
    juce::String playbackDeviceId;   ///< raw MMDevice id; empty = default playback device
};

/** One program that currently has an audio session (Windows volume mixer row). */
struct AudioAppInfo
{
    juce::uint32 pid { 0 };
    juce::String name;             ///< exe base name, e.g. "Spotify"
    juce::String sessionDevices;   ///< friendly name(s) of the device(s) it plays on
    juce::String activeDevices;    ///< names of devices with a currently ACTIVE session
    juce::String routedDeviceId;   ///< persisted per-app route ("" = system default)
    float        volume { 1.0f };  ///< per-app mixer volume 0..1
    bool         mute { false };
};

/** The Windows per-application audio mixer, in-app.

    Two Windows APIs power this:
      - IAudioSessionManager2 (documented): which programs are playing audio on
        which output device, with per-app volume/mute/peak level.
      - IAudioPolicyConfigFactory (undocumented but stable since Win10 1803;
        the same API the Settings page "App volume and device preferences" and
        EarTrumpet use): re-route a specific program to another output device,
        e.g. Spotify -> "CABLE Input" so VRChat hears it through the virtual mic.

    Message thread only. */
class AppAudioRouter
{
public:
    AppAudioRouter();
    ~AppAudioRouter();

    /** All active output devices (for the per-app device dropdown). */
    juce::Array<AudioAppEndpoint> getRenderEndpoints() const;

    /** The system default playback device (what "You" hear). */
    AudioAppEndpoint getDefaultRenderEndpoint() const;

    /** All active input devices (for the "Listen" section). */
    juce::Array<AudioAppEndpoint> getCaptureEndpoints() const;

    /** Reads the "Listen to this device" checkbox + playback target of a
        capture endpoint. Returns false if the device can't be queried. */
    bool getListen (const juce::String& captureDeviceId, ListenState& state) const;

    /** Writes the "Listen to this device" checkbox + playback target.
        Takes effect immediately (Windows starts/stops the loopback). */
    bool setListen (const juce::String& captureDeviceId, const ListenState& state,
                    juce::String& error);

    /** Rebuilds the app list (also refreshes the volume/mute/route snapshot). */
    void refreshSessions();
    const juce::Array<AudioAppInfo>& getApps() const noexcept { return apps; }

    /** Live peak meter (0..1) across all sessions of that app. */
    float getPeak (juce::uint32 pid) const;

    void setVolume (juce::uint32 pid, float volume0to1);
    void setMute (juce::uint32 pid, bool mute);

    /** Routes all audio of the process to the given output device
        (empty id = back to the system default). Takes effect immediately;
        Windows remembers it per exe (like the Settings app). */
    bool routeAppToDevice (juce::uint32 pid, const juce::String& deviceIdOrEmpty,
                           juce::String& error);

    /** True if per-app routing is available on this Windows build. */
    bool isRoutingSupported() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    juce::Array<AudioAppInfo> apps;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppAudioRouter)
};

} // namespace bandjam
