// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class AudioRecorderNode;

class AudioRecorderEditor : public NodeEditor,
                            public juce::ChangeListener,
                            private juce::Timer
{
public:
    AudioRecorderEditor (const Node& node);
    ~AudioRecorderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void timerCallback() override;
    void chooseDirectory();
    void refreshFromNode();
    void toggleRecording();

    AudioRecorderNode* recorder = nullptr;

    juce::Label  titleLabel { {}, "Audio Recorder" };

    juce::Label  destLabel    { {}, "Folder" };
    juce::Label  destValue;
    juce::TextButton chooseButton { "Choose..." };

    juce::Label    formatLabel { {}, "Bit depth" };
    juce::ComboBox formatBox;

    juce::Label    modeLabel { {}, "File mode" };
    juce::ComboBox modeBox;

    juce::TextButton recordButton;
    juce::Label      timeLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRecorderEditor)
};

} // namespace element
