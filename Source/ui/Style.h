#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace bandjam
{
namespace style
{
    inline juce::Colour background()   { return juce::Colour (0xff14171c); }
    inline juce::Colour panel()        { return juce::Colour (0xff1c2129); }
    inline juce::Colour panelOutline() { return juce::Colour (0xff2d3542); }
    inline juce::Colour field()        { return juce::Colour (0xff10131a); }
    inline juce::Colour accent()       { return juce::Colour (0xff4fa3ff); }
    inline juce::Colour accentDark()   { return juce::Colour (0xff2b6cb8); }
    inline juce::Colour textPrimary()  { return juce::Colour (0xffe8ecf2); }
    inline juce::Colour textDim()      { return juce::Colour (0xff97a2b0); }
    inline juce::Colour good()         { return juce::Colour (0xff5fd068); }
    inline juce::Colour warn()         { return juce::Colour (0xffe8a13c); }
    inline juce::Colour bad()          { return juce::Colour (0xffe05f5f); }

    /** Rounded panel background used to group sections. */
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, float corner = 8.0f)
    {
        g.setColour (panel());
        g.fillRoundedRectangle (r, corner);
        g.setColour (panelOutline());
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    inline juce::Font titleFont()   { return juce::Font (juce::FontOptions (26.0f, juce::Font::bold)); }
    inline juce::Font sectionFont() { return juce::Font (juce::FontOptions (16.0f, juce::Font::bold)); }
    inline juce::Font normalFont()  { return juce::Font (juce::FontOptions (14.5f)); }

    inline void styleSectionLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (sectionFont());
        l.setColour (juce::Label::textColourId, textPrimary());
    }
}

/** App-wide dark theme. */
class BandJamLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BandJamLookAndFeel()
    {
        auto scheme = getDarkColourScheme();
        scheme.setUIColour (ColourScheme::windowBackground,   style::background());
        scheme.setUIColour (ColourScheme::widgetBackground,   style::panel());
        scheme.setUIColour (ColourScheme::menuBackground,     style::panel());
        scheme.setUIColour (ColourScheme::outline,            style::panelOutline());
        scheme.setUIColour (ColourScheme::defaultText,        style::textPrimary());
        scheme.setUIColour (ColourScheme::defaultFill,        style::accentDark());
        scheme.setUIColour (ColourScheme::highlightedText,    style::textPrimary());
        scheme.setUIColour (ColourScheme::highlightedFill,    style::accent().withAlpha (0.45f));
        scheme.setUIColour (ColourScheme::menuText,           style::textPrimary());
        setColourScheme (scheme);

        setColour (juce::TextButton::buttonColourId,        style::accentDark());
        setColour (juce::TextButton::buttonOnColourId,      style::accent());
        setColour (juce::TextButton::textColourOffId,       style::textPrimary());
        setColour (juce::TextButton::textColourOnId,        style::textPrimary());

        setColour (juce::TextEditor::backgroundColourId,    style::field());
        setColour (juce::TextEditor::textColourId,          style::textPrimary());
        setColour (juce::TextEditor::outlineColourId,       style::panelOutline());
        setColour (juce::TextEditor::focusedOutlineColourId, style::accent());

        setColour (juce::ComboBox::backgroundColourId,      style::field());
        setColour (juce::ComboBox::outlineColourId,         style::panelOutline());

        setColour (juce::ListBox::backgroundColourId,       style::field());
        setColour (juce::ListBox::outlineColourId,          style::panelOutline());

        setColour (juce::Slider::backgroundColourId,        style::field());
        setColour (juce::Slider::trackColourId,             style::accentDark());
        setColour (juce::Slider::thumbColourId,             style::accent());
        setColour (juce::Slider::textBoxBackgroundColourId, style::field());
        setColour (juce::Slider::textBoxOutlineColourId,    style::panelOutline());

        setColour (juce::Label::textColourId,               style::textPrimary());
        setColour (juce::ToggleButton::textColourId,        style::textPrimary());
        setColour (juce::ToggleButton::tickColourId,        style::accent());

        setColour (juce::PopupMenu::backgroundColourId,     style::panel());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, style::accentDark());

        setColour (juce::ScrollBar::thumbColourId,          style::panelOutline().brighter (0.3f));
        setColour (juce::TabbedComponent::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::TabbedComponent::outlineColourId,  juce::Colours::transparentBlack);
        setColour (juce::TabbedButtonBar::tabTextColourId,  style::textDim());
        setColour (juce::TabbedButtonBar::frontTextColourId, style::textPrimary());

        setColour (juce::AlertWindow::backgroundColourId,   style::panel());
        setColour (juce::AlertWindow::textColourId,         style::textPrimary());
        setColour (juce::AlertWindow::outlineColourId,      style::panelOutline());
    }
};

} // namespace bandjam
