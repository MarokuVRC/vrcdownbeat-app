#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace bandjam
{
/** Plain tab-page container whose layout is delegated to the owning view
    (the views keep their layout code in one place). */
class LambdaPage : public juce::Component
{
public:
    std::function<void()> onLayout;

    void resized() override
    {
        if (onLayout)
            onLayout();
    }
};

} // namespace bandjam
