#pragma once

#include <juce_core/juce_core.h>
#include "common/StreamOutput.h"
#include <memory>

namespace bandjam
{
/** Captures the audio of one program (and its child processes) with Windows'
    per-process loopback capture (Win10 2004+, the API behind OBS' "Application
    Audio Capture") and mixes it into the StreamOutput as an aux source.

    This is what makes "You + VRChat mic" possible at the same time: the app
    keeps playing on the speakers, BandJam taps a copy into the virtual cable.
    Nothing is routed, so the user's talk mic never loops back to them.

    The capture runs on its own thread; construction just starts it (check
    isActive() a moment later to see whether Windows granted the capture). */
class AppCapture
{
public:
    AppCapture (juce::uint32 pid, StreamOutput& streamOutput);
    ~AppCapture();

    juce::uint32 getPid() const noexcept { return pid; }

    /** True once frames are flowing; false if activation failed. */
    bool isActive() const noexcept;

    /** True on Windows builds that support per-process loopback. */
    static bool isSupported();

private:
    const juce::uint32 pid;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppCapture)
};

} // namespace bandjam
