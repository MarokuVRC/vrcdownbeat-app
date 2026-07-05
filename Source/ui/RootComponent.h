#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace bandjam
{
class HostView;
class MusicianView;

/** Top-level view: shows the mode picker (Host / Musiker) and swaps in the
    chosen mode's view. Each mode view offers a "Verlassen" action to get back
    here (only enabled while nothing is running). */
class RootComponent : public juce::Component
{
public:
    RootComponent();
    ~RootComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void showModeSelect();
    void showHost();
    void showMusician();

    // Mode select widgets
    juce::Label      titleLabel;
    juce::Label      subtitleLabel;
    juce::TextButton hostButton;
    juce::TextButton musicianButton;
    juce::Label      hostHint;
    juce::Label      musicianHint;

    juce::Image backgroundImage, logoImage;
    juce::Rectangle<int> centreCard, logoArea;   ///< painted behind/above the picker

    std::unique_ptr<HostView>     hostView;
    std::unique_ptr<MusicianView> musicianView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RootComponent)
};

} // namespace bandjam
