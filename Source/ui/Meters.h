#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Style.h"

namespace bandjam
{
/** Horizontal peak meter: rises instantly, falls with a time-based decay
    (framerate independent), with a short peak-hold tick. */
class LevelMeter : public juce::Component
{
public:
    void setLevel (float peak0to1)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const float dt = lastUpdateMs == 0 ? 0.033f
                                           : juce::jlimit (0.0f, 0.25f, (float) (now - lastUpdateMs) * 0.001f);
        lastUpdateMs = now;

        const float target = juce::jlimit (0.0f, 1.4f, peak0to1);

        if (target >= displayed)
            displayed = target;                                    // instant attack
        else
            displayed = juce::jmax (target, displayed - decayPerSecond * dt);

        if (target >= peakHold || now - peakSetMs > 1200)
        {
            peakHold  = target;
            peakSetMs = now;
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (style::field());
        g.fillRoundedRectangle (r, 3.0f);

        const float norm = juce::jlimit (0.0f, 1.0f, displayed);
        if (norm > 0.002f)
        {
            auto fill = r.reduced (2.0f);
            fill = fill.withWidth (fill.getWidth() * norm);
            g.setColour (displayed > 1.0f ? style::bad()
                       : displayed > 0.85f ? style::warn()
                                           : style::good());
            g.fillRoundedRectangle (fill, 2.0f);
        }

        const float peakNorm = juce::jlimit (0.0f, 1.0f, peakHold);
        if (peakNorm > 0.02f)
        {
            const float x = 2.0f + (r.getWidth() - 4.0f) * peakNorm;
            g.setColour (style::textPrimary().withAlpha (0.65f));
            g.fillRect (x - 1.0f, r.getY() + 2.0f, 2.0f, r.getHeight() - 4.0f);
        }

        g.setColour (style::panelOutline());
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    }

private:
    float displayed { 0.0f };
    float peakHold  { 0.0f };
    juce::uint32 lastUpdateMs { 0 };
    juce::uint32 peakSetMs    { 0 };
    static constexpr float decayPerSecond = 2.2f;   ///< full-scale falls in ~0.45 s
};

/** Mute toggle drawn as a speaker icon; when muted the speaker turns red
    with a bold red cross over it. */
class MuteButton : public juce::Button
{
public:
    MuteButton() : juce::Button ("Mute")
    {
        setClickingTogglesState (true);
        setTooltip ("Mute");
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const bool muted = getToggleState();
        auto r = getLocalBounds().toFloat();

        g.setColour (muted ? style::bad().withAlpha (0.18f)
                           : style::field().brighter (highlighted ? 0.15f : 0.0f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (muted ? style::bad() : style::panelOutline());
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, muted ? 1.5f : 1.0f);

        auto icon = r.reduced (juce::jmax (3.0f, r.getWidth() * 0.22f),
                               juce::jmax (3.0f, r.getHeight() * 0.22f));
        if (down)
            icon = icon.reduced (0.5f);

        const auto colour = muted ? style::bad() : style::textPrimary().withAlpha (0.85f);

        // Speaker: box + cone.
        const float w = icon.getWidth(), h = icon.getHeight();
        const float cx = icon.getX(),   cy = icon.getCentreY();

        juce::Path speaker;
        speaker.startNewSubPath (cx,               cy - h * 0.18f);
        speaker.lineTo         (cx + w * 0.28f,    cy - h * 0.18f);
        speaker.lineTo         (cx + w * 0.55f,    cy - h * 0.45f);
        speaker.lineTo         (cx + w * 0.55f,    cy + h * 0.45f);
        speaker.lineTo         (cx + w * 0.28f,    cy + h * 0.18f);
        speaker.lineTo         (cx,                cy + h * 0.18f);
        speaker.closeSubPath();
        g.setColour (colour);
        g.fillPath (speaker);

        if (muted)
        {
            // Bold red X over the whole icon.
            g.setColour (style::bad());
            const float t = juce::jmax (2.0f, w * 0.14f);
            g.drawLine (icon.getX(), icon.getY(), icon.getRight(), icon.getBottom(), t);
            g.drawLine (icon.getX(), icon.getBottom(), icon.getRight(), icon.getY(), t);
        }
        else
        {
            // Sound waves.
            g.setColour (colour.withAlpha (0.8f));
            const float waveX = cx + w * 0.62f;
            for (int i = 0; i < 2; ++i)
            {
                const float radius = h * (0.22f + 0.16f * (float) i);
                juce::Path wave;
                wave.addCentredArc (waveX, cy, radius * 0.7f, radius,
                                    0.0f, juce::MathConstants<float>::pi * 0.25f,
                                    juce::MathConstants<float>::pi * 0.75f, true);
                g.strokePath (wave, juce::PathStrokeType (1.6f));
            }
        }
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MuteButton)
};

/** One mixer channel: name, dB slider, optional info text, meter, mute. */
class ChannelStrip : public juce::Component
{
public:
    std::function<void (float gainDb)> onGain;
    std::function<void (bool mute)>    onMute;

    explicit ChannelStrip (const juce::String& channelName)
    {
        nameLabel.setText (channelName, juce::dontSendNotification);
        nameLabel.setFont (style::normalFont());
        nameLabel.setColour (juce::Label::textColourId, style::textPrimary());
        nameLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (nameLabel);

        slider.setRange (-40.0, 12.0, 0.1);
        slider.setValue (0.0, juce::dontSendNotification);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 20);
        slider.setTextValueSuffix (" dB");
        slider.onValueChange = [this] { if (onGain) onGain ((float) slider.getValue()); };
        addAndMakeVisible (slider);

        addAndMakeVisible (meter);

        muteButton.onClick = [this] { if (onMute) onMute (muteButton.getToggleState()); };
        addAndMakeVisible (muteButton);

        infoLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
        infoLabel.setColour (juce::Label::textColourId, style::textDim());
        infoLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (infoLabel);
    }

    void setValues (float gainDb, bool mute)
    {
        slider.setValue (gainDb, juce::dontSendNotification);
        muteButton.setToggleState (mute, juce::dontSendNotification);
    }

    void setLevel (float peak)                   { meter.setLevel (peak); }
    void setInfoText (const juce::String& text)  { infoLabel.setText (text, juce::dontSendNotification); }
    void setName (const juce::String& text)      { nameLabel.setText (text, juce::dontSendNotification); }

    void resized() override
    {
        auto r = getLocalBounds();
        nameLabel.setBounds (r.removeFromLeft (juce::jmin (140, r.getWidth() / 3)));
        muteButton.setBounds (r.removeFromRight (30).reduced (0, 3));
        r.removeFromRight (4);
        meter.setBounds (r.removeFromRight (70).reduced (0, 7));
        r.removeFromRight (4);
        infoLabel.setBounds (r.removeFromRight (64));
        r.removeFromRight (4);
        slider.setBounds (r.reduced (0, 1));
    }

private:
    juce::Label      nameLabel, infoLabel;
    juce::Slider     slider;
    LevelMeter       meter;
    MuteButton       muteButton;
};

} // namespace bandjam
