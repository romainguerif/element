// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <vector>

namespace element {

class TempoAnalyzer : private juce::Thread
{
public:
    struct Result
    {
        double bpm = 0.0;
        double firstBeatSeconds = 0.0;
        std::vector<double> beatsSeconds;
        bool valid = false;
    };

    using Callback = std::function<void (Result)>;

    TempoAnalyzer();
    ~TempoAnalyzer() override;

    void analyze (const juce::File& audioFile,
                  juce::AudioFormatManager& formats,
                  Callback whenDone);

    void cancel();

    bool isBusy() const noexcept { return busy.load(); }

private:
    void run() override;

    juce::File pendingFile;
    juce::AudioFormatManager* pendingFormats = nullptr;
    Callback pendingCallback;
    std::atomic<bool> busy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoAnalyzer)
};

} // namespace element
