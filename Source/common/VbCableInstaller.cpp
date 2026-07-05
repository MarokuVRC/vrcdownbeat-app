#include "VbCableInstaller.h"
#include "StreamOutput.h"

namespace bandjam
{
namespace vbcable
{
namespace
{
    // Official VB-Audio download; the pack number changes rarely. If the URL
    // ever 404s we fall back to opening the website.
    constexpr const char* kZipUrl   = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";
    constexpr const char* kWebsite  = "https://vb-audio.com/Cable/";
}

bool isInstalled()
{
    for (const auto& name : StreamOutput::getOutputDeviceNames())
        if (name.containsIgnoreCase ("CABLE Input"))
            return true;
    return false;
}

void installAsync (std::function<void (const juce::String&, bool, bool)> onStatus)
{
    auto report = [onStatus] (const juce::String& text, bool finished, bool ok)
    {
        juce::MessageManager::callAsync ([onStatus, text, finished, ok]
        {
            if (onStatus) onStatus (text, finished, ok);
        });
    };

    juce::Thread::launch ([report]
    {
        auto failToWebsite = [&report] (const juce::String& why)
        {
            report (why + " Opening the VB-CABLE website instead - download and run the setup there.",
                    true, false);
            juce::URL (kWebsite).launchInDefaultBrowser();
        };

        report ("Downloading VB-CABLE from vb-audio.com ...", false, true);

        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (15000);
        auto stream = juce::URL (kZipUrl).createInputStream (options);
        if (stream == nullptr)
        {
            failToWebsite ("Download failed.");
            return;
        }

        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("bandjam-vbcable");
        tempDir.deleteRecursively();
        tempDir.createDirectory();
        const auto zipFile = tempDir.getChildFile ("vbcable.zip");

        {
            juce::FileOutputStream out (zipFile);
            if (! out.openedOk() || out.writeFromInputStream (*stream, -1) <= 0)
            {
                failToWebsite ("Download failed.");
                return;
            }
        }

        report ("Unpacking ...", false, true);

        juce::ZipFile zip (zipFile);
        if (zip.getNumEntries() == 0 || zip.uncompressTo (tempDir, true).failed())
        {
            failToWebsite ("The downloaded file could not be unpacked.");
            return;
        }

        auto candidates = tempDir.findChildFiles (juce::File::findFiles, true, "VBCABLE_Setup_x64.exe");
        if (candidates.isEmpty())
            candidates = tempDir.findChildFiles (juce::File::findFiles, true, "VBCABLE_Setup*.exe");
        if (candidates.isEmpty())
        {
            failToWebsite ("No setup program found in the download.");
            return;
        }

        if (! candidates.getReference (0).startAsProcess())
        {
            failToWebsite ("Could not start the installer.");
            return;
        }

        report ("Installer started: confirm the admin prompt and click 'Install Driver'. "
                "BandJam detects the cable automatically (if it never shows up, reboot Windows).",
                true, true);
    });
}

} // namespace vbcable
} // namespace bandjam
