// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace element {

/// Pro-grade waveform widget with draggable loop handles and a beat-grid overlay.
class WaveformDisplay : public juce::Component,
                        public juce::ChangeListener,
                        private juce::Timer
{
public:
    WaveformDisplay (juce::AudioFormatManager& formats);
    ~WaveformDisplay() override;

    void setAudioFile (const juce::File& file);
    void clearAudio();

    /// Live playhead position in seconds.
    void setPlayheadPosition (double seconds);

    /// Loop region in seconds. End <= 0 is treated as "to end of file".
    void setLoopRegion (double startSec, double endSec);
    void setLoopEnabled (bool shouldLoop);
    void setSnapEnabled (bool enabled);

    /// Beat grid (start of first beat in seconds, BPM). bpm == 0 hides grid.
    void setBeatGrid (double firstBeatSeconds, double bpm);

    double getLoopStart() const noexcept { return loopStart; }
    double getLoopEnd() const noexcept   { return loopEnd; }
    double getFileLength() const noexcept { return totalLength; }

    std::function<void (double startSec, double endSec)> onLoopChanged;
    std::function<void (double seekSec)> onSeekRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void timerCallback() override;

    enum class DragMode { None, LoopStart, LoopEnd, LoopRegion, Seek };

    double pixelToTime (int x) const;
    int timeToPixel (double t) const;
    double snapToBeat (double t, double thresholdSec) const;
    DragMode hitTest (juce::Point<int> p) const;
    void notifyLoopChanged();

    juce::AudioFormatManager& formats;
    juce::AudioThumbnailCache cache { 32 };
    juce::AudioThumbnail thumbnail;

    double totalLength = 0.0;
    double playhead = 0.0;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    bool   loopEnabled = false;

    double firstBeat = 0.0;
    double bpm = 0.0;
    bool   snapEnabled = true;

    DragMode dragMode = DragMode::None;
    double dragOffsetSec = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

} // namespace element
