#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>

namespace bandjam
{
/** One-click install of VB-Audio's free VB-CABLE virtual audio device.

    The driver itself must come from VB-Audio (kernel drivers can't be created
    by an app, and bundling their installer needs a distribution license), so
    this downloads the official pack from vb-audio.com, unpacks it and launches
    the setup - the user only has to confirm the admin prompt and click
    "Install Driver". If anything fails, the download website is opened
    instead. */
namespace vbcable
{
    /** True if a "CABLE Input" playback device exists (VB-CABLE installed). */
    bool isInstalled();

    /** Downloads + launches the official installer on a background thread.
        onStatus fires on the message thread with progress text; finished=true
        marks the last message, ok=false means it fell back to the website. */
    void installAsync (std::function<void (const juce::String& status, bool finished, bool ok)> onStatus);
}

} // namespace bandjam
