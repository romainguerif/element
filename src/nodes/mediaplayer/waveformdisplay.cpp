// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/mediaplayer/waveformdisplay.hpp"

#include <algorithm>
#include <cmath>

using namespace juce;

namespace element {

namespace {
constexpr int kHandleHitPixels = 7;
constexpr int kHeaderHeight = 14;
} // namespace

WaveformDisplay::WaveformDisplay (AudioFormatManager& fm)
    : formats (fm),
      thumbnail (512, fm, cache)
{
    thumbnail.addChangeListener (this);
    setMouseCursor (MouseCursor::NormalCursor);
    setOpaque (true);
    startTimerHz (30);
}

WaveformDisplay::~WaveformDisplay()
{
    thumbnail.removeChangeListener (this);
}

void WaveformDisplay::setAudioFile (const File& file)
{
    if (! file.existsAsFile())
    {
        clearAudio();
        return;
    }

    thumbnail.setSource (new FileInputSource (file));
    totalLength = thumbnail.getTotalLength();
    playhead = 0.0;
    loopStart = 0.0;
    loopEnd = totalLength;
    repaint();
}

void WaveformDisplay::clearAudio()
{
    thumbnail.setSource (nullptr);
    totalLength = 0.0;
    playhead = 0.0;
    loopStart = 0.0;
    loopEnd = 0.0;
    firstBeat = 0.0;
    bpm = 0.0;
    repaint();
}

void WaveformDisplay::setPlayheadPosition (double seconds)
{
    if (std::abs (seconds - playhead) > 1.0e-4)
    {
        playhead = seconds;
        repaint();
    }
}

void WaveformDisplay::setLoopRegion (double startSec, double endSec)
{
    loopStart = jmax (0.0, startSec);
    if (endSec <= 0.0)
        loopEnd = totalLength;
    else
        loopEnd = jmin (totalLength > 0.0 ? totalLength : endSec, endSec);
    repaint();
}

void WaveformDisplay::setLoopEnabled (bool shouldLoop)
{
    if (loopEnabled != shouldLoop)
    {
        loopEnabled = shouldLoop;
        repaint();
    }
}

void WaveformDisplay::setSnapEnabled (bool enabled)
{
    snapEnabled = enabled;
}

void WaveformDisplay::setBeatGrid (double firstBeatSeconds, double newBpm)
{
    firstBeat = firstBeatSeconds;
    bpm = newBpm;
    repaint();
}

double WaveformDisplay::pixelToTime (int x) const
{
    if (getWidth() <= 0 || totalLength <= 0.0)
        return 0.0;
    return jlimit (0.0, totalLength, (double) x / getWidth() * totalLength);
}

int WaveformDisplay::timeToPixel (double t) const
{
    if (totalLength <= 0.0)
        return 0;
    return (int) std::round (t / totalLength * getWidth());
}

double WaveformDisplay::snapToBeat (double t, double thresholdSec) const
{
    if (bpm <= 0.0)
        return t;
    // Snap to BAR boundaries (every 4 beats) — that's the musically
    // useful grain for loop points.
    const double barLen = (60.0 / bpm) * 4.0;
    const double k = std::round ((t - firstBeat) / barLen);
    const double snapped = firstBeat + k * barLen;
    return std::abs (snapped - t) <= thresholdSec ? snapped : t;
}

WaveformDisplay::DragMode WaveformDisplay::hitTest (Point<int> p) const
{
    if (totalLength <= 0.0)
        return DragMode::None;

    // Loop handles always take priority — they are always visible, so
    // they must always be grabbable. Anything else seeks the playhead.
    const int xs = timeToPixel (loopStart);
    const int xe = timeToPixel (loopEnd);

    if (std::abs (p.x - xs) <= kHandleHitPixels)
        return DragMode::LoopStart;
    if (std::abs (p.x - xe) <= kHandleHitPixels)
        return DragMode::LoopEnd;

    return DragMode::Seek;
}

void WaveformDisplay::notifyLoopChanged()
{
    if (onLoopChanged)
        onLoopChanged (loopStart, loopEnd);
}

void WaveformDisplay::mouseMove (const MouseEvent& e)
{
    switch (hitTest (e.getPosition()))
    {
        case DragMode::LoopStart:
        case DragMode::LoopEnd:
            setMouseCursor (MouseCursor::LeftRightResizeCursor);
            break;
        case DragMode::Seek:
            setMouseCursor (MouseCursor::IBeamCursor);
            break;
        default:
            setMouseCursor (MouseCursor::NormalCursor);
            break;
    }
}

void WaveformDisplay::mouseDown (const MouseEvent& e)
{
    if (totalLength <= 0.0)
        return;
    dragMode = hitTest (e.getPosition());

    // Single-click seeking — fire immediately so the user can tap-to-set
    // the playhead, not only via drag.
    if (dragMode == DragMode::Seek && onSeekRequested)
    {
        const double t = pixelToTime (e.x);
        onSeekRequested (t);
    }
}

void WaveformDisplay::mouseDrag (const MouseEvent& e)
{
    if (totalLength <= 0.0 || dragMode == DragMode::None)
        return;

    // Snap is on by user choice AND not overridden by shift. Threshold
    // is generous when snap is on (snap from anywhere on the beat) and
    // disabled when off.
    const bool useSnap = snapEnabled && ! e.mods.isShiftDown();
    double t = pixelToTime (e.x);
    if (useSnap)
    {
        // When the toggle is on, snap from arbitrarily far — i.e. always
        // pull to the nearest bar regardless of pixel distance.
        t = snapToBeat (t, 1.0e9);
    }

    switch (dragMode)
    {
        case DragMode::LoopStart:
            loopStart = jlimit (0.0, loopEnd - 0.01, t);
            // Push to the processor every frame so the editor's 60Hz
            // timer doesn't snap our in-progress drag back.
            notifyLoopChanged();
            break;
        case DragMode::LoopEnd:
            loopEnd = jlimit (loopStart + 0.01, totalLength, t);
            notifyLoopChanged();
            break;
        case DragMode::Seek:
            if (onSeekRequested)
                onSeekRequested (t);
            break;
        default:
            break;
    }
    repaint();
}

void WaveformDisplay::mouseUp (const MouseEvent&)
{
    if (dragMode == DragMode::LoopStart
        || dragMode == DragMode::LoopEnd
        || dragMode == DragMode::LoopRegion)
        notifyLoopChanged();
    dragMode = DragMode::None;
}

void WaveformDisplay::mouseDoubleClick (const MouseEvent&)
{
    loopStart = 0.0;
    loopEnd = totalLength;
    notifyLoopChanged();
    repaint();
}

void WaveformDisplay::changeListenerCallback (ChangeBroadcaster*)
{
    if (totalLength <= 0.0 && thumbnail.getTotalLength() > 0.0)
    {
        totalLength = thumbnail.getTotalLength();
        loopEnd = totalLength;
    }
    repaint();
}

void WaveformDisplay::timerCallback()
{
    // Setting setPlayheadPosition already triggers a repaint when the
    // playhead actually moves, so this timer doesn't need to do anything
    // on its own. We keep the timer alive so that if a parent or sibling
    // forgets to drive us, we still tick — but no unconditional repaint.
}

void WaveformDisplay::resized() {}

void WaveformDisplay::paint (Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background.
    g.setGradientFill (ColourGradient (Colour (0xff15191e), 0, 0,
                                       Colour (0xff0c0e12), 0, (float) bounds.getHeight(), false));
    g.fillAll();

    auto waveArea = bounds.withTrimmedTop (kHeaderHeight);

    // Center line.
    g.setColour (Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine (waveArea.getCentreY(), (float) waveArea.getX(), (float) waveArea.getRight());

    // Sanity-check the length the thumbnail reports — guards against
    // bogus values during the early loading phase (NaN, negative,
    // gigantic) that would corrupt the grid math below.
    const double safeLen = (std::isfinite (totalLength) && totalLength > 0.0 && totalLength < 24.0 * 3600.0)
                               ? totalLength : 0.0;

    // Waveform.
    if (safeLen > 0.0)
    {
        g.setGradientFill (ColourGradient (Colour (0xff5cc8ff), 0.0f, (float) waveArea.getY(),
                                           Colour (0xff2879d0), 0.0f, (float) waveArea.getBottom(),
                                           false));
        try
        {
            thumbnail.drawChannels (g, waveArea, 0.0, safeLen, 0.95f);
        }
        catch (...)
        {
            // If the thumbnail's background reader hit a malformed chunk
            // we may end up here. Better to skip the draw than crash.
        }
    }
    else
    {
        g.setColour (Colours::white.withAlpha (0.35f));
        g.setFont (Font (FontOptions (13.0f)));
        g.drawText ("Drop an audio file here", waveArea, Justification::centred);
        return;
    }

    // Beat grid (only if BPM is sane).
    if (std::isfinite (bpm) && bpm > 20.0 && bpm < 400.0
        && std::isfinite (firstBeat) && firstBeat >= -3600.0 && firstBeat <= safeLen)
    {
        const double beatLen = 60.0 / bpm;
        if (beatLen > 0.01 && beatLen < 10.0)
        {
            // Walk forward from the first visible beat at or before t=0.
            double t = firstBeat;
            while (t > 0.0) t -= beatLen;          // step back to first onset <= 0
            int beatNumber = (int) std::round ((t - firstBeat) / beatLen);

            // Safety cap on iterations — never draw more than 4096 beats.
            int safety = 4096;
            while (t < safeLen && safety-- > 0)
            {
                if (t >= 0.0)
                {
                    const bool isBar = (beatNumber % 4) == 0;
                    g.setColour (isBar ? Colours::white.withAlpha (0.35f)
                                       : Colours::white.withAlpha (0.12f));
                    const int x = timeToPixel (t);
                    g.drawVerticalLine (x, (float) waveArea.getY(), (float) waveArea.getBottom());

                    if (isBar)
                    {
                        const auto barIdx = beatNumber / 4 + 1;
                        g.setColour (Colours::white.withAlpha (0.55f));
                        g.setFont (Font (FontOptions (10.0f)));
                        g.drawText (String (barIdx),
                                    x + 2, 0, 28, kHeaderHeight,
                                    Justification::centredLeft);
                    }
                }
                t += beatLen;
                ++beatNumber;
            }
        }
    }

    // Loop region overlay.
    if (safeLen > 0.0 && loopEnd > loopStart
        && std::isfinite (loopStart) && std::isfinite (loopEnd))
    {
        const int xs = timeToPixel (jlimit (0.0, safeLen, loopStart));
        const int xe = timeToPixel (jlimit (0.0, safeLen, loopEnd));
        const auto regionRect = Rectangle<int> (xs, waveArea.getY(),
                                                jmax (1, xe - xs), waveArea.getHeight());

        Colour fill = (loopEnabled ? Colours::yellow : Colours::lightblue).withAlpha (0.10f);
        g.setColour (fill);
        g.fillRect (regionRect);

        Colour border = (loopEnabled ? Colours::yellow : Colours::lightblue).withAlpha (0.65f);
        g.setColour (border);
        g.drawVerticalLine (xs, (float) waveArea.getY(), (float) waveArea.getBottom());
        g.drawVerticalLine (xe, (float) waveArea.getY(), (float) waveArea.getBottom());

        // Handle "flags".
        g.fillRect (xs - 4, waveArea.getY(), 8, 6);
        g.fillRect (xs - 4, waveArea.getBottom() - 6, 8, 6);
        g.fillRect (xe - 4, waveArea.getY(), 8, 6);
        g.fillRect (xe - 4, waveArea.getBottom() - 6, 8, 6);
    }

    // Playhead.
    if (safeLen > 0.0 && std::isfinite (playhead) && playhead >= 0.0)
    {
        const int xp = timeToPixel (jlimit (0.0, safeLen, playhead));
        g.setColour (Colours::white.withAlpha (0.85f));
        g.drawVerticalLine (xp, (float) waveArea.getY(), (float) waveArea.getBottom());
    }
}

} // namespace element
