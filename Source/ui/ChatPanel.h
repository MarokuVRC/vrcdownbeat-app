#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/Style.h"
#include "ui/Meters.h"

namespace bandjam
{
/** Band chat: a message log, a text input and a "Talk" toggle that turns the
    talk mic into live voice chat. The owning view wires the callbacks to the
    network and the VoiceChat engine. */
class ChatPanel : public juce::Component,
                  private juce::Timer
{
public:
    std::function<void (const juce::String& text)> onSendText;
    std::function<void (bool talk)>                onTalkToggled;

    ChatPanel()
    {
        style::styleSectionLabel (caption, "Band chat");
        addAndMakeVisible (caption);

        history.setMultiLine (true);
        history.setReadOnly (true);
        history.setScrollbarsShown (true);
        history.setCaretVisible (false);
        history.setFont (juce::Font (juce::FontOptions (13.5f)));
        history.setColour (juce::TextEditor::backgroundColourId, style::field());
        addAndMakeVisible (history);

        input.setTextToShowWhenEmpty ("Type a message...", style::textDim());
        input.setFont (style::normalFont());
        input.onReturnKey = [this] { sendClicked(); };
        addAndMakeVisible (input);

        sendButton.setButtonText ("Send");
        sendButton.onClick = [this] { sendClicked(); };
        addAndMakeVisible (sendButton);

        talkButton.setButtonText ("Talk");
        talkButton.setClickingTogglesState (true);
        talkButton.setColour (juce::TextButton::buttonOnColourId, style::good());
        talkButton.setTooltip ("Send your talk mic to the band (voice chat)");
        talkButton.onClick = [this]
        {
            if (onTalkToggled)
                onTalkToggled (talkButton.getToggleState());
        };
        addAndMakeVisible (talkButton);

        addAndMakeVisible (talkMeter);

        statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        statusLabel.setColour (juce::Label::textColourId, style::textDim());
        addAndMakeVisible (statusLabel);

        startTimerHz (20);
    }

    /** Appends "[HH:MM] name: text" and scrolls down. */
    void addMessage (const juce::String& name, const juce::String& text)
    {
        const auto stamp = juce::Time::getCurrentTime().formatted ("%H:%M");
        history.moveCaretToEnd();
        history.insertTextAtCaret ("[" + stamp + "] " + name + ": " + text + "\n");
        history.moveCaretToEnd();
    }

    void addSystemMessage (const juce::String& text)
    {
        history.moveCaretToEnd();
        history.insertTextAtCaret ("-- " + text + " --\n");
        history.moveCaretToEnd();
    }

    void setStatusText (const juce::String& text)  { statusLabel.setText (text, juce::dontSendNotification); }
    void setTalkAvailable (bool available)         { talkButton.setEnabled (available); }
    void setTalkActive (bool active)               { talkButton.setToggleState (active, juce::dontSendNotification); }
    bool isTalkActive() const                      { return talkButton.getToggleState(); }

    /** Live level of the own talk mic (polled). */
    std::function<float()> getTalkLevel;

    void resized() override
    {
        auto r = getLocalBounds();

        auto top = r.removeFromTop (24);
        caption.setBounds (top.removeFromLeft (juce::jmax (90, top.getWidth() - 150)));
        talkMeter.setBounds (top.removeFromRight (54).reduced (0, 5));
        top.removeFromRight (4);
        talkButton.setBounds (top.removeFromRight (56).reduced (0, 1));

        r.removeFromTop (4);
        statusLabel.setBounds (r.removeFromBottom (16));

        auto inputRow = r.removeFromBottom (26);
        sendButton.setBounds (inputRow.removeFromRight (58));
        inputRow.removeFromRight (4);
        input.setBounds (inputRow);

        r.removeFromBottom (4);
        history.setBounds (r);
    }

private:
    void sendClicked()
    {
        const auto text = input.getText().trim();
        if (text.isEmpty())
            return;

        input.clear();
        if (onSendText)
            onSendText (text);
    }

    void timerCallback() override
    {
        talkMeter.setLevel (getTalkLevel && talkButton.getToggleState() ? getTalkLevel() : 0.0f);
    }

    juce::Label      caption, statusLabel;
    juce::TextEditor history, input;
    juce::TextButton sendButton, talkButton;
    LevelMeter       talkMeter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChatPanel)
};

} // namespace bandjam
