// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"

#include <array>
#include <atomic>

namespace element {

/// One MIDI CC in, up to 6 MIDI CCs out. Each output has its own
/// CC#, range (min..max in 0..127 — max < min inverts polarity) and
/// curve (linear / exp / log). Designed for "1 hardware knob -> several
/// plugin params with different ranges" macro mappings.
class MidiMacroProcessor : public BaseProcessor
{
public:
    static constexpr int kNumOutputs = 6;

    enum Curve { CurveLinear = 0, CurveExp, CurveLog };

    MidiMacroProcessor();
    ~MidiMacroProcessor() override;

    const juce::String getName() const override { return "MIDI Macro"; }

    void fillInPluginDescription (PluginDescription& desc) const override;

    void prepareToPlay (double sr, int blockSize) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override;

    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }
    bool canAddBus    (bool) const override { return false; }
    bool canRemoveBus (bool) const override { return false; }

    bool acceptsMidi()  const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    bool hasEditor()    const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }

    AudioProcessorEditor* createEditor() override;

    int  getNumPrograms() override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return getName(); }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Public-but-internal state accessed by the editor.
    struct Output
    {
        std::atomic<bool> enabled  { false };
        std::atomic<int>  outCc    { 1 };
        std::atomic<int>  outChan  { 1 };    // 1..16
        std::atomic<int>  rangeMin { 0 };    // 0..127
        std::atomic<int>  rangeMax { 127 };  // 0..127 (can be < min for invert)
        std::atomic<int>  curve    { CurveLinear };
    };

    std::atomic<int> listenCc    { 1 };   // 0..127
    std::atomic<int> listenChan  { 0 };   // 0 = any, 1..16 = specific
    std::array<Output, kNumOutputs> outputs;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMacroProcessor)
};

} // namespace element
