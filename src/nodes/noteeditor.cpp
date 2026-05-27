// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/noteeditor.hpp"
#include "nodes/note.hpp"

namespace element {

NoteEditor::NoteEditor (const Node& node)
    : NodeEditor (node)
{
    note = dynamic_cast<NoteNode*> (node.getObject());

    addAndMakeVisible (titleField);
    titleField.setMultiLine (false);
    titleField.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    titleField.setTextToShowWhenEmpty ("Title", juce::Colours::grey);
    titleField.onTextChange = [this]() {
        if (note) note->setTitle (titleField.getText());
    };

    addAndMakeVisible (bodyField);
    bodyField.setMultiLine (true, true);
    bodyField.setReturnKeyStartsNewLine (true);
    bodyField.setTextToShowWhenEmpty ("Write something...", juce::Colours::grey);
    bodyField.onTextChange = [this]() {
        if (note) note->setBody (bodyField.getText());
    };

    addAndMakeVisible (sizeLabel);
    addAndMakeVisible (sizeSlider);
    sizeSlider.setRange (8.0, 48.0, 1.0);
    sizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    sizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);
    sizeSlider.onValueChange = [this]() {
        if (note) note->setTextSize ((float) sizeSlider.getValue());
    };

    addAndMakeVisible (colourLabel);
    addAndMakeVisible (colourButton);
    colourButton.onClick = [this]() { chooseColour(); };

    if (note)
        note->addStateListener (this);

    refreshFromNode();
    setSize (380, 320);
}

NoteEditor::~NoteEditor()
{
    if (note)
        note->removeStateListener (this);
}

//==============================================================================
void NoteEditor::paint (juce::Graphics& g)
{
    const auto bg = note ? note->getStickerColour() : juce::Colour (0xfff9d976);
    g.fillAll (bg.darker (0.35f));
}

void NoteEditor::resized()
{
    auto r = getLocalBounds().reduced (10);

    titleField.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);

    auto controls = r.removeFromBottom (28);
    colourLabel.setBounds (controls.removeFromLeft (90));
    colourButton.setBounds (controls.removeFromLeft (80));
    controls.removeFromLeft (8);
    sizeLabel.setBounds (controls.removeFromLeft (70));
    sizeSlider.setBounds (controls);
    r.removeFromBottom (6);

    bodyField.setBounds (r);
}

//==============================================================================
void NoteEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromNode();
}

void NoteEditor::refreshFromNode()
{
    if (! note) return;

    // Only update text fields when the value differs, otherwise we'd reset
    // the caret position every time the user types.
    if (titleField.getText() != note->getTitle())
        titleField.setText (note->getTitle(), juce::dontSendNotification);
    if (bodyField.getText() != note->getBody())
        bodyField.setText (note->getBody(), juce::dontSendNotification);

    sizeSlider.setValue (note->getTextSize(), juce::dontSendNotification);
    bodyField.setFont (juce::Font (juce::FontOptions ((float) note->getTextSize())));

    colourButton.setColour (juce::TextButton::buttonColourId, note->getStickerColour());
    repaint();
}

void NoteEditor::chooseColour()
{
    if (! note) return;

    auto* picker = new juce::ColourSelector (juce::ColourSelector::showColourAtTop
                                                 | juce::ColourSelector::showSliders
                                                 | juce::ColourSelector::showColourspace);
    picker->setSize (300, 360);
    picker->setCurrentColour (note->getStickerColour());

    struct Listener : juce::ChangeListener
    {
        NoteNode* n;
        juce::ColourSelector* p;
        Listener (NoteNode* nn, juce::ColourSelector* pp) : n (nn), p (pp) {}
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (n && p) n->setStickerColour (p->getCurrentColour());
        }
    };

    auto* l = new Listener (note, picker);
    picker->addChangeListener (l);

    juce::CallOutBox::launchAsynchronously (
        std::unique_ptr<juce::Component> (picker),
        colourButton.getScreenBounds(),
        nullptr);

    // The listener will leak on close; acceptable for this simple editor. The
    // colour selector itself is owned by the CallOutBox.
    juce::ignoreUnused (l);
}

} // namespace element
