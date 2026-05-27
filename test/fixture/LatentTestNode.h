// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <element/processor.hpp>

namespace element {

// Test fixture used by PDC tests.
//
// Two flavors of latency are useful when verifying plugin delay compensation:
//
//   - "Real" latency: the node actually delays its input by N samples via a
//     circular buffer. Use this when you want to assert that an impulse fed
//     into a serial chain comes out at the expected absolute sample.
//
//   - "Phantom" latency: the node passes input through unmodified but still
//     reports N samples of latency. Use this when you want to verify that the
//     graph builder inserts compensation on the OTHER parallel branches at a
//     summing node — without the LatentTestNode's own behavior interfering.
class LatentTestNode : public Processor
{
public:
    LatentTestNode (int numAudioChannels, int latencyInSamples, bool actuallyDelay = true)
        : Processor (0),
          numChannels (numAudioChannels),
          latency (latencyInSamples),
          doDelay (actuallyDelay)
    {
        LatentTestNode::refreshPorts();
        setLatencySamples (latency);
    }

    void setLatency (int newLatency)
    {
        if (newLatency == latency)
            return;
        latency = newLatency;
        rebuildDelayLine();
        setLatencySamples (latency);
    }

    void prepareToRender (double newSampleRate, int newBlockSize) override
    {
        setRenderDetails (newSampleRate, newBlockSize);
        rebuildDelayLine();
    }

    void releaseResources() override {}

    bool wantsContext() const noexcept override { return true; }

    void render (RenderContext& rc) override
    {
        const int numSamples = rc.audio.getNumSamples();
        const int chans = juce::jmin (rc.audio.getNumChannels(), numChannels);

        if (! doDelay || latency <= 0)
            return; // pure pass-through, just report phantom latency

        for (int ch = 0; ch < chans; ++ch)
        {
            float* data = rc.audio.getWritePointer (ch);
            auto& line = delayLines.getReference (ch);
            auto& write = writeIndex.getReference (ch);
            auto& read  = readIndex.getReference (ch);
            const int bufSize = line.size();

            for (int i = 0; i < numSamples; ++i)
            {
                line.set (write, data[i]);
                data[i] = line.getUnchecked (read);

                if (++write >= bufSize) write = 0;
                if (++read  >= bufSize) read  = 0;
            }
        }
    }

    void renderBypassed (RenderContext&) override {}

    int getNumPrograms() const override { return 1; }
    int getCurrentProgram() const override { return 0; }
    const String getProgramName (int) const override { return "program"; }
    void setCurrentProgram (int) override {}

    void getState (juce::MemoryBlock&) override {}
    void setState (const void*, int) override {}

    void getPluginDescription (juce::PluginDescription& d) const override
    {
        d.pluginFormatName = "Element";
        d.fileOrIdentifier = "element.latentTestNode";
        d.manufacturerName = "Element";
    }

    void refreshPorts() override
    {
        PortList list;
        uint32 port = 0;
        for (int c = 0; c < numChannels; ++c)
            list.add (PortType::Audio, port++, c,
                      juce::String ("audio_in_") + juce::String (c + 1),
                      juce::String ("In ") + juce::String (c + 1), true);
        for (int c = 0; c < numChannels; ++c)
            list.add (PortType::Audio, port++, c,
                      juce::String ("audio_out_") + juce::String (c + 1),
                      juce::String ("Out ") + juce::String (c + 1), false);
        setPorts (list);
    }

protected:
    void initialize() override {}

private:
    void rebuildDelayLine()
    {
        delayLines.clear();
        writeIndex.clear();
        readIndex.clear();

        const int size = juce::jmax (1, latency + 1);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::Array<float> line;
            line.insertMultiple (0, 0.0f, size);
            delayLines.add (std::move (line));
            writeIndex.add (latency);
            readIndex.add (0);
        }
    }

    int numChannels;
    int latency;
    const bool doDelay;
    juce::Array<juce::Array<float>> delayLines;
    juce::Array<int> writeIndex;
    juce::Array<int> readIndex;
};

} // namespace element
