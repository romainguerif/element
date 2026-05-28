// SPDX-FileCopyrightText: 2026 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/mediaplayer/tempoanalyzer.hpp"
#include "crashdiagnostics.hpp"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "dsp/onsets/DetectionFunction.h"
#include "dsp/tempotracking/TempoTrackV2.h"

using namespace juce;

namespace element {

namespace {

constexpr int kStepSize = 512;
constexpr int kFrameLength = 1024;

double medianOf (std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort (values.begin(), values.end());
    const auto n = values.size();
    return (n % 2 == 0) ? 0.5 * (values[n / 2 - 1] + values[n / 2])
                        : values[n / 2];
}

} // namespace

TempoAnalyzer::TempoAnalyzer()
    : juce::Thread ("element.TempoAnalyzer")
{
}

TempoAnalyzer::~TempoAnalyzer()
{
    cancel();
}

void TempoAnalyzer::cancel()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        stopThread (2000);
    }
}

void TempoAnalyzer::analyze (const File& audioFile,
                             AudioFormatManager& formats,
                             Callback whenDone)
{
    cancel();
    pendingFile = audioFile;
    pendingFormats = &formats;
    pendingCallback = std::move (whenDone);
    busy.store (true);
    startThread();
}

void TempoAnalyzer::run()
{
    Result result;
    auto fireCallback = [this] (Result r) {
        busy.store (false);
        if (pendingCallback)
        {
            auto cb = std::move (pendingCallback);
            MessageManager::callAsync ([cb, r] { cb (r); });
        }
    };

    if (pendingFormats == nullptr || ! pendingFile.existsAsFile())
    {
        fireCallback (result);
        return;
    }

    try
    {

    std::unique_ptr<AudioFormatReader> reader (pendingFormats->createReaderFor (pendingFile));
    if (reader == nullptr
        || reader->numChannels < 1
        || reader->numChannels > 8
        || reader->sampleRate < 8000.0
        || reader->sampleRate > 192000.0
        || reader->lengthInSamples <= kFrameLength)
    {
        fireCallback (result);
        return;
    }

    const auto sampleRate = reader->sampleRate;
    // Cap analysis at 10 minutes of audio — enough for any clip and
    // avoids exploding memory on hour-long files.
    const auto maxAnalysisSamples = (juce::int64) (sampleRate * 600.0);
    const auto totalSamples = juce::jmin (reader->lengthInSamples, maxAnalysisSamples);

    DFConfig dfConfig;
    dfConfig.DFType = DF_COMPLEXSD;
    dfConfig.stepSize = kStepSize;
    dfConfig.frameLength = kFrameLength;
    dfConfig.dbRise = 3.0;
    dfConfig.adaptiveWhitening = false;
    dfConfig.whiteningRelaxCoeff = -1.0;
    dfConfig.whiteningFloor = -1.0;

    DetectionFunction df (dfConfig);

    AudioBuffer<float> readBuffer ((int) reader->numChannels, kFrameLength);
    std::vector<double> mono ((size_t) kFrameLength, 0.0);
    std::vector<double> dfValues;
    dfValues.reserve ((size_t) (totalSamples / kStepSize + 1));

    juce::int64 pos = 0;
    while (pos + kFrameLength <= totalSamples)
    {
        if (threadShouldExit())
        {
            fireCallback (result);
            return;
        }

        readBuffer.clear();
        if (! reader->read (&readBuffer, 0, kFrameLength, pos, true, true))
            break;

        const auto numCh = readBuffer.getNumChannels();
        if (numCh == 1)
        {
            const auto* src = readBuffer.getReadPointer (0);
            for (int i = 0; i < kFrameLength; ++i)
                mono[(size_t) i] = (double) src[i];
        }
        else
        {
            const auto* l = readBuffer.getReadPointer (0);
            const auto* r = readBuffer.getReadPointer (1);
            for (int i = 0; i < kFrameLength; ++i)
                mono[(size_t) i] = 0.5 * (double) (l[i] + r[i]);
        }

        dfValues.push_back (df.processTimeDomain (mono.data()));
        pos += kStepSize;
    }

    if (dfValues.size() < 16)
    {
        fireCallback (result);
        return;
    }

    TempoTrackV2 tt ((float) sampleRate, kStepSize);
    // CRITICAL: TempoTrackV2 does NOT resize these output vectors itself;
    // viterbi_decode writes up to df.size() entries into beatPeriod via
    // raw indexing. Caller must pre-size to dfValues.size() (Mixxx and
    // the QM Vamp plugins both do this). Passing empty vectors causes
    // out-of-bounds writes and a near-instant crash.
    std::vector<double> beatPeriod (dfValues.size(), 0.0);
    std::vector<double> tempi;
    std::vector<double> beatFrames;
    tt.calculateBeatPeriod (dfValues, beatPeriod, tempi);
    tt.calculateBeats (dfValues, beatPeriod, beatFrames);

    if (beatFrames.size() < 2)
    {
        fireCallback (result);
        return;
    }

    const double frameToSec = (double) kStepSize / sampleRate;
    result.beatsSeconds.resize (beatFrames.size());
    for (size_t i = 0; i < beatFrames.size(); ++i)
        result.beatsSeconds[i] = beatFrames[i] * frameToSec;

    std::vector<double> iois;
    iois.reserve (result.beatsSeconds.size());
    for (size_t i = 1; i < result.beatsSeconds.size(); ++i)
        iois.push_back (result.beatsSeconds[i] - result.beatsSeconds[i - 1]);

    const double medianIoi = medianOf (std::move (iois));
    if (medianIoi <= 0.0)
    {
        fireCallback (result);
        return;
    }

    result.bpm = 60.0 / medianIoi;
    result.firstBeatSeconds = result.beatsSeconds.front();
    result.valid = result.bpm > 30.0 && result.bpm < 300.0;
    fireCallback (result);

    } // try
    catch (...)
    {
        // Defensive: a malformed file or unexpected QM-DSP state cannot
        // be allowed to take down the host. Drop a breadcrumb at the
        // crash-log file directly so we still see something next time.
        element::diagnostics::breadcrumb ("tempo", "exception in analyzer");
        fireCallback (Result {});
    }
}

} // namespace element
