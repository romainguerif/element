// Copyright 2026 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/audiomixer.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

namespace element {

//==============================================================================
/// Minimal black/white knob, inspired by Condesa / MasterSounds. Flat ring,
/// thin white pointer, optional centered tick marks.
class MixerKnobLAF : public juce::LookAndFeel_V4
{
public:
    MixerKnobLAF();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float rotaryStart, float rotaryEnd,
                           juce::Slider&) override;
    juce::Font getLabelFont (juce::Label&) override;
};

//==============================================================================
/// A Slider that magnetises to 0.0 within a small "detent" window when the
/// user drags through it. Use for bipolar controls (pan, EQ ±, transient
/// ±, master isolator ±) so the centre position is easy to find. Also
/// hooks up double-click reset to the configured default.
class BipolarSnapSlider : public juce::Slider
{
public:
    BipolarSnapSlider() = default;
    double snapValue (double attempted, DragMode dragMode) override;
};

//==============================================================================
/// 10-segment LED VU bar — pair of these per channel for stereo, single for
/// mono helpers. Reads RMS via a getter to keep it decoupled from the model.
class LedMeter : public juce::Component, private juce::Timer
{
public:
    using LevelSource = std::function<float()>;
    LedMeter();
    void setLevelSource (LevelSource src) { source = std::move (src); }
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    LevelSource source;
    float displayedLevel = 0.0f;
};

//==============================================================================
/// A level knob with a ring of LEDs around it showing the input RMS level
/// (like a MIDI Fighter Twister or Tube-Tech LCA-2B style indicator).
/// Wraps a Slider and adds a custom LED arc overlay driven by a LevelSource.
class LevelKnob : public juce::Component, private juce::Timer
{
public:
    using LevelSource = std::function<float()>;
    explicit LevelKnob (MixerKnobLAF& laf);
    ~LevelKnob() override;

    void setLevelSource (LevelSource src) { source = std::move (src); }
    juce::Slider& slider() { return knob; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    juce::Slider knob;
    MixerKnobLAF& lookAndFeelRef;
    LevelSource source;
    float displayedLevel = 0.0f;
};

class AudioMixerEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit AudioMixerEditor (AudioMixerProcessor&);
    ~AudioMixerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    class ChannelStrip;
    class ReturnStrip;
    class MasterStrip;
    class RecorderBar;

    void rebuildStrips();

    AudioMixerProcessor& processor;
    MixerKnobLAF knobLAF;

    juce::OwnedArray<ChannelStrip> channelStrips;
    juce::OwnedArray<ReturnStrip>  returnStrips;
    std::unique_ptr<MasterStrip>   masterStrip;
    std::unique_ptr<RecorderBar>   recorderBar;

    juce::TextButton addBtn { "+" };
    juce::TextButton remBtn { "-" };
    juce::TextButton recorderToggle { "REC" };  // show/hide the recorder bar

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioMixerEditor)
};

} // namespace element
