// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class ParameterMapperNode;

class ParameterMapperEditor : public NodeEditor,
                              public juce::ChangeListener,
                              private juce::Timer
{
public:
    ParameterMapperEditor (const Node& node);
    ~ParameterMapperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void timerCallback() override;
    class KnobCell;

    void refreshFromNode();
    void toggleLearnMode();

    ParameterMapperNode* mapper = nullptr;

    class SnapshotButton;

    juce::TextButton learnButton   { "Learn" };
    juce::TextButton syncButton    { "Sync" };
    juce::TextButton previewButton { "Preview" };
    juce::TextButton nativeButton  { "Native" };
    juce::OwnedArray<juce::TextButton> bankButtons;
    juce::OwnedArray<KnobCell> knobs;
    juce::OwnedArray<SnapshotButton> snapshotButtons;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterMapperEditor)
};

} // namespace element
