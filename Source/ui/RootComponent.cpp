#include "RootComponent.h"
#include "Style.h"
#include "host/HostView.h"
#include "musician/MusicianView.h"
#include "BinaryData.h"

namespace bandjam
{
namespace
{
    // Dark-gold theme of the startup screen (matches the app icon).
    const juce::Colour kGoldDark   (0xff8a6410);
    const juce::Colour kGold       (0xfff2a71b);
    const juce::Colour kGoldBright (0xffffc94d);

    juce::Font pickTitleFont (float height)
    {
        // Bahnschrift ships with Windows 10+; fall back to Impact, then the
        // JUCE default, so the screen never breaks on exotic systems.
        for (const auto* name : { "Bahnschrift", "Impact" })
            if (juce::Font::findAllTypefaceNames().contains (name))
                return juce::Font (juce::FontOptions (name, height, juce::Font::bold));

        return juce::Font (juce::FontOptions (height, juce::Font::bold));
    }
}

RootComponent::RootComponent()
{
    backgroundImage = juce::ImageCache::getFromMemory (BinaryData::startup_background_png,
                                                       BinaryData::startup_background_pngSize);
    logoImage       = juce::ImageCache::getFromMemory (BinaryData::app_icon_png,
                                                       BinaryData::app_icon_pngSize);

    auto titleFont = pickTitleFont (58.0f);
    titleFont.setExtraKerningFactor (0.08f);
    titleLabel.setText ("VRC DOWNBEAT", juce::dontSendNotification);
    titleLabel.setFont (titleFont);
    titleLabel.setColour (juce::Label::textColourId, kGoldBright);
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    auto subtitleFont = pickTitleFont (18.0f);
    subtitleFont.setExtraKerningFactor (0.20f);
    subtitleLabel.setText ("PLAY MUSIC TOGETHER - WITHOUT LATENCY PROBLEMS", juce::dontSendNotification);
    subtitleLabel.setFont (subtitleFont);
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd7dbe2));
    subtitleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (subtitleLabel);

    auto styleGoldButton = [] (juce::TextButton& button)
    {
        button.setColour (juce::TextButton::buttonColourId, kGoldDark.withAlpha (0.34f));
        button.setColour (juce::TextButton::buttonOnColourId, kGold.withAlpha (0.55f));
        button.setColour (juce::TextButton::textColourOffId, kGoldBright);
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        button.setColour (juce::ComboBox::outlineColourId, kGold.withAlpha (0.65f));
    };

    hostButton.setButtonText ("Host a jam");
    hostButton.onClick = [this] { showHost(); };
    styleGoldButton (hostButton);
    addAndMakeVisible (hostButton);

    musicianButton.setButtonText ("Join as musician");
    musicianButton.onClick = [this] { showMusician(); };
    styleGoldButton (musicianButton);
    addAndMakeVisible (musicianButton);

    hostHint.setText ("Manage the library, start jams and hear\nthe finished mix of all musicians", juce::dontSendNotification);
    hostHint.setFont (style::normalFont());
    hostHint.setColour (juce::Label::textColourId, juce::Colour (0xffb9c0cb));
    hostHint.setJustificationType (juce::Justification::centredTop);
    addAndMakeVisible (hostHint);

    musicianHint.setText ("Connect to a host, load songs\nand play along to the backing track", juce::dontSendNotification);
    musicianHint.setFont (style::normalFont());
    musicianHint.setColour (juce::Label::textColourId, juce::Colour (0xffb9c0cb));
    musicianHint.setJustificationType (juce::Justification::centredTop);
    addAndMakeVisible (musicianHint);

    setSize (1400, 900);
}

RootComponent::~RootComponent() = default;

void RootComponent::paint (juce::Graphics& g)
{
    g.fillAll (style::background());

    // A mode view covers the whole component - skip the artwork then.
    if (hostView != nullptr || musicianView != nullptr)
        return;

    if (backgroundImage.isValid())
    {
        g.drawImage (backgroundImage, getLocalBounds().toFloat(),
                     juce::RectanglePlacement::fillDestination);

        // Darken for contrast, strongest at the bottom where the hints sit.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.35f), 0.0f, 0.0f,
                                                 juce::Colours::black.withAlpha (0.62f), 0.0f, (float) getHeight(), false));
        g.fillAll();
    }

    // Translucent card behind the picker so the text stays readable.
    {
        auto r = centreCard.toFloat();
        g.setColour (juce::Colour (0xff0c0e14).withAlpha (0.72f));
        g.fillRoundedRectangle (r, 18.0f);
        g.setColour (kGold.withAlpha (0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), 18.0f, 1.2f);
    }

    if (logoImage.isValid())
        g.drawImage (logoImage, logoArea.toFloat(), juce::RectanglePlacement::centred);
}

void RootComponent::resized()
{
    if (hostView != nullptr)     { hostView->setBounds (getLocalBounds()); return; }
    if (musicianView != nullptr) { musicianView->setBounds (getLocalBounds()); return; }

    auto area = getLocalBounds();
    auto centre = area.withSizeKeepingCentre (juce::jmin (760, area.getWidth() - 40), 520);
    centreCard = centre.expanded (36, 24);

    logoArea = centre.removeFromTop (120).withSizeKeepingCentre (110, 110);
    centre.removeFromTop (10);
    titleLabel.setBounds (centre.removeFromTop (66));
    subtitleLabel.setBounds (centre.removeFromTop (30));
    centre.removeFromTop (36);

    auto buttons = centre.removeFromTop (86);
    const int gap = 24;
    auto left  = buttons.removeFromLeft ((buttons.getWidth() - gap) / 2);
    buttons.removeFromLeft (gap);
    auto right = buttons;

    hostButton.setBounds (left);
    musicianButton.setBounds (right);

    centre.removeFromTop (12);
    auto hints = centre.removeFromTop (60);
    auto hintLeft = hints.removeFromLeft ((hints.getWidth() - gap) / 2);
    hints.removeFromLeft (gap);
    hostHint.setBounds (hintLeft);
    musicianHint.setBounds (hints);
}

void RootComponent::showModeSelect()
{
    hostView.reset();
    musicianView.reset();

    for (auto* c : { (juce::Component*) &titleLabel, (juce::Component*) &subtitleLabel,
                     (juce::Component*) &hostButton, (juce::Component*) &musicianButton,
                     (juce::Component*) &hostHint,   (juce::Component*) &musicianHint })
        c->setVisible (true);

    resized();
    repaint();
}

void RootComponent::showHost()
{
    for (auto* c : { (juce::Component*) &titleLabel, (juce::Component*) &subtitleLabel,
                     (juce::Component*) &hostButton, (juce::Component*) &musicianButton,
                     (juce::Component*) &hostHint,   (juce::Component*) &musicianHint })
        c->setVisible (false);

    hostView = std::make_unique<HostView>();
    hostView->onLeave = [this] { showModeSelect(); };
    addAndMakeVisible (*hostView);
    resized();
}

void RootComponent::showMusician()
{
    for (auto* c : { (juce::Component*) &titleLabel, (juce::Component*) &subtitleLabel,
                     (juce::Component*) &hostButton, (juce::Component*) &musicianButton,
                     (juce::Component*) &hostHint,   (juce::Component*) &musicianHint })
        c->setVisible (false);

    musicianView = std::make_unique<MusicianView>();
    musicianView->onLeave = [this] { showModeSelect(); };
    addAndMakeVisible (*musicianView);
    resized();
}

} // namespace bandjam
