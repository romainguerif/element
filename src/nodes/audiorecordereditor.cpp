// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audiorecordereditor.hpp"
#include "nodes/audiorecorder.hpp"

namespace element {

namespace {
juce::String formatElapsed (juce::int64 samples, double sampleRate)
{
    if (sampleRate <= 0.0)
        return "00:00.000";
    const double seconds = (double) samples / sampleRate;
    const int totalMs = (int) std::llround (seconds * 1000.0);
    const int hr = totalMs / 3600000;
    const int mn = (totalMs / 60000) % 60;
    const int sc = (totalMs / 1000) % 60;
    const int ms = totalMs % 1000;
    if (hr > 0)
        return juce::String::formatted ("%02d:%02d:%02d.%03d", hr, mn, sc, ms);
    return juce::String::formatted ("%02d:%02d.%03d", mn, sc, ms);
}
} // namespace

//==============================================================================
AudioRecorderEditor::AudioRecorderEditor (const Node& node)
    : NodeEditor (node)
{
    recorder = dynamic_cast<AudioRecorderNode*> (node.getObject());

    addAndMakeVisible (titleLabel);
    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));

    addAndMakeVisible (destLabel);
    addAndMakeVisible (destValue);
    destValue.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.15f));
    destValue.setColour (juce::Label::outlineColourId, juce::Colours::black.withAlpha (0.25f));

    addAndMakeVisible (chooseButton);
    chooseButton.onClick = [this]() { chooseDirectory(); };

    addAndMakeVisible (formatLabel);
    addAndMakeVisible (formatBox);
    formatBox.addItem ("24-bit PCM", 1);
    formatBox.addItem ("32-bit float", 2);
    formatBox.onChange = [this]() {
        if (recorder)
            recorder->setBitDepth (formatBox.getSelectedId() == 2
                                       ? AudioRecorderNode::BitDepth::Float32
                                       : AudioRecorderNode::BitDepth::Int24);
    };

    addAndMakeVisible (modeLabel);
    addAndMakeVisible (modeBox);
    modeBox.addItem ("Single 32-channel WAV", 1);
    modeBox.addItem ("One WAV per stereo pair", 2);
    modeBox.onChange = [this]() {
        if (recorder)
            recorder->setFileMode (modeBox.getSelectedId() == 2
                                       ? AudioRecorderNode::FileMode::OneFilePerStereoPair
                                       : AudioRecorderNode::FileMode::OneMultichannelFile);
    };

    addAndMakeVisible (recordButton);
    recordButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkred);
    recordButton.onClick = [this]() { toggleRecording(); };

    addAndMakeVisible (timeLabel);
    timeLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                       18.0f,
                                                       juce::Font::plain)));
    timeLabel.setJustificationType (juce::Justification::centred);

    if (recorder)
        recorder->addStateListener (this);

    refreshFromNode();
    setSize (440, 220);
    startTimerHz (10);
}

AudioRecorderEditor::~AudioRecorderEditor()
{
    if (recorder)
        recorder->removeStateListener (this);
}

//==============================================================================
void AudioRecorderEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2d2f33));
}

void AudioRecorderEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    titleLabel.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);

    auto row1 = r.removeFromTop (28);
    destLabel.setBounds (row1.removeFromLeft (60));
    chooseButton.setBounds (row1.removeFromRight (90));
    destValue.setBounds (row1.reduced (4, 0));
    r.removeFromTop (6);

    auto row2 = r.removeFromTop (28);
    formatLabel.setBounds (row2.removeFromLeft (60));
    formatBox.setBounds (row2.removeFromLeft (180));
    r.removeFromTop (6);

    auto row3 = r.removeFromTop (28);
    modeLabel.setBounds (row3.removeFromLeft (60));
    modeBox.setBounds (row3.removeFromLeft (220));
    r.removeFromTop (12);

    auto bottom = r.removeFromBottom (52);
    recordButton.setBounds (bottom.removeFromLeft (140));
    bottom.removeFromLeft (12);
    timeLabel.setBounds (bottom);
}

//==============================================================================
void AudioRecorderEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromNode();
}

void AudioRecorderEditor::refreshFromNode()
{
    if (! recorder)
        return;

    destValue.setText (recorder->getDestinationDisplayName(), juce::dontSendNotification);

    formatBox.setSelectedId (recorder->getBitDepth() == AudioRecorderNode::BitDepth::Float32 ? 2 : 1,
                             juce::dontSendNotification);
    modeBox.setSelectedId (recorder->getFileMode() == AudioRecorderNode::FileMode::OneFilePerStereoPair ? 2 : 1,
                           juce::dontSendNotification);

    const bool rec = recorder->isRecording();
    recordButton.setButtonText (rec ? "Stop" : "Record");
    recordButton.setColour (juce::TextButton::buttonColourId,
                            rec ? juce::Colours::red : juce::Colours::darkred);

    // Settings are locked while recording.
    formatBox.setEnabled (! rec);
    modeBox.setEnabled (! rec);
    chooseButton.setEnabled (! rec);
}

void AudioRecorderEditor::timerCallback()
{
    if (! recorder)
        return;
    const auto sr = (double) (recorder->getSampleRate() > 0.0
                                  ? recorder->getSampleRate()
                                  : 0.0);
    timeLabel.setText (formatElapsed (recorder->getElapsedSamples(), sr),
                       juce::dontSendNotification);
}

//==============================================================================
void AudioRecorderEditor::chooseDirectory()
{
    if (! recorder)
        return;

    auto initial = recorder->getDestinationDirectory();
    if (! initial.isDirectory())
        initial = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    fileChooser = std::make_unique<juce::FileChooser> ("Choose recording folder",
                                                        initial,
                                                        juce::String(),
                                                        true);
    auto flags = juce::FileBrowserComponent::openMode
                 | juce::FileBrowserComponent::canSelectDirectories;
    fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc) {
        const auto chosen = fc.getResult();
        if (chosen != juce::File() && recorder)
            recorder->setDestinationDirectory (chosen);
    });
}

void AudioRecorderEditor::toggleRecording()
{
    if (! recorder)
        return;
    if (recorder->isRecording())
        recorder->stopRecording();
    else
        recorder->startRecording (recorder->getDestinationDirectory());
}

} // namespace element
