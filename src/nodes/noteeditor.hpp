// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class NoteNode;

class NoteEditor : public NodeEditor,
                   public juce::ChangeListener
{
public:
    NoteEditor (const Node& node);
    ~NoteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void refreshFromNode();
    void chooseColour();

    NoteNode* note = nullptr;

    juce::TextEditor titleField;
    juce::TextEditor bodyField;

    juce::Label    sizeLabel { {}, "Text size" };
    juce::Slider   sizeSlider;

    juce::Label      colourLabel { {}, "Sticker color" };
    juce::TextButton colourButton { "Pick..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEditor)
};

} // namespace element
