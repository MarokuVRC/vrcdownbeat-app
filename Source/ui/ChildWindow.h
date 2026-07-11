#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/Style.h"

namespace bandjam
{
/** A non-modal tool window (Audio, Audio Stream, Recordings, Test Port...)
    hosting a content component owned by the view. Closing only hides it, so
    it can stay open next to the main window while the user keeps working. */
class ChildWindow : public juce::DocumentWindow
{
public:
    ChildWindow (const juce::String& title, juce::Component& content, int width, int height)
        : juce::DocumentWindow (title, style::background(),
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setContentNonOwned (&content, false);
        centreWithSize (width, height);
    }

    ~ChildWindow() override
    {
        clearContentComponent();   // the view owns the content
    }

    void closeButtonPressed() override { setVisible (false); }

    /** Shows and raises the window (used by the menu items). */
    void open()
    {
        setVisible (true);
        toFront (true);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChildWindow)
};

} // namespace bandjam
