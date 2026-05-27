// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/meteringview.hpp"

#include <element/context.hpp>

namespace element {

namespace {
constexpr int kUpdateHz = 24;

// Renoise "Spectrum" view spectrogram palette. Renoise uses a heat-map
// gradient that sweeps from black through deep blue / violet, into bright
// cyan, then green, yellow, orange-red, and finishes near-white on peaks.
// The exact stops below were tuned to visually match captured screenshots
// of Renoise's default spectrogram colour scheme.
inline juce::Colour spectrogramColour (float t)
{
    t = juce::jlimit (0.0f, 1.0f, t);
    struct Stop { float pos; juce::Colour col; };
    static const Stop stops[] = {
        { 0.00f, juce::Colour::fromRGB (  0,   0,   0) }, // silence
        { 0.10f, juce::Colour::fromRGB ( 12,   8,  56) }, // very dark indigo
        { 0.22f, juce::Colour::fromRGB ( 30,  35, 160) }, // deep blue
        { 0.36f, juce::Colour::fromRGB (  0, 150, 220) }, // bright cyan
        { 0.52f, juce::Colour::fromRGB ( 60, 220,  80) }, // green
        { 0.68f, juce::Colour::fromRGB (255, 230,   0) }, // yellow
        { 0.83f, juce::Colour::fromRGB (255,  90,   0) }, // orange-red
        { 0.95f, juce::Colour::fromRGB (255,  40,  40) }, // red
        { 1.00f, juce::Colour::fromRGB (255, 255, 255) }  // clip
    };
    constexpr int n = (int) (sizeof (stops) / sizeof (stops[0]));
    for (int i = 1; i < n; ++i)
    {
        if (t <= stops[i].pos)
        {
            const float span = stops[i].pos - stops[i - 1].pos;
            const float local = span > 0.0f ? (t - stops[i - 1].pos) / span : 0.0f;
            return stops[i - 1].col.interpolatedWith (stops[i].col, local);
        }
    }
    return stops[n - 1].col;
}
} // namespace

//==============================================================================
MeteringView::MeteringView (Context& ctx)
    : context_ (ctx)
{
    setOpaque (true);

    if (auto engine = ctx.audio())
        tap = engine->getMasterTap();

    // Default samplerate-dependent windows will be re-prepared once we see a
    // live sample rate from the tap.
    rebuildKWeighting (48000.0);
    momentaryW.prepare (48000 * 400 / 1000);
    shortTermW.prepare (48000 * 3);

    fftBuf.calloc ((size_t) kFftSize * 2);
    hann.calloc ((size_t) kFftSize);
    spectrum.calloc ((size_t) (kFftSize / 2));
    for (int i = 0; i < kFftSize; ++i)
        hann[i] = 0.5f * (1.0f - std::cos ((float) juce::MathConstants<double>::twoPi
                                            * (float) i / (float) (kFftSize - 1)));

    startTimerHz (kUpdateHz);
}

MeteringView::~MeteringView() = default;

//==============================================================================
void MeteringView::resized()
{
    // Spectrogram image follows the panel width.
    const int sw = juce::jmax (16, getWidth() - 360);
    const int sh = juce::jmax (16, getHeight() - 16);
    if (spectrogramImage.getWidth() != sw || spectrogramImage.getHeight() != sh)
    {
        spectrogramImage = juce::Image (juce::Image::RGB, sw, sh, true);
    }
}

void MeteringView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171a));

    auto r = getLocalBounds().reduced (6);
    auto loudness  = r.removeFromLeft (150);
    r.removeFromLeft (6);
    auto correlationArea = r.removeFromLeft (180);
    r.removeFromLeft (6);
    auto spectro   = r;

    paintLoudness (g, loudness);
    paintCorrelation (g, correlationArea);
    paintSpectrogram (g, spectro);
}

//==============================================================================
void MeteringView::paintLoudness (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff202327));
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText ("LOUDNESS  (LUFS)", area.removeFromTop (16), juce::Justification::centred);

    auto inner = area.reduced (8);

    // Two vertical bars.
    auto leftBar = inner.removeFromLeft (inner.getWidth() / 2 - 4);
    inner.removeFromLeft (8);
    auto rightBar = inner;

    auto drawBar = [&] (juce::Rectangle<int> r, float lufs, const juce::String& label) {
        // Scale: -60 LUFS at bottom, 0 LUFS at top.
        const float minDb = -60.0f, maxDb = 0.0f;
        const float v = juce::jlimit (minDb, maxDb, lufs);
        const float n = (v - minDb) / (maxDb - minDb);

        g.setColour (juce::Colour (0xff0d0e10));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff262a2f));
        g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);

        const int barH = juce::roundToInt ((float) r.getHeight() * n);
        auto bar = r.withTrimmedTop (r.getHeight() - barH).reduced (3);

        // Neutral white/grey fill -- coloured meters disabled for now per
        // user request; bright shows higher level via more saturated white.
        g.setColour (juce::Colours::white.withAlpha (0.78f));
        g.fillRoundedRectangle (bar.toFloat(), 2.0f);

        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (label, r.withHeight (12), juce::Justification::centred);

        auto valText = juce::String (lufs, 1);
        g.setColour (juce::Colours::white);
        g.drawText (valText, r.withTrimmedTop (r.getHeight() - 14),
                    juce::Justification::centred);
    };

    drawBar (leftBar, momentaryLUFS, "M");
    drawBar (rightBar, shortTermLUFS, "S");
}

void MeteringView::paintCorrelation (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff202327));
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText ("PHASE  CORRELATION", area.removeFromTop (16), juce::Justification::centred);

    auto bar = area.reduced (8).withHeight (18).withCentre (area.getCentre());

    g.setColour (juce::Colour (0xff0d0e10));
    g.fillRoundedRectangle (bar.toFloat(), 3.0f);
    g.setColour (juce::Colour (0xff262a2f));
    g.drawRoundedRectangle (bar.toFloat(), 3.0f, 1.0f);

    // Map correlation [-1, +1] to bar position.
    const float c = juce::jlimit (-1.0f, 1.0f, correlation);
    const int cx = bar.getX() + juce::roundToInt ((float) bar.getWidth() * (c + 1.0f) * 0.5f);

    // Tick marks: -1, 0, +1.
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawLine ((float) bar.getX(), (float) bar.getCentreY(),
                (float) bar.getRight(), (float) bar.getCentreY(), 1.0f);
    for (float k : { -1.0f, 0.0f, 1.0f })
    {
        const int x = bar.getX() + juce::roundToInt ((float) bar.getWidth() * (k + 1.0f) * 0.5f);
        g.drawLine ((float) x, (float) bar.getY(), (float) x, (float) bar.getBottom(), 1.0f);
    }

    // Cursor -- neutral white for now (no coloured meters per user request).
    g.setColour (juce::Colours::white);
    g.fillEllipse ((float) cx - 5.0f, (float) bar.getCentreY() - 5.0f, 10.0f, 10.0f);

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText (juce::String (c, 2),
                area.withTrimmedTop (bar.getBottom() - area.getY() + 4).withHeight (14),
                juce::Justification::centred);
}

void MeteringView::paintSpectrogram (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff202327));
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText ("SPECTROGRAM  (20 Hz ... 20 kHz)", area.removeFromTop (16),
                juce::Justification::centred);

    auto inner = area.reduced (4);
    if (spectrogramImage.isValid())
        g.drawImage (spectrogramImage, inner.toFloat(),
                     juce::RectanglePlacement::stretchToFit);
}

//==============================================================================
void MeteringView::rebuildKWeighting (double sr)
{
    if (sr <= 0.0)
        return;
    currentSampleRate = sr;

    // BS.1770-4 K-weighting -- pre-filter (high-shelf around 1681 Hz, +4 dB)
    // followed by RLB (RC high-pass at ~38 Hz). Coefficients below are the
    // standard Direct-Form-II Transposed digital biquad coefficients given
    // by ITU and reproduced in many open-source loudness libraries (libebur128).
    {
        // High-shelf
        const double f0 = 1681.974450955533;
        const double G  = 3.999843853973347;
        const double Q  = 0.7071752369554196;

        const double K  = std::tan (juce::MathConstants<double>::pi * f0 / sr);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);

        const double a0_ = 1.0 + K / Q + K * K;
        kL.hs.b0 = (Vh + Vb * K / Q + K * K) / a0_;
        kL.hs.b1 = 2.0 * (K * K - Vh) / a0_;
        kL.hs.b2 = (Vh - Vb * K / Q + K * K) / a0_;
        kL.hs.a1 = 2.0 * (K * K - 1.0) / a0_;
        kL.hs.a2 = (1.0 - K / Q + K * K) / a0_;
        kR.hs = kL.hs;
        kL.hs.reset(); kR.hs.reset();
    }
    {
        // RLB high-pass
        const double f0 = 38.13547087602444;
        const double Q  = 0.5003270373238773;
        const double K  = std::tan (juce::MathConstants<double>::pi * f0 / sr);
        const double a0_ = 1.0 + K / Q + K * K;
        kL.rlb.b0 = 1.0;
        kL.rlb.b1 = -2.0;
        kL.rlb.b2 = 1.0;
        kL.rlb.a1 = 2.0 * (K * K - 1.0) / a0_;
        kL.rlb.a2 = (1.0 - K / Q + K * K) / a0_;
        // Coefficient scaling so b0=1 reflects the inherent gain of the RLB
        // section (libebur128 uses these literal forms).
        const double scale = 1.0 / a0_;
        kL.rlb.b0 *= scale;
        kL.rlb.b1 *= scale;
        kL.rlb.b2 *= scale;
        kR.rlb = kL.rlb;
        kL.rlb.reset(); kR.rlb.reset();
    }

    momentaryW.prepare ((int) std::round (sr * 0.4));
    shortTermW.prepare ((int) std::round (sr * 3.0));

    const int cap = juce::jmax ((int) std::round (sr * 0.5), kFftSize);
    if (cap > readBufCapacity)
    {
        readBufCapacity = cap;
        readBufL.calloc ((size_t) cap);
        readBufR.calloc ((size_t) cap);
    }
}

//==============================================================================
void MeteringView::timerCallback()
{
    analyse();
    repaint();
}

void MeteringView::analyse()
{
    if (tap == nullptr)
        return;

    const double sr = tap->sampleRate();
    if (sr <= 0.0)
        return;
    if (! juce::approximatelyEqual (sr, currentSampleRate))
        rebuildKWeighting (sr);

    // Read enough samples for one frame's worth of analysis. At 24 Hz refresh
    // and 48 kHz, that's 2000 samples; at 192 kHz, 8000. Cap at the FFT size
    // so we never under-feed the spectrogram.
    const int frameSamples = juce::jmax (kFftSize, (int) std::round (sr / kUpdateHz));
    const int n = tap->readLatest (readBufL.getData(), readBufR.getData(),
                                    juce::jmin (frameSamples, readBufCapacity));
    if (n <= 0)
        return;

    // --- LUFS via K-weighting + sliding mean square. ----------------------
    for (int i = 0; i < n; ++i)
    {
        const double xL = readBufL[i];
        const double xR = readBufR[i];
        const double yL = kL.rlb.process (kL.hs.process (xL));
        const double yR = kR.rlb.process (kR.hs.process (xR));
        const float sq = (float) (yL * yL + yR * yR); // stereo sum, BS.1770 channel weighting (1.0 for L/R)
        momentaryW.push (sq);
        shortTermW.push (sq);
    }
    const double msM = momentaryW.meanSquare();
    const double msS = shortTermW.meanSquare();
    momentaryLUFS = (msM > 1e-12) ? (float) (-0.691 + 10.0 * std::log10 (msM)) : -120.0f;
    shortTermLUFS = (msS > 1e-12) ? (float) (-0.691 + 10.0 * std::log10 (msS)) : -120.0f;

    // --- Correlation (Pearson over the analysis window). ------------------
    {
        // Use the latest min(n, sr*0.1) samples == ~100 ms reaction.
        const int corrN = juce::jmin (n, (int) std::round (sr * 0.1));
        const int offset = n - corrN;
        double mL = 0, mR = 0;
        for (int i = 0; i < corrN; ++i) { mL += readBufL[offset + i]; mR += readBufR[offset + i]; }
        mL /= corrN; mR /= corrN;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < corrN; ++i)
        {
            const double dl = readBufL[offset + i] - mL;
            const double dr = readBufR[offset + i] - mR;
            sxy += dl * dr;
            sxx += dl * dl;
            syy += dr * dr;
        }
        const double denom = std::sqrt (sxx * syy);
        correlation = (denom > 1e-12) ? (float) (sxy / denom) : 1.0f;
    }

    // --- Spectrogram: FFT of the most recent kFftSize samples (sum L+R). --
    {
        const int off = juce::jmax (0, n - kFftSize);
        // Sum L+R for a mono spectrum (typical mastering view).
        for (int i = 0; i < kFftSize; ++i)
        {
            const float l = readBufL[off + i];
            const float r = readBufR[off + i];
            fftBuf[i] = 0.5f * (l + r) * hann[i];
        }
        std::fill_n (fftBuf.getData() + kFftSize, (size_t) kFftSize, 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftBuf.getData());

        for (int i = 0; i < kFftSize / 2; ++i)
            spectrum[i] = fftBuf[i];

        // Scroll the image left by 1 px and draw a fresh column on the right.
        if (spectrogramImage.isValid())
        {
            juce::Graphics gImg (spectrogramImage);
            const int w = spectrogramImage.getWidth();
            const int h = spectrogramImage.getHeight();

            // Shift left by one pixel column by drawing the image on itself
            // offset by -1 in x. Using an intermediate copy avoids self-blit
            // artifacts.
            juce::Image copy (spectrogramImage.createCopy());
            gImg.drawImageAt (copy, -1, 0);

            // Render the new column at the right edge.
            const int colX = w - 1;
            const float nyquist = (float) (sr * 0.5);

            for (int y = 0; y < h; ++y)
            {
                // Log-mapped frequency from top (high) to bottom (low).
                const float t = 1.0f - (float) y / (float) h;
                // 20 Hz .. nyquist mapped log.
                const float fLow = 20.0f;
                const float fHigh = juce::jmax (fLow + 1.0f, nyquist);
                const float freq = fLow * std::pow (fHigh / fLow, t);
                // FFT bin index.
                const float binF = freq / ((float) sr / (float) kFftSize);
                const int bin = juce::jlimit (0, kFftSize / 2 - 1, (int) std::round (binF));
                // Convert magnitude to dB, normalise.
                const float mag = spectrum[bin];
                const float db = 20.0f * std::log10 (juce::jmax (mag, 1e-9f) / (float) (kFftSize / 2));
                // -90 dB .. 0 dB
                const float norm = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f);

                gImg.setColour (spectrogramColour (norm));
                gImg.fillRect (colX, y, 1, 1);
            }
        }
    }
}

} // namespace element
