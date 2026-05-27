// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/element.hpp>
#include <element/audioengine.hpp>

namespace element {

class Context;

/** Mastering-oriented metering panel.

    Three sections, intended to live in the same bottom-of-the-graph slot as
    the meter bridge:

      - Loudness: ITU-R BS.1770-4 K-weighted momentary (400 ms) and short-term
        (3 s) LUFS readings.
      - Phase correlation: Pearson coefficient between L and R over a short
        window. Displayed as a horizontal bar from -1 (out of phase) to +1
        (mono-compatible).
      - Spectrogram: scrolling colored FFT, Renoise-style, with frequency on
        the vertical axis (log-scaled) and dB-encoded color.

    All analysis runs on the message thread from a lock-free tap on the
    master stereo output (see AudioEngine::MasterTap). The audio thread is
    not touched. */
class MeteringView : public juce::Component,
                     private juce::Timer
{
public:
    explicit MeteringView (Context&);
    ~MeteringView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // Sub-section painters (called from paint()).
    void paintLoudness (juce::Graphics&, juce::Rectangle<int>);
    void paintCorrelation (juce::Graphics&, juce::Rectangle<int>);
    void paintSpectrogram (juce::Graphics&, juce::Rectangle<int>);

    // Analysis helpers.
    void analyse();

    Context& context_;
    AudioEngine::MasterTapPtr tap;

    // K-weighting biquad state (per channel). The "K" weighting filter from
    // BS.1770-4 is a cascade of a high-shelf (1681 Hz, +4 dB) and an RLB
    // high-pass (38 Hz). Coefficients are sample-rate dependent and rebuilt
    // when sampleRate changes.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        inline double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        inline void reset() noexcept { z1 = z2 = 0; }
    };
    struct KChannel
    {
        Biquad hs; // high shelf
        Biquad rlb; // RLB high-pass
        void reset() { hs.reset(); rlb.reset(); }
    };
    KChannel kL, kR;

    void rebuildKWeighting (double sampleRate);

    // Sliding-window mean squares for momentary (400 ms) and short-term (3 s).
    // We push new K-weighted squared samples and pop expired ones from
    // ring buffers, holding the running sums separately to avoid recomputing.
    struct Window
    {
        juce::HeapBlock<float> buf;
        int size = 0;
        int pos = 0;
        double sum = 0.0;
        void prepare (int sz) { size = juce::jmax (1, sz); buf.calloc ((size_t) size); pos = 0; sum = 0.0; }
        void push (float v)
        {
            sum -= (double) buf[pos];
            sum += (double) v;
            buf[pos] = v;
            pos = (pos + 1) % size;
            if (sum < 0.0) sum = 0.0; // guard floating-point drift
        }
        double meanSquare() const { return sum / (double) size; }
    };

    Window momentaryW;  // 400 ms
    Window shortTermW;  // 3 s

    double currentSampleRate = 0.0;

    // Per-update results.
    float momentaryLUFS = -120.0f;
    float shortTermLUFS = -120.0f;
    float correlation  = 1.0f;
    float truePeakDb   = -120.0f;

    // Scratch buffers for tap reads (avoid allocations on the timer).
    juce::HeapBlock<float> readBufL, readBufR;
    int readBufCapacity = 0;

    // FFT for spectrogram. The order yields 2048 bins (1024 useful real bins).
    static constexpr int kFftOrder = 11; // 2^11 = 2048
    static constexpr int kFftSize  = 1 << kFftOrder;
    juce::dsp::FFT fft { kFftOrder };
    juce::HeapBlock<float> fftBuf;   // 2 * kFftSize for performFrequencyOnlyForwardTransform
    juce::HeapBlock<float> hann;     // window
    juce::HeapBlock<float> spectrum; // last computed magnitudes (kFftSize/2)

    // The spectrogram is a scrolling colored image: one column per analysis
    // tick. We keep an Image and shift-blit it left each frame, drawing a
    // single new column at the right edge.
    juce::Image spectrogramImage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeteringView)
};

} // namespace element
