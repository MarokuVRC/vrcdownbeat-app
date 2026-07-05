#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/RootComponent.h"
#include "ui/Style.h"

namespace bandjam
{
class BandJamApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "VRC Downbeat"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return true; } // Host + Musiker auf einem PC testen

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : juce::DocumentWindow (name, style::background(), allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new RootComponent(), true);
            setResizable (true, true);
            setResizeLimits (960, 640, 10000, 10000);
            centreWithSize (1400, 900);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    BandJamLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace bandjam

START_JUCE_APPLICATION (bandjam::BandJamApplication)
