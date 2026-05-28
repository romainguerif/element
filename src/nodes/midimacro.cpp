// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/midimacro.hpp"

#include <element/ui/style.hpp>

#include <cmath>

using namespace juce;

namespace element {

namespace {

inline float applyCurve (int curve, float v01)
{
    switch (curve)
    {
        case MidiMacroProcessor::CurveExp: return v01 * v01;            // slow-start, fast-end
        case MidiMacroProcessor::CurveLog: return std::sqrt (v01);      // fast-start, slow-end
        default:                            return v01;                  // linear
    }
}

} // namespace

//==============================================================================
MidiMacroProcessor::MidiMacroProcessor()
    : BaseProcessor (BusesProperties())
{
    setPlayConfigDetails (0, 0, 44100.0, 512);
}

MidiMacroProcessor::~MidiMacroProcessor() = default;

void MidiMacroProcessor::fillInPluginDescription (PluginDescription& desc) const
{
    desc.name = getName();
    desc.fileOrIdentifier = EL_NODE_ID_MIDI_MACRO;
    desc.descriptiveName = "One CC in, six CCs out — live macro mapper";
    desc.category = "MIDI";
    desc.numInputChannels = 0;
    desc.numOutputChannels = 0;
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = "Element";
    desc.version = "1.0.0";
    desc.uniqueId = EL_NODE_UID_MIDI_MACRO;
}

void MidiMacroProcessor::prepareToPlay (double sr, int blockSize)
{
    setPlayConfigDetails (0, 0, sr, blockSize);
}

void MidiMacroProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi)
{
    if (midi.isEmpty())
        return;

    const int wantCc   = listenCc.load (std::memory_order_relaxed);
    const int wantChan = listenChan.load (std::memory_order_relaxed);

    juce::MidiBuffer out;
    for (const auto m : midi)
    {
        const auto msg = m.getMessage();
        const int  pos = m.samplePosition;

        // Always pass the original message through. The macro is additive —
        // it produces *extra* MIDI events alongside the input, not a
        // replacement.
        out.addEvent (msg, pos);

        if (! msg.isController())
            continue;
        if (msg.getControllerNumber() != wantCc)
            continue;
        if (wantChan != 0 && msg.getChannel() != wantChan)
            continue;

        const float v01 = (float) msg.getControllerValue() / 127.0f;

        for (auto& o : outputs)
        {
            if (! o.enabled.load (std::memory_order_relaxed))
                continue;

            const int curve   = o.curve.load (std::memory_order_relaxed);
            const int outCc   = juce::jlimit (0, 127, o.outCc.load (std::memory_order_relaxed));
            const int outChan = juce::jlimit (1, 16,  o.outChan.load (std::memory_order_relaxed));
            const int rMin    = juce::jlimit (0, 127, o.rangeMin.load (std::memory_order_relaxed));
            const int rMax    = juce::jlimit (0, 127, o.rangeMax.load (std::memory_order_relaxed));

            const float shaped = applyCurve (curve, v01);
            // Lerp from min to max (max < min inverts polarity naturally).
            const int   value  = juce::jlimit (0, 127,
                                  (int) std::round ((float) rMin + shaped * (float) (rMax - rMin)));

            out.addEvent (juce::MidiMessage::controllerEvent (outChan, outCc, value), pos);
        }
    }
    midi.swapWith (out);
}

//==============================================================================
void MidiMacroProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("midimacro");
    state.setProperty ("listenCc",   listenCc.load(),   nullptr);
    state.setProperty ("listenChan", listenChan.load(), nullptr);

    for (int i = 0; i < kNumOutputs; ++i)
    {
        juce::ValueTree o ("output");
        o.setProperty ("idx",      i, nullptr)
         .setProperty ("enabled",  outputs[(size_t) i].enabled.load(),  nullptr)
         .setProperty ("outCc",    outputs[(size_t) i].outCc.load(),    nullptr)
         .setProperty ("outChan",  outputs[(size_t) i].outChan.load(),  nullptr)
         .setProperty ("rangeMin", outputs[(size_t) i].rangeMin.load(), nullptr)
         .setProperty ("rangeMax", outputs[(size_t) i].rangeMax.load(), nullptr)
         .setProperty ("curve",    outputs[(size_t) i].curve.load(),    nullptr);
        state.addChild (o, -1, nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MidiMacroProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

    listenCc.store ((int) state.getProperty ("listenCc", 1));
    listenChan.store ((int) state.getProperty ("listenChan", 0));

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto o = state.getChild (i);
        const int idx = o.getProperty ("idx", -1);
        if (! juce::isPositiveAndBelow (idx, kNumOutputs))
            continue;
        outputs[(size_t) idx].enabled.store  ((bool) o.getProperty ("enabled", false));
        outputs[(size_t) idx].outCc.store    ((int)  o.getProperty ("outCc", 1));
        outputs[(size_t) idx].outChan.store  ((int)  o.getProperty ("outChan", 1));
        outputs[(size_t) idx].rangeMin.store ((int)  o.getProperty ("rangeMin", 0));
        outputs[(size_t) idx].rangeMax.store ((int)  o.getProperty ("rangeMax", 127));
        outputs[(size_t) idx].curve.store    ((int)  o.getProperty ("curve", CurveLinear));
    }
}

//==============================================================================
// Editor: compact panel — one row per output, plus an "input CC" header.
class MidiMacroEditor : public juce::AudioProcessorEditor
{
public:
    explicit MidiMacroEditor (MidiMacroProcessor& p) : juce::AudioProcessorEditor (&p), proc (p)
    {
        setOpaque (true);

        addAndMakeVisible (titleLabel);
        titleLabel.setText ("MIDI Macro", dontSendNotification);
        titleLabel.setJustificationType (Justification::centred);
        titleLabel.setFont (Font (FontOptions (12.0f, Font::bold)));
        titleLabel.setColour (Label::textColourId, Colours::white);

        addAndMakeVisible (inputCcLabel);
        inputCcLabel.setText ("Listen CC", dontSendNotification);
        inputCcLabel.setFont (Font (FontOptions (10.5f)));
        inputCcLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.7f));

        setupIntField (inputCcField, 0, 127);
        inputCcField.setText (String (proc.listenCc.load()), dontSendNotification);
        inputCcField.onTextChange = [this]{
            proc.listenCc.store (parseInt (inputCcField, 0, 127));
        };

        addAndMakeVisible (inputChanLabel);
        inputChanLabel.setText ("Chan (0=any)", dontSendNotification);
        inputChanLabel.setFont (Font (FontOptions (10.5f)));
        inputChanLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.7f));

        setupIntField (inputChanField, 0, 16);
        inputChanField.setText (String (proc.listenChan.load()), dontSendNotification);
        inputChanField.onTextChange = [this]{
            proc.listenChan.store (parseInt (inputChanField, 0, 16));
        };

        addAndMakeVisible (headerRow);

        for (int i = 0; i < MidiMacroProcessor::kNumOutputs; ++i)
        {
            auto& r = rows[i];
            auto& o = proc.outputs[(size_t) i];

            addAndMakeVisible (r.enabled);
            r.enabled.setClickingTogglesState (true);
            r.enabled.setButtonText ("");
            r.enabled.setColour (TextButton::buttonOnColourId, Colour (0xff60c060));
            r.enabled.setToggleState (o.enabled.load(), dontSendNotification);
            r.enabled.onClick = [&o, &r] { o.enabled.store (r.enabled.getToggleState()); };

            setupIntField (r.cc,  0, 127); r.cc.setText  (String (o.outCc.load()),    dontSendNotification);
            setupIntField (r.ch,  1, 16);  r.ch.setText  (String (o.outChan.load()),  dontSendNotification);
            setupIntField (r.min, 0, 127); r.min.setText (String (o.rangeMin.load()), dontSendNotification);
            setupIntField (r.max, 0, 127); r.max.setText (String (o.rangeMax.load()), dontSendNotification);
            r.cc.onTextChange  = [this, &o, &r]{ o.outCc.store    (parseInt (r.cc,  0, 127)); };
            r.ch.onTextChange  = [this, &o, &r]{ o.outChan.store  (parseInt (r.ch,  1, 16));  };
            r.min.onTextChange = [this, &o, &r]{ o.rangeMin.store (parseInt (r.min, 0, 127)); };
            r.max.onTextChange = [this, &o, &r]{ o.rangeMax.store (parseInt (r.max, 0, 127)); };

            addAndMakeVisible (r.curve);
            r.curve.addItem ("Lin", 1);
            r.curve.addItem ("Exp", 2);
            r.curve.addItem ("Log", 3);
            r.curve.setSelectedId (o.curve.load() + 1, dontSendNotification);
            r.curve.setColour (ComboBox::textColourId, Colours::white);
            r.curve.setColour (ComboBox::backgroundColourId, Colour (0xff1a1a1a));
            r.curve.setColour (ComboBox::outlineColourId, Colour (0xff2a2a2a));
            r.curve.onChange = [&o, &r]{ o.curve.store (r.curve.getSelectedId() - 1); };
        }

        setSize (440, 240);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff0d0d0d));

        // Column headers above the rows.
        auto headerArea = headerRow.getBounds();
        g.setColour (Colours::white.withAlpha (0.55f));
        g.setFont (Font (FontOptions (10.0f)));

        auto draw = [&] (int x, int w, const String& t)
        {
            g.drawText (t, headerArea.getX() + x, headerArea.getY(), w, headerArea.getHeight(),
                        Justification::centred);
        };
        draw (0,   24, "EN");
        draw (28,  44, "CC#");
        draw (76,  36, "CH");
        draw (116, 56, "MIN");
        draw (176, 56, "MAX");
        draw (236, 60, "CURVE");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        titleLabel.setBounds (r.removeFromTop (18));
        r.removeFromTop (6);

        // Input row
        auto inRow = r.removeFromTop (22);
        inputCcLabel.setBounds   (inRow.removeFromLeft (70));
        inputCcField.setBounds   (inRow.removeFromLeft (52).reduced (1));
        inRow.removeFromLeft (12);
        inputChanLabel.setBounds (inRow.removeFromLeft (90));
        inputChanField.setBounds (inRow.removeFromLeft (52).reduced (1));
        r.removeFromTop (8);

        // Column headers (just a placeholder bounds — drawn in paint())
        headerRow.setBounds (r.removeFromTop (14));
        r.removeFromTop (2);

        for (int i = 0; i < MidiMacroProcessor::kNumOutputs; ++i)
        {
            auto& row = rows[i];
            auto rr = r.removeFromTop (24);
            row.enabled.setBounds (rr.removeFromLeft (24).reduced (2));
            rr.removeFromLeft (4);
            row.cc.setBounds      (rr.removeFromLeft (44).reduced (1));
            rr.removeFromLeft (4);
            row.ch.setBounds      (rr.removeFromLeft (36).reduced (1));
            rr.removeFromLeft (4);
            row.min.setBounds     (rr.removeFromLeft (56).reduced (1));
            rr.removeFromLeft (4);
            row.max.setBounds     (rr.removeFromLeft (56).reduced (1));
            rr.removeFromLeft (4);
            row.curve.setBounds   (rr.removeFromLeft (60).reduced (1));
            r.removeFromTop (2);
        }
    }

private:
    void setupIntField (Label& l, int lo, int hi)
    {
        addAndMakeVisible (l);
        l.setEditable (false, true, false);
        l.setJustificationType (Justification::centred);
        l.setFont (Font (FontOptions (11.0f)));
        l.setColour (Label::textColourId, Colours::white);
        l.setColour (Label::backgroundColourId, Colour (0xff1a1a1a));
        l.setColour (Label::outlineColourId,    Colour (0xff2a2a2a));
        l.getProperties().set ("loBound", lo);
        l.getProperties().set ("hiBound", hi);
    }
    static int parseInt (Label& l, int lo, int hi)
    {
        return juce::jlimit (lo, hi, l.getText().getIntValue());
    }

    MidiMacroProcessor& proc;
    Label titleLabel;
    Label inputCcLabel, inputCcField;
    Label inputChanLabel, inputChanField;
    Component headerRow;
    struct Row
    {
        TextButton enabled;
        Label cc, ch, min, max;
        ComboBox curve;
    };
    Row rows[MidiMacroProcessor::kNumOutputs];
};

AudioProcessorEditor* MidiMacroProcessor::createEditor()
{
    return new MidiMacroEditor (*this);
}

} // namespace element
