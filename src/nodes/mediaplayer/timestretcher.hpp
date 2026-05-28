// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>

namespace RubberBand { class RubberBandStretcher; }

namespace element {

/// Real-time time-stretch wrapper around Rubber Band.
/// Pulls samples on demand from a callable that returns next input block.
class TimeStretcher
{
public:
    enum class Quality
    {
        Eco,  // R2 / Faster
        HiFi  // R3 / Finer
    };

    TimeStretcher();
    ~TimeStretcher();

    /// (Re)configure for sampleRate / channel count / quality.
    void prepare (double sampleRate, int numChannels, int maxBlockSize, Quality q);
    void release();

    /// Drop internal buffers. Use when seeking or after a discontinuity.
    void reset();

    /// Set playback rate: 1.0 = original speed. Internally Rubber Band's
    /// time-ratio is the inverse (timeRatio = 1.0 / rate).
    void setPlaybackRate (double rate);
    double getPlaybackRate() const noexcept { return currentRate; }

    /// Pull `numSamples` of stretched output into `dest`.
    /// `pullInput(scratch, numSamplesNeeded)` must fill `scratch` with that many
    /// input samples from the source (returns actual samples filled — short reads zero-pad).
    /// Returns true if any output was produced.
    bool process (juce::AudioBuffer<float>& dest,
                  int destStart,
                  int numSamples,
                  std::function<int (juce::AudioBuffer<float>& scratch, int needed)> pullInput);

    /// Whether prepare has been called and the stretcher is ready.
    bool isPrepared() const noexcept { return prepared; }

private:
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    juce::AudioBuffer<float> inputScratch;
    juce::AudioBuffer<float> outputScratch;
    double sr = 44100.0;
    int channels = 2;
    int maxBlock = 512;
    Quality quality = Quality::Eco;
    double currentRate = 1.0;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeStretcher)
};

} // namespace element
