// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/mediaplayer/timestretcher.hpp"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>

using namespace juce;
using RB = RubberBand::RubberBandStretcher;

namespace element {

TimeStretcher::TimeStretcher() = default;
TimeStretcher::~TimeStretcher() = default;

void TimeStretcher::prepare (double sampleRate, int numChannels, int maxBlockSize, Quality q)
{
    sr = sampleRate;
    channels = jmax (1, numChannels);
    maxBlock = jmax (64, maxBlockSize);
    quality = q;

    int options = RB::OptionProcessRealTime;
    options |= (q == Quality::HiFi ? RB::OptionEngineFiner : RB::OptionEngineFaster);
    // Faster engine: prefer percussive transient handling and crisp output.
    options |= RB::OptionTransientsCrisp;
    options |= RB::OptionPhaseLaminar;
    options |= RB::OptionWindowStandard;

    stretcher = std::make_unique<RB> ((size_t) sr,
                                      (size_t) channels,
                                      options,
                                      1.0 / std::max (currentRate, 1.0e-6),
                                      1.0);
    stretcher->setMaxProcessSize ((size_t) maxBlock);

    // Generous scratch for up to ~8x stretch / 1/8 compress.
    inputScratch.setSize (channels, maxBlock * 8, false, true, true);
    outputScratch.setSize (channels, maxBlock * 8, false, true, true);
    prepared = true;
}

void TimeStretcher::release()
{
    stretcher.reset();
    inputScratch.setSize (0, 0);
    outputScratch.setSize (0, 0);
    prepared = false;
}

void TimeStretcher::reset()
{
    if (stretcher)
        stretcher->reset();
}

void TimeStretcher::setPlaybackRate (double rate)
{
    rate = juce::jlimit (0.25, 4.0, rate);
    if (std::abs (rate - currentRate) < 1.0e-6)
        return;
    currentRate = rate;
    if (stretcher)
        stretcher->setTimeRatio (1.0 / rate);
}

bool TimeStretcher::process (AudioBuffer<float>& dest,
                             int destStart,
                             int numSamples,
                             std::function<int (AudioBuffer<float>&, int)> pullInput)
{
    if (! stretcher || numSamples <= 0)
        return false;

    const int ch = jmin (channels, dest.getNumChannels());

    int produced = 0;
    while (produced < numSamples)
    {
        // Make sure RubberBand has enough output queued; feed input until it does
        // or we've fed a sensible bound to avoid spinning.
        int safety = 8;
        while ((int) stretcher->available() < (numSamples - produced) && safety-- > 0)
        {
            int needed = (int) stretcher->getSamplesRequired();
            if (needed <= 0)
                needed = jmax (64, maxBlock / 4);
            needed = jmin (needed, inputScratch.getNumSamples());

            inputScratch.clear (0, needed);
            int got = pullInput (inputScratch, needed);
            if (got <= 0)
                break;
            // Zero out the tail if source returned fewer samples.
            if (got < needed)
                for (int c = 0; c < channels; ++c)
                    inputScratch.clear (c, got, needed - got);

            const float* pointers[8];
            const int chToUse = jmin (channels, 8);
            for (int c = 0; c < chToUse; ++c)
                pointers[c] = inputScratch.getReadPointer (c);
            stretcher->process (pointers, (size_t) needed, false);
        }

        int avail = (int) stretcher->available();
        if (avail <= 0)
        {
            // Couldn't produce any more output — give up to avoid stall.
            break;
        }

        int toTake = jmin (numSamples - produced, avail, outputScratch.getNumSamples());
        float* outPointers[8];
        const int chToUse = jmin (channels, 8);
        for (int c = 0; c < chToUse; ++c)
            outPointers[c] = outputScratch.getWritePointer (c);
        size_t got = stretcher->retrieve (outPointers, (size_t) toTake);

        for (int c = 0; c < ch; ++c)
            dest.copyFrom (c, destStart + produced, outputScratch, c, 0, (int) got);

        produced += (int) got;
        if (got == 0)
            break;
    }

    // Pad the tail with silence if we under-produced (avoids clicks).
    if (produced < numSamples)
    {
        for (int c = 0; c < ch; ++c)
            dest.clear (c, destStart + produced, numSamples - produced);
    }

    return produced > 0;
}

} // namespace element
