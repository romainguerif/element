// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audiomixer.hpp"
#include "nodes/audiomixereditor.hpp"

#include <element/ui/style.hpp>

#include <array>
#include <cmath>
#include <mutex>

namespace element {

namespace {

inline float kneeKnobToGain (float k)
{
    // Maps the -1..+1 EQ knob to a linear gain.
    //   +1  -> +6 dB
    //    0  -> unity (0 dB)
    //   -1  -> kill (-60 dB ≈ 0.001)
    //
    // CRITICAL: the gain factor must NEVER be exactly 0. A JUCE shelf
    // or peak biquad designed with gainFactor=0 collapses to b0=b1=b2=0
    // (low-shelf) or normalizes via a0=Inf (peak/high-shelf) — both
    // produce silence at every frequency, not just the targeted band.
    // We use a -200 dB floor on decibelsToGain and a hard min of 0.001
    // on the result so the biquad math stays well-defined.
    if (k >= 0.0f)
        return juce::Decibels::decibelsToGain (k * 6.0f);
    const float dB = k * 60.0f;
    return juce::jmax (0.001f, juce::Decibels::decibelsToGain (dB, -200.0f));
}

inline float panLawL (float pan)  // pan in -1..+1, -3dB constant power
{
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    const float a = (p + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    return std::cos (a);
}

inline float panLawR (float pan)
{
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    const float a = (p + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    return std::sin (a);
}

//==============================================================================
// Drive waveshapers — operate on already-oversampled samples.
//
// Each returns the wet (driven) output for a single sample, given a "drive"
// scaling (1.0 = unity, higher = harder). Mode-specific pre/post EQ and DC
// blockers are applied OUTSIDE this function in processBlock — keep these
// curves pure so they can be inlined and SIMD'd later.

inline float shapeTape (float x, float drive)
{
    // Symmetric tanh with tiny asymmetric bias for a touch of 2nd harmonic.
    // Bias term is removed analytically so no DC reaches the output.
    constexpr float bias = 0.03f;
    const float gx   = drive * (x + bias);
    const float gnb  = drive * bias;
    const float norm = std::tanh (drive);
    return (std::tanh (gx) - std::tanh (gnb)) / juce::jmax (1.0e-6f, norm);
}

inline float shapeTube (float x, float drive)
{
    // Asymmetric: positive half tanh at full drive, negative half softer (0.7x).
    // Produces strong 2nd-harmonic, the defining tube quality.
    const float gNeg = drive * 0.7f;
    const float n    = (x >= 0.0f) ? std::tanh (drive * x) : std::tanh (gNeg * x);
    const float norm = std::tanh (drive);
    return n / juce::jmax (1.0e-6f, norm);
}

inline float shapeSoftClip (float x, float drive)
{
    // 5th-order odd polynomial with smooth knee (f(1) = 8/15, f'(1) = 0,
    // f''(1) = 0). Derivation: solve y = x + bx^3 + cx^5 with the three
    // constraints; gives b = -2/3, c = 1/5. The earlier b = -1/3 form is
    // a common error: it produces a 0.33 amplitude discontinuity at the
    // knee, which clicks audibly when the input crosses unity.
    const float gx = drive * x;
    if (gx >=  1.0f) return  8.0f / 15.0f;
    if (gx <= -1.0f) return -8.0f / 15.0f;
    const float x3 = gx * gx * gx;
    const float x5 = x3 * gx * gx;
    return gx - (2.0f / 3.0f) * x3 + x5 / 5.0f;
}

// Per-mode max-drive scaling for the "amount" knob.
inline float driveScaleFor (int mode, float amount)
{
    // amount^1.5 gives the Decapitator-style feel: subtle bottom, character mid, hot top.
    const float a = std::pow (juce::jlimit (0.0f, 1.0f, amount), 1.5f);
    switch (mode)
    {
        case 0: return 1.0f + a * 5.0f;   // Tape: 1..6
        case 1: return 1.0f + a * 7.0f;   // Tube: 1..8
        case 2: return 1.0f + a * 4.0f;   // Transformer: 1..5
        case 3: return 1.0f + a * 3.0f;   // Soft-Clip: 1..4
        default: return 1.0f;
    }
}

// Output makeup gain LUT — measured at startup by injecting a -3 dBFS
// 1 kHz sine through each curve at 64 amount values and storing the
// inverse RMS. Constant equal-loudness across the entire amount sweep,
// per mode. The 1 kHz reference matches what nearly every saturation
// plugin uses for makeup calibration.
constexpr int   kMakeupLUTSize = 64;
std::array<float, kMakeupLUTSize> kMakeupLUT[4];
std::once_flag  kMakeupLUTFlag;

inline void buildMakeupLUT()
{
    constexpr float testFreq      = 1000.0f;
    constexpr float testSampleRate = 48000.0f;
    constexpr int   N             = 4800;       // 100 ms = 100 cycles
    constexpr float testAmp       = 0.707f;     // -3 dBFS peak
    constexpr float referenceRms  = testAmp / 1.41421356f;  // = 0.5

    for (int mode = 0; mode < 4; ++mode)
    {
        for (int i = 0; i < kMakeupLUTSize; ++i)
        {
            const float amount = (float) i / (float) (kMakeupLUTSize - 1);
            const float drive  = driveScaleFor (mode, amount);

            float sumSq = 0.0f;
            for (int n = 0; n < N; ++n)
            {
                const float t = (float) n / testSampleRate;
                const float x = testAmp * std::sin (2.0f * juce::MathConstants<float>::pi * testFreq * t);
                float y;
                switch (mode)
                {
                    case 0:  y = shapeTape (x, drive); break;
                    case 1:  y = shapeTube (x, drive); break;
                    case 2:  // Transformer at 1 kHz: HF band saturates mildly (dhi = 1 + (d-1)*0.15)
                    {
                        const float dhi = 1.0f + (drive - 1.0f) * 0.15f;
                        y = std::tanh (dhi * x) / juce::jmax (1.0e-6f, std::tanh (dhi));
                        break;
                    }
                    case 3:  y = shapeSoftClip (x, drive); break;
                    default: y = x; break;
                }
                sumSq += y * y;
            }
            const float rms = std::sqrt (sumSq / (float) N);
            kMakeupLUT[mode][i] = (rms > 1.0e-6f) ? (referenceRms / rms) : 1.0f;
        }
    }
}

inline float driveMakeup (int mode, float amount)
{
    std::call_once (kMakeupLUTFlag, buildMakeupLUT);
    mode = juce::jlimit (0, 3, mode);
    const float idx = juce::jlimit (0.0f, (float) (kMakeupLUTSize - 1),
                                    amount * (float) (kMakeupLUTSize - 1));
    const int   lo  = (int) idx;
    const int   hi  = juce::jmin (lo + 1, kMakeupLUTSize - 1);
    const float fr  = idx - (float) lo;
    return juce::jmap (fr, kMakeupLUT[mode][lo], kMakeupLUT[mode][hi]);
}

} // namespace

//==============================================================================
void AudioMixerProcessor::Channel::prepare (double sr, int blockSize, int numChannels)
{
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, (juce::uint32) numChannels };
    for (auto& f : eqLowFilter)   f.prepare (spec);
    for (auto& f : eqMidFilter)   f.prepare (spec);
    for (auto& f : eqHighFilter)  f.prepare (spec);
    svf.prepare (spec);
    svf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    svf.setCutoffFrequency (live_filterFreq);
    svf.setResonance (juce::jlimit (0.1f, 10.0f, live_filterReso * 10.0f));

    // Transient envelope followers (peak detection, ms time constants).
    juce::dsp::ProcessSpec monoSpec { sr, (juce::uint32) blockSize, 1u };
    for (auto* e : { &envFast, &envSlow, &envLong, &envGainSmooth })
    {
        e->prepare (monoSpec);
        e->setLevelCalculationType (juce::dsp::BallisticsFilterLevelCalculationType::peak);
    }
    envFast.setAttackTime (0.5f);   envFast.setReleaseTime (80.0f);
    envSlow.setAttackTime (30.0f);  envSlow.setReleaseTime (200.0f);
    envLong.setAttackTime (80.0f);  envLong.setReleaseTime (400.0f);
    envGainSmooth.setAttackTime (8.0f); envGainSmooth.setReleaseTime (8.0f);

    // Drive: 2x IIR oversampling for low latency (live use).
    using OS = juce::dsp::Oversampling<float>;
    oversampler = std::make_unique<OS> (
        2,                                        // 2 channels
        1,                                        // 2x oversampling
        OS::filterHalfBandPolyphaseIIR,
        false /*integerLatency*/);
    oversampler->initProcessing ((size_t) blockSize);

    for (auto& f : drivePreShelf)   f.prepare (spec);
    for (auto& f : drivePostShelf)  f.prepare (spec);
    for (auto& f : driveDcBlock)    f.prepare (spec);
    for (auto& f : driveXoverLow)   f.prepare (spec);
    for (auto& f : driveXoverHigh)  f.prepare (spec);
    drive_memory[0] = drive_memory[1] = 0.0f;
    drive_lastModeApplied = -1;

    // Pre-allocated dry-scratch for the Drive wet/dry crossfade. Must be
    // sized to the worst-case host block — allocating in processBlock
    // would be a real-time violation.
    dryScratch.setSize (2, blockSize, false, true, true);

    // Click-free parameter smoothing. 10 ms ramps — short enough that
    // the user can't perceive lag, long enough that hard mute toggles
    // don't pop on percussive material.
    constexpr double kRampSec = 0.010;
    for (auto* s : { &sendSmooth1, &sendSmooth2, &sendSmooth3,
                     &driveAmountSmooth, &gateSmooth })
    {
        s->reset (sr, kRampSec);
    }
    sendSmooth1.setCurrentAndTargetValue (0.0f);
    sendSmooth2.setCurrentAndTargetValue (0.0f);
    sendSmooth3.setCurrentAndTargetValue (0.0f);
    driveAmountSmooth.setCurrentAndTargetValue (0.0f);
    gateSmooth.setCurrentAndTargetValue (1.0f);   // not muted/soloed by default

    // Pre-compute all four modes' coefficient sets once. updateDriveFilters
    // just swaps pointers — no allocation when the user changes mode at
    // runtime.
    cachedPre [0] = IIRCoef::makeHighShelf (sr, 4000.0,  0.7,
                                            juce::Decibels::decibelsToGain ( 4.0f));
    cachedPost[0] = IIRCoef::makeHighShelf (sr, 4000.0,  0.7,
                                            juce::Decibels::decibelsToGain (-4.0f));
    cachedPre [1] = IIRCoef::makePeakFilter (sr, 200.0,  0.7,
                                            juce::Decibels::decibelsToGain ( 1.5f));
    cachedPost[1] = IIRCoef::makePeakFilter (sr, 200.0,  0.7,
                                            juce::Decibels::decibelsToGain (-0.5f));
    cachedPre [2] = IIRCoef::makeAllPass (sr, 1000.0, 1.0);   // unused for Xformer
    cachedPost[2] = IIRCoef::makeAllPass (sr, 1000.0, 1.0);
    cachedPre [3] = IIRCoef::makeAllPass (sr, 1000.0, 1.0);
    cachedPost[3] = IIRCoef::makeAllPass (sr, 1000.0, 1.0);
    cachedDc    = IIRCoef::makeHighPass (sr, 5.0);
    // Xover filters are applied INSIDE the 2x oversampled processing loop,
    // so their coefficients must be designed for the oversampled rate.
    // Otherwise the actual corner frequency drops by 2x (120 Hz becomes
    // 60 Hz at OS rate).
    cachedXLow  = IIRCoef::makeLowPass  (sr * 2.0, 120.0);
    cachedXHigh = IIRCoef::makeHighPass (sr * 2.0, 120.0);

    updateFilters (sr);
    updateDriveFilters (sr);
}

void AudioMixerProcessor::Channel::updateDriveFilters (double /*sr*/)
{
    // No allocations: assign cached coefficient pointers prepared in
    // prepare(). Safe to call from the audio thread on mode change.
    const int m = juce::jlimit (0, 3, live_driveMode);
    for (auto& f : drivePreShelf)  f.coefficients = cachedPre [m];
    for (auto& f : drivePostShelf) f.coefficients = cachedPost[m];
    for (auto& f : driveDcBlock)   f.coefficients = cachedDc;
    for (auto& f : driveXoverLow)  f.coefficients = cachedXLow;
    for (auto& f : driveXoverHigh) f.coefficients = cachedXHigh;
    drive_lastModeApplied = m;
}

void AudioMixerProcessor::Channel::updateFilters (double sr)
{
    constexpr float kLowFreq  = 70.0f;
    constexpr float kMidFreq  = 1000.0f;
    constexpr float kHighFreq = 13000.0f;
    constexpr float kQ        = 0.7f;

    const float gainLow  = kneeKnobToGain (live_eqLow);
    const float gainMid  = kneeKnobToGain (live_eqMid);
    const float gainHigh = kneeKnobToGain (live_eqHigh);

    auto coefLow  = IIRCoef::makeLowShelf  (sr, kLowFreq,  kQ, gainLow);
    auto coefMid  = IIRCoef::makePeakFilter (sr, kMidFreq, kQ, gainMid);
    auto coefHigh = IIRCoef::makeHighShelf (sr, kHighFreq, kQ, gainHigh);

    for (auto& f : eqLowFilter)   f.coefficients = coefLow;
    for (auto& f : eqMidFilter)   f.coefficients = coefMid;
    for (auto& f : eqHighFilter)  f.coefficients = coefHigh;
}

//==============================================================================
void AudioMixerProcessor::Master::prepare (double sr, int blockSize, int numChannels)
{
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, (juce::uint32) numChannels };
    xoverLow.prepare (spec);
    xoverHighL.prepare (spec);
    xoverHigh.prepare (spec);
    xoverLow.setType  (juce::dsp::LinkwitzRileyFilterType::lowpass);
    xoverHighL.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    xoverHigh.setType  (juce::dsp::LinkwitzRileyFilterType::highpass);
    xoverLow.setCutoffFrequency  (300.0f);
    xoverHighL.setCutoffFrequency (300.0f);
    xoverHigh.setCutoffFrequency  (3000.0f);

    constexpr double kRampSec = 0.010;
    gainSmooth.reset (sr, kRampSec);
    boothSmooth.reset (sr, kRampSec);
    muteGainSmooth.reset (sr, kRampSec);
    gainSmooth.setCurrentAndTargetValue (live_gain);
    boothSmooth.setCurrentAndTargetValue (live_booth);
    muteGainSmooth.setCurrentAndTargetValue (live_mute ? 0.0f : 1.0f);
}

//==============================================================================
AudioMixerProcessor::BusesProperties AudioMixerProcessor::makeInitialBuses (int /*numTracks*/)
{
    // All channel + return buses are declared up-front and never change.
    // The "active channel count" is logical only — buses that aren't
    // currently active are still in the layout but receive silence.
    BusesProperties p;
    for (int i = 0; i < kMixerMaxChannels; ++i)
        p = p.withInput ("Channel " + juce::String (i + 1),
                         juce::AudioChannelSet::stereo(), true);
    for (int i = 0; i < kMixerFxReturns; ++i)
        p = p.withInput ("FX Return " + juce::String (i + 1),
                         juce::AudioChannelSet::stereo(), true);
    p = p.withOutput ("Master", juce::AudioChannelSet::stereo(), true)
         .withOutput ("Booth",  juce::AudioChannelSet::stereo(), true)
         .withOutput ("Send 1", juce::AudioChannelSet::stereo(), true)
         .withOutput ("Send 2", juce::AudioChannelSet::stereo(), true)
         .withOutput ("Send 3", juce::AudioChannelSet::stereo(), true);
    return p;
}

AudioMixerProcessor::AudioMixerProcessor (int numTracks, double sampleRate, int blockSize)
    : BaseProcessor (makeInitialBuses (numTracks))
{
    currentSampleRate = sampleRate;
    currentBlockSize  = blockSize;
    setRateAndBufferSizeDetails (sampleRate, blockSize);

    addLegacyParameter (masterVolumeParam = new juce::AudioParameterFloat (
        juce::ParameterID ("masterVolume", 1), "Master Volume",
        -90.0f, 12.0f, 0.0f));
    addLegacyParameter (masterMuteParam = new juce::AudioParameterBool (
        juce::ParameterID ("masterMute", 1), "Master Mute", false));

    // Pre-allocate ALL Channel instances. The active count is logical.
    channels.reserve (kMixerMaxChannels);
    for (int i = 0; i < kMixerMaxChannels; ++i)
    {
        auto ch = std::make_unique<Channel>();
        ch->index  = i;
        ch->busIdx = i;
        ch->name   = "Track " + juce::String (i + 1);
        channels.push_back (std::move (ch));
    }
    activeChannels.store (juce::jlimit (1, kMixerMaxChannels, numTracks));

    for (int i = 0; i < kMixerFxReturns; ++i)
        returns[(size_t) i].busIdx = kMixerMaxChannels + i;

    // Configure the embedded recorder for our 10-stem capture layout:
    //   tracks 1..6 (post-fader)  -> pairs 1..6
    //   FX sends 1..3              -> pairs 7..9
    //   master output              -> pair 10
    juce::StringArray labels;
    for (int i = 0; i < 6; ++i)
        labels.add ("Track" + juce::String (i + 1));
    labels.add ("FX1");
    labels.add ("FX2");
    labels.add ("FX3");
    labels.add ("Master");
    recorder.setStemLabels (labels);
    recorder.setNumActivePairs (10);
    recorder.setCreateSessionFolder (true);
    recorder.setFileMode (AudioRecorderNode::FileMode::OneFilePerStereoPair);
    recorder.setBitDepth (AudioRecorderNode::BitDepth::Float32);
}

AudioMixerProcessor::~AudioMixerProcessor() = default;

void AudioMixerProcessor::fillInPluginDescription (PluginDescription& desc) const
{
    desc.name = getName();
    desc.fileOrIdentifier = "element.audioMixer";
    desc.descriptiveName = "6+ track analog-style mixer";
    desc.category = "Mixer";
    desc.numInputChannels = getTotalNumInputChannels();
    desc.numOutputChannels = getTotalNumOutputChannels();
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = "Element";
    desc.version = "2.0.0";
}

int AudioMixerProcessor::getNumChannels() const noexcept
{
    return activeChannels.load (std::memory_order_relaxed);
}

AudioMixerProcessor::Channel* AudioMixerProcessor::getChannel (int i) const noexcept
{
    if (! juce::isPositiveAndBelow (i, kMixerMaxChannels))
        return nullptr;
    return channels[(size_t) i].get();
}

AudioMixerProcessor::Return* AudioMixerProcessor::getReturn (int i) noexcept
{
    if (! juce::isPositiveAndBelow (i, kMixerFxReturns))
        return nullptr;
    return &returns[(size_t) i];
}

int AudioMixerProcessor::addChannel()
{
    const int cur = activeChannels.load (std::memory_order_relaxed);
    if (cur >= kMixerMaxChannels)
        return -1;
    activeChannels.store (cur + 1, std::memory_order_relaxed);
    return cur;  // index of the newly-activated channel
}

void AudioMixerProcessor::removeLastChannel()
{
    const int cur = activeChannels.load (std::memory_order_relaxed);
    if (cur <= 1)
        return;
    activeChannels.store (cur - 1, std::memory_order_relaxed);
}

//==============================================================================
void AudioMixerProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = blockSize;
    setRateAndBufferSizeDetails (sampleRate, blockSize);

    for (auto& ch : channels)
        ch->prepare (sampleRate, blockSize, 2);
    master.prepare (sampleRate, blockSize, 2);

    sumBuffer.setSize (2, blockSize, false, true, true);
    for (auto& b : sendBuffers)
        b.setSize (2, blockSize, false, true, true);
    channelScratch.setSize (2, blockSize, false, true, true);
    bandBuffer.setSize  (2, blockSize, false, true, true);
    midBuffer.setSize   (2, blockSize, false, true, true);
    highBuffer.setSize  (2, blockSize, false, true, true);

    // 10 stereo stems = 20 channels — pre-allocated for the recorder.
    recordBuffer.setSize (20, blockSize, false, true, true);
    recorder.prepareToRender (sampleRate, blockSize);
}

void AudioMixerProcessor::releaseResources()
{
    sumBuffer.setSize (0, 0);
    for (auto& b : sendBuffers)
        b.setSize (0, 0);
    channelScratch.setSize (0, 0);
    bandBuffer.setSize (0, 0);
    midBuffer.setSize  (0, 0);
    highBuffer.setSize (0, 0);
    recordBuffer.setSize (0, 0);
    recorder.releaseResources();
}

void AudioMixerProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    // Switch the CPU's denormal handling to flush-to-zero for the duration
    // of this block. Without this, the IIR filters in the EQ / SVF / Drive
    // chain can accumulate denormal floats whenever the master fader (or
    // any other gain) goes very low — denormal arithmetic is 50-100x
    // slower on x86 and ARM and routinely produces audible crackling
    // under load. Standard practice for any pro audio processBlock.
    juce::ScopedNoDenormals noDenormals;

    midi.clear();
    const int n = audio.getNumSamples();

    juce::ScopedLock sl (getCallbackLock());

    const int nActive = activeChannels.load (std::memory_order_relaxed);

    // Pull UI targets into live state for each channel and recompute filters.
    bool anyChannelSoloed = false;
    for (int i = 0; i < nActive; ++i)
        if (channels[(size_t) i]->soloTarget.load (std::memory_order_relaxed))
            anyChannelSoloed = true;

    for (int i = 0; i < nActive; ++i)
    {
        auto& ch = channels[(size_t) i];   // unique_ptr<Channel>& — keep '->' style
        const float newGain = ch->gainTarget.load (std::memory_order_relaxed);
        const float newPan  = ch->panTarget.load (std::memory_order_relaxed);

        if (newGain != ch->live_gain) ch->live_gain = newGain;
        if (newPan != ch->live_panL + ch->live_panR /* not equal */) {} // unused
        ch->live_panL = panLawL (newPan);
        ch->live_panR = panLawR (newPan);

        const float el = ch->eqLowTarget.load (std::memory_order_relaxed);
        const float em = ch->eqMidTarget.load (std::memory_order_relaxed);
        const float eh = ch->eqHighTarget.load (std::memory_order_relaxed);
        // Deadband: skip the IIR coefficient recompute (which DOES heap-
        // allocate inside JUCE's make*) when the change is below the
        // ~0.5 dB audibility threshold. Stops continuous knob movement
        // from generating allocations on every block.
        constexpr float kEqDeadband = 0.01f;   // ~0.06 dB at low-knob, irrelevant in cut
        if (std::abs (el - ch->live_eqLow)  > kEqDeadband ||
            std::abs (em - ch->live_eqMid)  > kEqDeadband ||
            std::abs (eh - ch->live_eqHigh) > kEqDeadband)
        {
            ch->live_eqLow  = el;
            ch->live_eqMid  = em;
            ch->live_eqHigh = eh;
            ch->updateFilters (currentSampleRate);
        }

        const float ff = ch->filterFreqTarget.load (std::memory_order_relaxed);
        const float fr = ch->filterResoTarget.load (std::memory_order_relaxed);
        const int   fm = ch->filterModeTarget.load (std::memory_order_relaxed);

        // Always push cutoff/resonance into the SVF — the TPT filter has
        // internal smoothing on these setters, so no zipper noise, and
        // the previous deadband-based update path could leave the filter
        // un-initialised if the UI never moved the knobs.
        ch->live_filterFreq = ff;
        ch->live_filterReso = fr;
        ch->svf.setCutoffFrequency (juce::jlimit (20.0f, 20000.0f, ff));
        const float rNorm = std::pow (juce::jlimit (0.0f, 1.0f, fr), 2.0f);
        ch->svf.setResonance (juce::jlimit (0.1f, 10.0f, 0.5f + rNorm * 9.5f));

        if (fm != ch->live_filterMode)
        {
            ch->live_filterMode = fm;
            using FT = juce::dsp::StateVariableTPTFilterType;
            ch->svf.setType (fm == 0 ? FT::lowpass : (fm == 2 ? FT::highpass : FT::bandpass));
            // Drop the filter's internal state on mode change to avoid the
            // tail of the previous mode bleeding into the new one.
            ch->svf.reset();
        }

        ch->live_mute = ch->muteTarget.load (std::memory_order_relaxed);
        ch->live_solo = ch->soloTarget.load (std::memory_order_relaxed);
        ch->live_cue  = ch->cueTarget.load (std::memory_order_relaxed);

        // Drive amount: smoothed to avoid stepping when the user sweeps the knob.
        ch->driveAmountSmooth.setTargetValue (
            ch->driveAmountTarget.load (std::memory_order_relaxed));

        const int newDriveMode = ch->driveModeTarget.load (std::memory_order_relaxed);
        if (newDriveMode != ch->live_driveMode)
        {
            ch->live_driveMode = newDriveMode;
            ch->updateDriveFilters (currentSampleRate);
            // Reset filter + waveshaper state on mode change so the new
            // mode starts from silence-in-the-pipe rather than the old
            // mode's accumulated state — kills the click that would
            // otherwise hit when the user toggles modes mid-signal.
            for (auto& f : ch->drivePreShelf)  f.reset();
            for (auto& f : ch->drivePostShelf) f.reset();
            for (auto& f : ch->driveDcBlock)   f.reset();
            for (auto& f : ch->driveXoverLow)  f.reset();
            for (auto& f : ch->driveXoverHigh) f.reset();
            ch->drive_memory[0] = ch->drive_memory[1] = 0.0f;
            if (ch->oversampler != nullptr)
                ch->oversampler->reset();
        }

        ch->live_transient = ch->transientTarget.load (std::memory_order_relaxed);

        // Sends: smoothed targets, applied as ramps below.
        ch->sendSmooth1.setTargetValue (ch->send1Target.load (std::memory_order_relaxed));
        ch->sendSmooth2.setTargetValue (ch->send2Target.load (std::memory_order_relaxed));
        ch->sendSmooth3.setTargetValue (ch->send3Target.load (std::memory_order_relaxed));

        // Mute/solo/cue collapse into a single gate target. The 10 ms ramp
        // on this value gives us click-free fade in/out without dedicated
        // fade machinery.
        const bool gated = ch->live_mute
                        || (anyChannelSoloed && ! ch->live_solo);
        ch->gateSmooth.setTargetValue (gated ? 0.0f : 1.0f);
    }

    // Pull master state. Gain/booth/mute go through LinearSmoothedValue
    // ramps; isolator and the others are read directly (they only affect
    // filter coefficients applied per-sample, not per-block scalars).
    master.gainSmooth.setTargetValue  (master.gainTarget.load (std::memory_order_relaxed));
    master.boothSmooth.setTargetValue (master.boothTarget.load (std::memory_order_relaxed));
    master.live_mute = master.muteTarget.load (std::memory_order_relaxed);
    master.muteGainSmooth.setTargetValue (master.live_mute ? 0.0f : 1.0f);
    master.live_isoLow  = master.isoLowTarget.load (std::memory_order_relaxed);
    master.live_isoMid  = master.isoMidTarget.load (std::memory_order_relaxed);
    master.live_isoHigh = master.isoHighTarget.load (std::memory_order_relaxed);

    // Pull return state.
    for (auto& r : returns)
    {
        r.live_level    = r.levelTarget.load (std::memory_order_relaxed);
        r.live_toMaster = r.toMasterTarget.load (std::memory_order_relaxed);
        r.live_mute     = r.muteTarget.load (std::memory_order_relaxed);
    }

    // Clear sum + send buffers + record buffer.
    sumBuffer.clear (0, n);
    for (auto& b : sendBuffers)
        b.clear (0, n);
    recordBuffer.clear();

    //--- Per channel ------------------------------------------------------
    for (int chIdx = 0; chIdx < nActive; ++chIdx)
    {
        auto& ch = *channels[(size_t) chIdx];

        if (ch.busIdx < 0)
        {
            ch.rmsL.store (0.0f, std::memory_order_relaxed);
            ch.rmsR.store (0.0f, std::memory_order_relaxed);
            continue;
        }

        // Sample the smoothed gate at the start and end of this block.
        // We'll apply the ramp inside addFromWithRamp by folding the gate
        // into the per-channel gain. When both ends are silent the channel
        // contributes nothing — but we still must advance smoothed values
        // and reset RMS, so don't early-return.
        const float gateStart = ch.gateSmooth.getCurrentValue();
        ch.gateSmooth.skip (n);
        const float gateEnd   = ch.gateSmooth.getCurrentValue();
        const bool  silent    = (gateStart < 1.0e-5f && gateEnd < 1.0e-5f);

        if (silent)
        {
            // Still advance other smoothed values so they don't snap
            // when the gate reopens.
            ch.sendSmooth1.skip (n);
            ch.sendSmooth2.skip (n);
            ch.sendSmooth3.skip (n);
            ch.driveAmountSmooth.skip (n);
            ch.rmsL.store (0.0f, std::memory_order_relaxed);
            ch.rmsR.store (0.0f, std::memory_order_relaxed);
            continue;
        }

        // Drive amount: pull smoothed end-of-block into live state for the
        // shaper math below. Within a block, the value is "constant enough"
        // since the 10 ms ramp limits per-block change to a few % anyway.
        ch.driveAmountSmooth.skip (n);
        ch.live_driveAmount = ch.driveAmountSmooth.getCurrentValue();

        auto input = getBusBuffer<float> (audio, true, ch.busIdx);
        if (input.getNumChannels() < 1)
            continue;

        // Copy to scratch (2 channels — mono inputs get duplicated).
        channelScratch.clear (0, n);
        const int nIn = juce::jmin (2, input.getNumChannels());
        for (int c = 0; c < 2; ++c)
        {
            const int src = juce::jmin (c, nIn - 1);
            channelScratch.copyFrom (c, 0, input, src, 0, n);
        }

        // -------- Transient shaper --------
        // Stereo-summed sidechain so L/R get matched gain modulation.
        if (std::abs (ch.live_transient) > 1.0e-3f)
        {
            auto* L = channelScratch.getWritePointer (0);
            auto* R = channelScratch.getWritePointer (1);
            const float k = ch.live_transient;
            const float k3 = k * k * k;
            const float curveDb = (k >= 0.0f ? 10.0f : 15.0f) * k3;
            for (int i = 0; i < n; ++i)
            {
                const float monoAbs = 0.5f * (std::abs (L[i]) + std::abs (R[i]));
                const float es = ch.envSlow.processSample (0, monoAbs);
                const float el = ch.envLong.processSample (0, monoAbs);

                const float esDb = juce::Decibels::gainToDecibels (es, -120.0f);
                const float elDb = juce::Decibels::gainToDecibels (el, -120.0f);
                const float dSustain = juce::jlimit (0.0f, 12.0f, esDb - elDb);

                float gainDb = curveDb * dSustain * (1.0f / 12.0f);
                if (esDb < -55.0f) gainDb = 0.0f;
                const float smoothed = ch.envGainSmooth.processSample (0, gainDb);
                const float gain = juce::Decibels::decibelsToGain (smoothed);
                L[i] *= gain;
                R[i] *= gain;
            }
        }

        // -------- EQ: low -> mid -> high --------
        for (int c = 0; c < 2; ++c)
        {
            auto* d = channelScratch.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                x = ch.eqLowFilter[(size_t) c].processSample (x);
                x = ch.eqMidFilter[(size_t) c].processSample (x);
                x = ch.eqHighFilter[(size_t) c].processSample (x);
                d[i] = x;
            }
        }

        // -------- State-variable filter (bypass in mode 1) --------
        if (ch.live_filterMode != 1)
        {
            juce::dsp::AudioBlock<float> blk (channelScratch.getArrayOfWritePointers(),
                                              2, 0, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            ch.svf.process (ctx);
        }

        // -------- Drive --------
        if (ch.live_driveAmount > 1.0e-3f && ch.oversampler != nullptr)
        {
            const float drive   = driveScaleFor (ch.live_driveMode, ch.live_driveAmount);
            const float makeup  = driveMakeup   (ch.live_driveMode, ch.live_driveAmount);
            // Wet/dry smoothstep at low end of the knob — guarantees true bypass
            // around 0 even when the curve isn't perfectly unity at amount=0.
            const float t = juce::jlimit (0.0f, 1.0f,
                                          (ch.live_driveAmount - 0.02f) / 0.06f);
            const float wet = t * t * (3.0f - 2.0f * t);  // smoothstep
            const float dry = 1.0f - wet;
            const int mode  = ch.live_driveMode;

            // Save dry copy for wet/dry blend (pre-allocated scratch — no
            // heap allocation in the real-time path).
            for (int c = 0; c < 2; ++c)
                ch.dryScratch.copyFrom (c, 0, channelScratch, c, 0, n);

            // Pre-EQ (per mode).
            if (mode == 0 || mode == 1)
            {
                for (int c = 0; c < 2; ++c)
                {
                    auto* d = channelScratch.getWritePointer (c);
                    for (int i = 0; i < n; ++i)
                        d[i] = ch.drivePreShelf[(size_t) c].processSample (d[i]);
                }
            }

            // Oversample, shape, oversample-down.
            juce::dsp::AudioBlock<float> blk (channelScratch.getArrayOfWritePointers(),
                                              2, 0, (size_t) n);
            auto upBlock = ch.oversampler->processSamplesUp (blk);
            const size_t nUp = upBlock.getNumSamples();

            for (int c = 0; c < 2; ++c)
            {
                auto* up = upBlock.getChannelPointer ((size_t) c);
                for (size_t i = 0; i < nUp; ++i)
                {
                    const float xDry = up[i];
                    float y;
                    switch (mode)
                    {
                        case 0:  y = shapeTape (xDry, drive); break;
                        case 1:  y = shapeTube (xDry, drive); break;
                        case 2:
                        {
                            // Transformer: split LF/HF, saturate LF hard, HF mild,
                            // then add a slow memory term for inductor "weight".
                            float low  = ch.driveXoverLow [(size_t) c].processSample (xDry);
                            float high = ch.driveXoverHigh[(size_t) c].processSample (xDry);
                            const float dlo = drive;
                            const float dhi = 1.0f + (drive - 1.0f) * 0.15f;
                            low  = std::tanh (dlo * low) / juce::jmax (1.0e-6f, std::tanh (dlo));
                            high = std::tanh (dhi * high) / juce::jmax (1.0e-6f, std::tanh (dhi));
                            y = low + high;
                            ch.drive_memory[c] += (y - ch.drive_memory[c]) * 0.02f;
                            y += 0.15f * ch.drive_memory[c];
                            break;
                        }
                        case 3:  y = shapeSoftClip (xDry, drive); break;
                        default: y = xDry; break;
                    }
                    up[i] = y * makeup;
                }
            }
            ch.oversampler->processSamplesDown (blk);

            // Post-EQ + DC block.
            for (int c = 0; c < 2; ++c)
            {
                auto* d = channelScratch.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    float y = d[i];
                    if (mode == 0)
                        y = ch.drivePostShelf[(size_t) c].processSample (y);
                    else if (mode == 1)
                    {
                        y = ch.drivePostShelf[(size_t) c].processSample (y);
                        y = ch.driveDcBlock [(size_t) c].processSample (y);
                    }
                    else if (mode == 2)
                        y = ch.driveDcBlock [(size_t) c].processSample (y);
                    d[i] = y;
                }
            }

            // Wet/dry blend with the saved dry copy.
            if (wet < 0.999f)
            {
                for (int c = 0; c < 2; ++c)
                {
                    auto* wetPtr = channelScratch.getWritePointer (c);
                    auto* dryPtr = ch.dryScratch.getReadPointer (c);
                    for (int i = 0; i < n; ++i)
                        wetPtr[i] = dry * dryPtr[i] + wet * wetPtr[i];
                }
            }
        }

        // Pan + gain into the master sum. Gate ramp (gateStart..gateEnd)
        // is folded into the start/end of the per-channel ramp so a mute
        // toggle becomes a click-free 10 ms fade across the block.
        const float lastL = ch.live_lastGain * ch.live_panL * gateStart;
        const float lastR = ch.live_lastGain * ch.live_panR * gateStart;
        const float newL  = ch.live_gain     * ch.live_panL * gateEnd;
        const float newR  = ch.live_gain     * ch.live_panR * gateEnd;

        sumBuffer.addFromWithRamp (0, 0, channelScratch.getReadPointer (0), n, lastL, newL);
        sumBuffer.addFromWithRamp (1, 0, channelScratch.getReadPointer (1), n, lastR, newR);

        // Capture this channel's post-fader stem for the embedded recorder.
        // Only the first 6 tracks are captured (matches the default 6-channel
        // layout — extra channels are merged into the master only).
        if (chIdx < 6)
        {
            const int stemBase = chIdx * 2;
            recordBuffer.addFromWithRamp (stemBase,     0, channelScratch.getReadPointer (0), n, lastL, newL);
            recordBuffer.addFromWithRamp (stemBase + 1, 0, channelScratch.getReadPointer (1), n, lastR, newR);
        }

        // Sends (post-fader, post-EQ, post-filter, post-gate). Each send
        // is ramped via addFromWithRamp using the smoothed send value at
        // the start and end of the block, so twisting a send fader is
        // click-free.
        auto rampSend = [&] (juce::AudioBuffer<float>& dst,
                             juce::LinearSmoothedValue<float>& s)
        {
            const float a = s.getCurrentValue();
            s.skip (n);
            const float b = s.getCurrentValue();
            if (a < 1.0e-5f && b < 1.0e-5f)
                return;
            const float aL = a * gateStart * ch.live_lastGain * ch.live_panL;
            const float aR = a * gateStart * ch.live_lastGain * ch.live_panR;
            const float bL = b * gateEnd   * ch.live_gain     * ch.live_panL;
            const float bR = b * gateEnd   * ch.live_gain     * ch.live_panR;
            dst.addFromWithRamp (0, 0, channelScratch.getReadPointer (0), n, aL, bL);
            dst.addFromWithRamp (1, 0, channelScratch.getReadPointer (1), n, aR, bR);
        };
        rampSend (sendBuffers[0], ch.sendSmooth1);
        rampSend (sendBuffers[1], ch.sendSmooth2);
        rampSend (sendBuffers[2], ch.sendSmooth3);

        ch.live_lastGain = ch.live_gain;

        // Meters (post-fader RMS — reflect what reaches the master).
        ch.rmsL.store (channelScratch.getRMSLevel (0, 0, n) * std::abs (newL),
                       std::memory_order_relaxed);
        ch.rmsR.store (channelScratch.getRMSLevel (1, 0, n) * std::abs (newR),
                       std::memory_order_relaxed);
    }

    //--- FX returns -------------------------------------------------------
    for (auto& r : returns)
    {
        if (r.busIdx < 0 || r.live_mute || ! r.live_toMaster)
        {
            r.rmsL.store (0.0f, std::memory_order_relaxed);
            r.rmsR.store (0.0f, std::memory_order_relaxed);
            continue;
        }
        auto rin = getBusBuffer<float> (audio, true, r.busIdx);
        if (rin.getNumChannels() < 1)
            continue;
        const int nIn = juce::jmin (2, rin.getNumChannels());
        for (int c = 0; c < 2; ++c)
        {
            const int src = juce::jmin (c, nIn - 1);
            sumBuffer.addFromWithRamp (c, 0, rin.getReadPointer (src), n,
                                       r.live_lastLevel, r.live_level);
        }
        r.rmsL.store (rin.getRMSLevel (0, 0, n) * r.live_level, std::memory_order_relaxed);
        r.rmsR.store (rin.getRMSLevel (juce::jmin (1, nIn - 1), 0, n) * r.live_level,
                      std::memory_order_relaxed);
        r.live_lastLevel = r.live_level;
    }

    //--- Master 3-band isolator -------------------------------------------
    // Split sum into low (≤300), mid (300..3000), high (≥3000) using LR4.
    // Then weigh each band by its iso knob's kneeKnobToGain and recombine.
    {
        // Low band: lowpass at 300
        bandBuffer.makeCopyOf (sumBuffer, true);
        {
            juce::dsp::AudioBlock<float> blk (bandBuffer);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            master.xoverLow.process (ctx);
        }
        // 3-band split using LR4: low = LP@300, high = HP@3000, mid = the
        // rest. LR4 LP+HP at a given corner sums to allpass(signal), so
        // mid = sum − low − high recovers a clean middle band.
        // Buffers are pre-allocated members — no heap alloc here.
        for (int c = 0; c < 2; ++c)
            highBuffer.copyFrom (c, 0, sumBuffer, c, 0, n);
        {
            juce::dsp::AudioBlock<float> blk (highBuffer);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            master.xoverHigh.process (ctx);  // HP @ 3000
        }

        const float gLow  = kneeKnobToGain (master.live_isoLow);
        const float gHigh = kneeKnobToGain (master.live_isoHigh);
        const float gMid  = kneeKnobToGain (master.live_isoMid);

        // mid = sum - low - high
        for (int c = 0; c < 2; ++c)
        {
            auto* m = midBuffer.getWritePointer (c);
            auto* s = sumBuffer.getReadPointer (c);
            auto* l = bandBuffer.getReadPointer (c);
            auto* h = highBuffer.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                m[i] = s[i] - l[i] - h[i];
        }

        sumBuffer.clear (0, n);
        for (int c = 0; c < 2; ++c)
        {
            sumBuffer.addFrom (c, 0, bandBuffer, c, 0, n, gLow);
            sumBuffer.addFrom (c, 0, midBuffer,  c, 0, n, gMid);
            sumBuffer.addFrom (c, 0, highBuffer, c, 0, n, gHigh);
        }
    }

    //--- Master output ----------------------------------------------------
    auto masterOut = getBusBuffer<float> (audio, false, kMixerOutMaster);
    auto boothOut  = getBusBuffer<float> (audio, false, kMixerOutBooth);
    masterOut.clear (0, n);
    boothOut.clear (0, n);

    // Sample master gain at start/end of block from the smoother. The mute
    // gain is multiplied in so that mute toggle is a 10 ms fade rather
    // than an instant cut.
    const float gA = master.gainSmooth.getCurrentValue();
    master.gainSmooth.skip (n);
    const float gB = master.gainSmooth.getCurrentValue();

    const float mA = master.muteGainSmooth.getCurrentValue();
    master.muteGainSmooth.skip (n);
    const float mB = master.muteGainSmooth.getCurrentValue();

    const float masterStart = gA * mA;
    const float masterEnd   = gB * mB;
    for (int c = 0; c < masterOut.getNumChannels() && c < 2; ++c)
        masterOut.copyFromWithRamp (c, 0, sumBuffer.getReadPointer (c), n,
                                    masterStart, masterEnd);

    // Booth has its own gain, no mute (engineers want monitor up while
    // the front-of-house is muted — that's the whole point of a booth).
    const float bA = master.boothSmooth.getCurrentValue();
    master.boothSmooth.skip (n);
    const float bB = master.boothSmooth.getCurrentValue();
    for (int c = 0; c < boothOut.getNumChannels() && c < 2; ++c)
        boothOut.copyFromWithRamp (c, 0, sumBuffer.getReadPointer (c), n,
                                   bA, bB);

    // Keep the legacy live_lastGain/Booth fields in sync so anything else
    // reading them sees current values.
    master.live_gain = gB;
    master.live_booth = bB;
    master.live_lastGain = gB;
    master.live_lastBooth = bB;

    // Send outputs.
    for (int s = 0; s < kMixerFxSends; ++s)
    {
        auto sendOut = getBusBuffer<float> (audio, false, kMixerOutSendFirst + s);
        sendOut.clear (0, n);
        for (int c = 0; c < sendOut.getNumChannels() && c < 2; ++c)
            sendOut.copyFrom (c, 0, sendBuffers[(size_t) s], c, 0, n);

        // Capture FX send as a stem for the recorder (pairs 7..9 = chans 12..17).
        const int sendStemBase = 12 + s * 2;
        recordBuffer.copyFrom (sendStemBase,     0, sendBuffers[(size_t) s], 0, 0, n);
        recordBuffer.copyFrom (sendStemBase + 1, 0, sendBuffers[(size_t) s], 1, 0, n);
    }

    // Master stem (pair 10 = chans 18, 19) — post-isolator, post-master-gain.
    for (int c = 0; c < 2 && c < masterOut.getNumChannels(); ++c)
        recordBuffer.copyFrom (18 + c, 0, masterOut.getReadPointer (c), n);

    // Push the assembled 20-channel buffer to the disk writer thread.
    recorder.writeAudioBlock (recordBuffer);

    // Master meters (RMS + peak).
    if (masterOut.getNumChannels() >= 1)
    {
        master.rmsL.store (masterOut.getRMSLevel (0, 0, n), std::memory_order_relaxed);
        master.peakL.store (masterOut.getMagnitude (0, 0, n), std::memory_order_relaxed);
    }
    if (masterOut.getNumChannels() >= 2)
    {
        master.rmsR.store (masterOut.getRMSLevel (1, 0, n), std::memory_order_relaxed);
        master.peakR.store (masterOut.getMagnitude (1, 0, n), std::memory_order_relaxed);
    }

    // Sync master params to atomics (for state persistence + automation).
    if (masterVolumeParam != nullptr)
    {
        const float dB = juce::Decibels::gainToDecibels (master.live_gain, -90.0f);
        if (std::abs (dB - masterVolumeParam->get()) > 0.05f)
            *masterVolumeParam = dB;
    }
    if (masterMuteParam != nullptr && *masterMuteParam != master.live_mute)
        *masterMuteParam = master.live_mute;
}

//==============================================================================
bool AudioMixerProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    // All buses are stereo or mono.
    for (auto& b : layout.inputBuses)
        if (b != juce::AudioChannelSet::stereo() && b != juce::AudioChannelSet::mono())
            return false;
    for (auto& b : layout.outputBuses)
        if (b != juce::AudioChannelSet::stereo() && b != juce::AudioChannelSet::mono())
            return false;
    return true;
}

bool AudioMixerProcessor::canApplyBusCountChange (bool /*isInput*/, bool /*isAdding*/,
                                                  AudioProcessor::BusProperties& /*outProperties*/)
{
    // Bus layout is fixed at construction (16 channel + 3 return inputs,
    // 5 outputs). Active channel count is a logical UI concept only.
    return false;
}

//==============================================================================
void AudioMixerProcessor::getStateInformation (juce::MemoryBlock& block)
{
    juce::ValueTree state ("audiomixer");
    state.setProperty ("version", 2, nullptr);
    state.setProperty ("numChannels", activeChannels.load(), nullptr);
    state.setProperty ("masterGain", master.live_gain, nullptr);
    state.setProperty ("masterBooth", master.live_booth, nullptr);
    state.setProperty ("masterMute", master.live_mute, nullptr);
    state.setProperty ("isoLow", master.live_isoLow, nullptr);
    state.setProperty ("isoMid", master.live_isoMid, nullptr);
    state.setProperty ("isoHigh", master.live_isoHigh, nullptr);

    const int nActiveSave = activeChannels.load();
    for (int i = 0; i < nActiveSave; ++i)
    {
        auto& ch = *channels[(size_t) i];
        juce::ValueTree t ("channel");
        t.setProperty ("index", ch.index, nullptr)
         .setProperty ("name", ch.name, nullptr)
         .setProperty ("gain", ch.live_gain, nullptr)
         .setProperty ("pan",  ch.panTarget.load(), nullptr)
         .setProperty ("mute", ch.live_mute, nullptr)
         .setProperty ("solo", ch.live_solo, nullptr)
         .setProperty ("cue",  ch.live_cue, nullptr)
         .setProperty ("eqLow", ch.live_eqLow, nullptr)
         .setProperty ("eqMid", ch.live_eqMid, nullptr)
         .setProperty ("eqHigh", ch.live_eqHigh, nullptr)
         .setProperty ("filterFreq", ch.live_filterFreq, nullptr)
         .setProperty ("filterReso", ch.live_filterReso, nullptr)
         .setProperty ("filterMode", ch.live_filterMode, nullptr)
         .setProperty ("send1", ch.send1Target.load(), nullptr)
         .setProperty ("send2", ch.send2Target.load(), nullptr)
         .setProperty ("send3", ch.send3Target.load(), nullptr)
         .setProperty ("driveAmount", ch.driveAmountTarget.load(), nullptr)
         .setProperty ("driveMode",   ch.driveModeTarget.load(),   nullptr)
         .setProperty ("transient",   ch.transientTarget.load(),   nullptr);
        state.addChild (t, -1, nullptr);
    }
    for (int i = 0; i < kMixerFxReturns; ++i)
    {
        juce::ValueTree t ("return");
        t.setProperty ("index", i, nullptr)
         .setProperty ("level", returns[(size_t) i].live_level, nullptr)
         .setProperty ("mute",  returns[(size_t) i].live_mute, nullptr)
         .setProperty ("toMaster", returns[(size_t) i].live_toMaster, nullptr);
        state.addChild (t, -1, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, block);
}

void AudioMixerProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr)
        return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return;

    const int wantChannels = juce::jlimit (1, kMixerMaxChannels,
                                           (int) state.getProperty ("numChannels", activeChannels.load()));
    activeChannels.store (wantChannels);

    master.gainTarget.store  ((float) state.getProperty ("masterGain", 1.0));
    master.boothTarget.store ((float) state.getProperty ("masterBooth", 1.0));
    master.muteTarget.store  ((bool)  state.getProperty ("masterMute", false));
    master.isoLowTarget.store ((float) state.getProperty ("isoLow", 0.0));
    master.isoMidTarget.store ((float) state.getProperty ("isoMid", 0.0));
    master.isoHighTarget.store ((float) state.getProperty ("isoHigh", 0.0));

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto t = state.getChild (i);
        if (t.hasType ("channel"))
        {
            const int idx = t.getProperty ("index", -1);
            if (auto* ch = getChannel (idx))
            {
                ch->name = t.getProperty ("name", ch->name).toString();
                ch->gainTarget.store ((float) t.getProperty ("gain", 1.0));
                ch->panTarget.store  ((float) t.getProperty ("pan", 0.0));
                ch->muteTarget.store ((bool)  t.getProperty ("mute", false));
                ch->soloTarget.store ((bool)  t.getProperty ("solo", false));
                ch->cueTarget.store  ((bool)  t.getProperty ("cue", false));
                ch->eqLowTarget.store ((float) t.getProperty ("eqLow", 0.0));
                ch->eqMidTarget.store ((float) t.getProperty ("eqMid", 0.0));
                ch->eqHighTarget.store ((float) t.getProperty ("eqHigh", 0.0));
                ch->filterFreqTarget.store ((float) t.getProperty ("filterFreq", 1000.0));
                ch->filterResoTarget.store ((float) t.getProperty ("filterReso", 0.5));
                ch->filterModeTarget.store ((int) t.getProperty ("filterMode", 1));
                ch->send1Target.store ((float) t.getProperty ("send1", 0.0));
                ch->send2Target.store ((float) t.getProperty ("send2", 0.0));
                ch->send3Target.store ((float) t.getProperty ("send3", 0.0));
                ch->driveAmountTarget.store ((float) t.getProperty ("driveAmount", 0.0));
                ch->driveModeTarget.store ((int) t.getProperty ("driveMode", 0));
                ch->transientTarget.store ((float) t.getProperty ("transient", 0.0));
            }
        }
        else if (t.hasType ("return"))
        {
            const int idx = t.getProperty ("index", -1);
            if (auto* r = getReturn (idx))
            {
                r->levelTarget.store ((float) t.getProperty ("level", 1.0));
                r->muteTarget.store ((bool) t.getProperty ("mute", false));
                r->toMasterTarget.store ((bool) t.getProperty ("toMaster", true));
            }
        }
    }
}

//==============================================================================
AudioProcessorEditor* AudioMixerProcessor::createEditor()
{
    return new AudioMixerEditor (*this);
}

} // namespace element
