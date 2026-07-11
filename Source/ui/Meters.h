#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Style.h"
#include "common/ChannelFx.h"

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

/** Pop-out editor for one channel's FX chain (opened from the "FX" button
    of a ChannelStrip via CallOutBox). Effects are added from a menu, can be
    reordered with Up/Down (processing runs top to bottom) and removed;
    selecting one shows its parameters below. Every change is applied to the
    live audio immediately. */
class FxEditorPanel : public juce::Component,
                      private juce::ListBoxModel
{
public:
    FxEditorPanel (const juce::String& channelName, FxSettings initial,
                   std::function<void (FxSettings)> applyCallback)
        : apply (std::move (applyCallback)), settings (std::move (initial))
    {
        title.setText (channelName, juce::dontSendNotification);
        title.setFont (style::normalFont());
        title.setColour (juce::Label::textColourId, style::textPrimary());
        addAndMakeVisible (title);

        orderHint.setText ("Signal runs top to bottom", juce::dontSendNotification);
        orderHint.setFont (juce::Font (juce::FontOptions (11.5f)));
        orderHint.setColour (juce::Label::textColourId, style::textDim());
        addAndMakeVisible (orderHint);

        addButton.setButtonText ("+ Add effect");
        addButton.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (addButton);

        upButton.setButtonText ("Up");
        upButton.onClick = [this] { moveSelected (-1); };
        addAndMakeVisible (upButton);

        downButton.setButtonText ("Down");
        downButton.onClick = [this] { moveSelected (1); };
        addAndMakeVisible (downButton);

        removeButton.setButtonText ("Remove");
        removeButton.onClick = [this] { removeSelected(); };
        addAndMakeVisible (removeButton);

        clearButton.setButtonText ("Clear all");
        clearButton.onClick = [this]
        {
            settings.effects.clear();
            chainList.updateContent();
            chainList.deselectAllRows();
            rebuildParams();
            push();
        };
        addAndMakeVisible (clearButton);

        chainList.setModel (this);
        chainList.setRowHeight (24);
        chainList.setColour (juce::ListBox::backgroundColourId, style::field());
        chainList.setColour (juce::ListBox::outlineColourId, style::panelOutline());
        chainList.setOutlineThickness (1);
        addAndMakeVisible (chainList);

        emptyHint.setText ("No effects - click \"+ Add effect\".", juce::dontSendNotification);
        emptyHint.setFont (juce::Font (juce::FontOptions (12.5f)));
        emptyHint.setColour (juce::Label::textColourId, style::textDim());
        emptyHint.setJustificationType (juce::Justification::centred);
        addChildComponent (emptyHint);

        setSize (500, 360);

        if (! settings.effects.empty())
            chainList.selectRow (0);
        rebuildParams();
        updateButtons();
    }

    void paint (juce::Graphics& g) override { g.fillAll (style::background()); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);

        auto top = r.removeFromTop (22);
        clearButton.setBounds (top.removeFromRight (72).reduced (0, 1));
        title.setBounds (top);
        r.removeFromTop (4);

        auto buttons = r.removeFromTop (26);
        addButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        orderHint.setBounds (buttons.removeFromLeft (170));
        removeButton.setBounds (buttons.removeFromRight (70));
        buttons.removeFromRight (4);
        downButton.setBounds (buttons.removeFromRight (52));
        buttons.removeFromRight (4);
        upButton.setBounds (buttons.removeFromRight (44));
        r.removeFromTop (6);

        auto listArea = r.removeFromTop (130);
        chainList.setBounds (listArea);
        emptyHint.setBounds (listArea);
        r.removeFromTop (8);

        paramArea = r;
        layoutParams();
    }

private:
    // -- ListBoxModel -----------------------------------------------------------------
    int getNumRows() override { return (int) settings.effects.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) settings.effects.size()))
            return;

        if (selected)
            g.fillAll (style::accentDark().withAlpha (0.45f));

        const auto& effect = settings.effects[(size_t) row];
        const auto* desc = fx::find (effect.type);

        g.setColour (style::textPrimary());
        g.setFont (juce::Font (juce::FontOptions (13.5f)));
        g.drawText (juce::String (row + 1) + ".  " + (desc != nullptr ? desc->name : effect.type),
                    8, 0, width - 16, height, juce::Justification::centredLeft);
    }

    void selectedRowsChanged (int) override
    {
        rebuildParams();
        updateButtons();
    }

    // -- actions ----------------------------------------------------------------------
    void showAddMenu()
    {
        juce::PopupMenu menu;
        int id = 1;
        for (const auto& desc : fx::catalog())
            menu.addItem (id++, desc.name);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton),
            [safe = juce::Component::SafePointer<FxEditorPanel> (this)] (int result)
            {
                if (safe == nullptr || result <= 0)
                    return;
                const auto& catalog = fx::catalog();
                if (! juce::isPositiveAndBelow (result - 1, (int) catalog.size()))
                    return;

                safe->settings.effects.push_back (
                    FxEffect::makeDefault (catalog[(size_t) (result - 1)].type));
                safe->chainList.updateContent();
                safe->chainList.selectRow ((int) safe->settings.effects.size() - 1);
                safe->push();
                safe->updateButtons();
            });
    }

    void moveSelected (int delta)
    {
        const int row = chainList.getSelectedRow();
        const int target = row + delta;
        if (! juce::isPositiveAndBelow (row, (int) settings.effects.size())
            || ! juce::isPositiveAndBelow (target, (int) settings.effects.size()))
            return;

        std::swap (settings.effects[(size_t) row], settings.effects[(size_t) target]);
        chainList.updateContent();
        chainList.selectRow (target);
        chainList.repaint();
        push();
    }

    void removeSelected()
    {
        const int row = chainList.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, (int) settings.effects.size()))
            return;

        settings.effects.erase (settings.effects.begin() + row);
        chainList.updateContent();
        if (! settings.effects.empty())
            chainList.selectRow (juce::jmin (row, (int) settings.effects.size() - 1));
        else
            rebuildParams();
        push();
        updateButtons();
    }

    // -- parameters for the selected effect ---------------------------------------------
    void rebuildParams()
    {
        paramSliders.clear (true);
        paramLabels.clear (true);

        const int row = chainList.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, (int) settings.effects.size()))
        {
            layoutParams();
            return;
        }

        const auto& effect = settings.effects[(size_t) row];
        const auto* desc = fx::find (effect.type);
        if (desc == nullptr)
        {
            layoutParams();
            return;
        }

        for (const auto& pd : desc->params)
        {
            auto* label = new juce::Label();
            label->setText (pd.label, juce::dontSendNotification);
            label->setFont (juce::Font (juce::FontOptions (12.0f)));
            label->setColour (juce::Label::textColourId, style::textDim());
            label->setJustificationType (juce::Justification::centred);
            addAndMakeVisible (label);
            paramLabels.add (label);

            auto* slider = new juce::Slider();
            slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 16);
            slider->setRange (pd.minValue, pd.maxValue, 0.0);
            slider->setNumDecimalPlacesToDisplay (pd.maxValue - pd.minValue > 100.0f ? 0 : 2);
            if (pd.skewMidPoint > 0.0f)
                slider->setSkewFactorFromMidPoint (pd.skewMidPoint);
            slider->setTextValueSuffix (pd.suffix);
            slider->setValue (effect.get (pd.id), juce::dontSendNotification);

            const juce::String paramId (pd.id);
            slider->onValueChange = [this, row, paramId, slider]
            {
                if (juce::isPositiveAndBelow (row, (int) settings.effects.size()))
                {
                    settings.effects[(size_t) row].set (paramId, (float) slider->getValue());
                    push();
                }
            };
            addAndMakeVisible (slider);
            paramSliders.add (slider);
        }

        layoutParams();
    }

    void layoutParams()
    {
        emptyHint.setVisible (settings.effects.empty());

        if (paramSliders.isEmpty() || paramArea.isEmpty())
            return;

        auto r = paramArea;
        const int cols = juce::jmax (1, paramSliders.size());
        const int w = r.getWidth() / cols;

        for (int i = 0; i < paramSliders.size(); ++i)
        {
            auto col = r.withX (r.getX() + i * w).withWidth (w).reduced (2, 0);
            paramLabels.getUnchecked (i)->setBounds (col.removeFromTop (16));
            paramSliders.getUnchecked (i)->setBounds (col);
        }
    }

    void updateButtons()
    {
        const int row = chainList.getSelectedRow();
        const bool valid = juce::isPositiveAndBelow (row, (int) settings.effects.size());
        upButton.setEnabled (valid && row > 0);
        downButton.setEnabled (valid && row < (int) settings.effects.size() - 1);
        removeButton.setEnabled (valid);
        clearButton.setEnabled (! settings.effects.empty());
    }

    void push()
    {
        if (apply)
            apply (settings);
    }

    std::function<void (FxSettings)> apply;
    FxSettings settings;

    juce::Label      title, orderHint, emptyHint;
    juce::TextButton addButton, upButton, downButton, removeButton, clearButton;
    juce::ListBox    chainList { "fxChain", nullptr };
    juce::OwnedArray<juce::Slider> paramSliders;
    juce::OwnedArray<juce::Label>  paramLabels;
    juce::Rectangle<int> paramArea;
};

/** One mixer channel: name, dB slider, optional info text, meter, FX, mute. */
class ChannelStrip : public juce::Component
{
public:
    std::function<void (float gainDb)> onGain;
    std::function<void (bool mute)>    onMute;

    /** Set both to show an "FX" button that opens the EQ/reverb editor. */
    std::function<FxSettings()>            getFx;
    std::function<void (const FxSettings&)> setFx;

    /** Optional second mute ("MIC"): cuts the channel out of recordings and
        the VRChat stream (used on the host's own input strip). */
    std::function<void (bool)> onCaptureMute;

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

        fxButton.setButtonText ("FX");
        fxButton.setTooltip ("Equalizer + effects (applied live - recordings stay raw)");
        fxButton.onClick = [this] { showFxEditor(); };
        addChildComponent (fxButton);

        captureMuteButton.setButtonText ("MIC");
        captureMuteButton.setClickingTogglesState (true);
        captureMuteButton.setTooltip ("Keep this input out of recordings and the VRChat stream");
        captureMuteButton.onClick = [this]
        {
            refreshCaptureMuteColour();
            if (onCaptureMute)
                onCaptureMute (captureMuteButton.getToggleState());
        };
        addChildComponent (captureMuteButton);

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

    /** Call after setting getFx/setFx to show the FX button. */
    void enableFx()
    {
        fxButton.setVisible (getFx != nullptr && setFx != nullptr);
        refreshFxColour();
        resized();
    }

    /** Shows the "MIC" capture-mute button (host input strip). */
    void enableCaptureMute (bool muted, const juce::String& tooltip = {})
    {
        captureMuteButton.setVisible (true);
        captureMuteButton.setToggleState (muted, juce::dontSendNotification);
        if (tooltip.isNotEmpty())
            captureMuteButton.setTooltip (tooltip);
        refreshCaptureMuteColour();
        resized();
    }

    void setLevel (float peak)                   { meter.setLevel (peak); }
    void setInfoText (const juce::String& text)  { infoLabel.setText (text, juce::dontSendNotification); }
    void setName (const juce::String& text)      { nameLabel.setText (text, juce::dontSendNotification); }

    void setMuteTooltip (const juce::String& text) { muteButton.setTooltip (text); }

    void resized() override
    {
        auto r = getLocalBounds();
        nameLabel.setBounds (r.removeFromLeft (juce::jmin (140, r.getWidth() / 3)));
        muteButton.setBounds (r.removeFromRight (30).reduced (0, 3));
        r.removeFromRight (4);
        if (captureMuteButton.isVisible())
        {
            captureMuteButton.setBounds (r.removeFromRight (40).reduced (0, 4));
            r.removeFromRight (4);
        }
        if (fxButton.isVisible())
        {
            fxButton.setBounds (r.removeFromRight (34).reduced (0, 4));
            r.removeFromRight (4);
        }
        meter.setBounds (r.removeFromRight (70).reduced (0, 7));
        r.removeFromRight (4);
        infoLabel.setBounds (r.removeFromRight (64));
        r.removeFromRight (4);
        slider.setBounds (r.reduced (0, 1));
    }

private:
    void showFxEditor()
    {
        if (getFx == nullptr || setFx == nullptr)
            return;

        auto content = std::make_unique<FxEditorPanel> (nameLabel.getText(), getFx(),
            [this] (FxSettings s)
            {
                if (setFx)
                    setFx (s);
                refreshFxColour();
            });

        juce::CallOutBox::launchAsynchronously (std::move (content),
                                                getScreenBounds().withLeft (fxButton.getScreenX()),
                                                nullptr);
    }

    void refreshFxColour()
    {
        const bool active = getFx != nullptr && getFx().isActive();
        fxButton.setColour (juce::TextButton::buttonColourId,
                            active ? style::accent() : style::field());
        fxButton.setColour (juce::TextButton::textColourOffId,
                            active ? style::background() : style::textPrimary());
        fxButton.repaint();
    }

    void refreshCaptureMuteColour()
    {
        const bool muted = captureMuteButton.getToggleState();
        captureMuteButton.setColour (juce::TextButton::buttonColourId,
                                     muted ? style::bad() : style::field());
        captureMuteButton.setColour (juce::TextButton::buttonOnColourId,
                                     style::bad());
        captureMuteButton.repaint();
    }

    juce::Label      nameLabel, infoLabel;
    juce::Slider     slider;
    LevelMeter       meter;
    MuteButton       muteButton;
    juce::TextButton fxButton, captureMuteButton;
};

} // namespace bandjam
