// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class ParameterMapperNode;

class ParameterMapperEditor : public NodeEditor,
                              public juce::ChangeListener
{
public:
    ParameterMapperEditor (const Node& node);
    ~ParameterMapperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    class KnobCell;

    void refreshFromNode();
    void toggleLearnMode();

    ParameterMapperNode* mapper = nullptr;

    juce::TextButton learnButton { "Learn" };
    juce::OwnedArray<juce::TextButton> bankButtons;
    juce::OwnedArray<KnobCell> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterMapperEditor)
};

} // namespace element
