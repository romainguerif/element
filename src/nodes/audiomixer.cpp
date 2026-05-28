// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audiomixer.hpp"
#include "nodes/audiomixereditor.hpp"

#include <element/ui/style.hpp>

#include <cmath>

namespace element {

namespace {

inline float kneeKnobToGain (float k)
{
    // Maps the -1..+1 EQ knob to a linear gain.
    //   +1  -> +6 dB
    //    0  -> unity (0 dB)
    //   -1  -> kill (-60 dB ≈ 0.001)
    if (k >= 0.0f)
        return juce::Decibels::decibelsToGain (k * 6.0f);
    return juce::Decibels::decibelsToGain (k * 60.0f, -60.0f);
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
    updateFilters (sr);
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
}

//==============================================================================
namespace {
// Build the initial bus layout with proper names for every bus, so the
// host's wiring UI displays "Channel 1..N" and "FX Return 1..3" rather
// than the JUCE default "Input #N".
BusesProperties makeInitialBuses (int numTracks)
{
    const int n = juce::jlimit (1, kMixerMaxChannels, numTracks);
    BusesProperties p;
    for (int i = 0; i < n; ++i)
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
} // namespace

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

    // Buses already exist (declared in makeInitialBuses). Just create the
    // Channel/Return wrappers tied to the matching bus indices.
    channels.reserve (kMixerMaxChannels);
    const int n = juce::jlimit (1, kMixerMaxChannels, numTracks);
    for (int i = 0; i < n; ++i)
        addChannelInternal (false);

    for (int i = 0; i < kMixerFxReturns; ++i)
        returns[(size_t) i].busIdx = n + i;
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
    return (int) channels.size();
}

AudioMixerProcessor::Channel* AudioMixerProcessor::getChannel (int i) const noexcept
{
    if (! juce::isPositiveAndBelow (i, (int) channels.size()))
        return nullptr;
    return channels[(size_t) i].get();
}

AudioMixerProcessor::Return* AudioMixerProcessor::getReturn (int i) noexcept
{
    if (! juce::isPositiveAndBelow (i, kMixerFxReturns))
        return nullptr;
    return &returns[(size_t) i];
}

void AudioMixerProcessor::addChannelInternal (bool registerBus)
{
    if ((int) channels.size() >= kMixerMaxChannels)
        return;

    auto ch = std::make_unique<Channel>();
    ch->index = (int) channels.size();
    ch->name  = "Track " + juce::String (ch->index + 1);

    if (registerBus)
    {
        pendingBusName = "Channel " + juce::String (ch->index + 1);
        addBus (true);
        pendingBusName.clear();
        if (auto* bus = getBus (true, getBusCount (true) - 1))
            ch->busIdx = bus->getBusIndex();
    }
    else
    {
        // Bus already exists (declared at construction). Match by index.
        ch->busIdx = ch->index;
    }

    channels.push_back (std::move (ch));
}

int AudioMixerProcessor::addChannel()
{
    juce::ScopedLock sl (getCallbackLock());
    if ((int) channels.size() >= kMixerMaxChannels)
        return -1;

    // Returns are at the tail of the input bus list. To keep them at
    // the tail with stable indices after we add a channel, we strip
    // them off, add the channel bus, then re-add them by name.
    for (int i = 0; i < kMixerFxReturns; ++i)
        removeBus (true);
    addChannelInternal (true);
    for (int i = 0; i < kMixerFxReturns; ++i)
    {
        pendingBusName = "FX Return " + juce::String (i + 1);
        addBus (true);
        pendingBusName.clear();
        if (auto* bus = getBus (true, getBusCount (true) - 1))
            returns[(size_t) i].busIdx = bus->getBusIndex();
    }

    auto& ch = *channels.back();
    ch.prepare (currentSampleRate, currentBlockSize, 2);
    return ch.index;
}

void AudioMixerProcessor::removeLastChannel()
{
    juce::ScopedLock sl (getCallbackLock());
    if (channels.size() <= 1)
        return;

    for (int i = 0; i < kMixerFxReturns; ++i)
        removeBus (true);
    removeBus (true);                      // the channel's bus
    channels.pop_back();
    for (int i = 0; i < kMixerFxReturns; ++i)
    {
        pendingBusName = "FX Return " + juce::String (i + 1);
        addBus (true);
        pendingBusName.clear();
        if (auto* bus = getBus (true, getBusCount (true) - 1))
            returns[(size_t) i].busIdx = bus->getBusIndex();
    }
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
    bandBuffer.setSize (2, blockSize, false, true, true);
}

void AudioMixerProcessor::releaseResources()
{
    sumBuffer.setSize (0, 0);
    for (auto& b : sendBuffers)
        b.setSize (0, 0);
    channelScratch.setSize (0, 0);
    bandBuffer.setSize (0, 0);
}

void AudioMixerProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    midi.clear();
    const int n = audio.getNumSamples();

    juce::ScopedLock sl (getCallbackLock());

    // Pull UI targets into live state for each channel and recompute filters.
    bool anyFiltersDirty = false;
    bool anyChannelSoloed = false;
    for (auto& ch : channels)
        if (ch->soloTarget.load (std::memory_order_relaxed))
            anyChannelSoloed = true;

    for (auto& ch : channels)
    {
        const float newGain = ch->gainTarget.load (std::memory_order_relaxed);
        const float newPan  = ch->panTarget.load (std::memory_order_relaxed);

        if (newGain != ch->live_gain) ch->live_gain = newGain;
        if (newPan != ch->live_panL + ch->live_panR /* not equal */) {} // unused
        ch->live_panL = panLawL (newPan);
        ch->live_panR = panLawR (newPan);

        const float el = ch->eqLowTarget.load (std::memory_order_relaxed);
        const float em = ch->eqMidTarget.load (std::memory_order_relaxed);
        const float eh = ch->eqHighTarget.load (std::memory_order_relaxed);
        if (el != ch->live_eqLow || em != ch->live_eqMid || eh != ch->live_eqHigh)
        {
            ch->live_eqLow  = el;
            ch->live_eqMid  = em;
            ch->live_eqHigh = eh;
            ch->updateFilters (currentSampleRate);
        }

        const float ff = ch->filterFreqTarget.load (std::memory_order_relaxed);
        const float fr = ch->filterResoTarget.load (std::memory_order_relaxed);
        const int   fm = ch->filterModeTarget.load (std::memory_order_relaxed);
        if (std::abs (ff - ch->live_filterFreq) > 0.5f)
        {
            ch->live_filterFreq = ff;
            ch->svf.setCutoffFrequency (juce::jlimit (20.0f, 20000.0f, ff));
        }
        if (std::abs (fr - ch->live_filterReso) > 1.0e-4f)
        {
            ch->live_filterReso = fr;
            ch->svf.setResonance (juce::jlimit (0.1f, 10.0f, 0.5f + fr * 9.5f));
        }
        if (fm != ch->live_filterMode)
        {
            ch->live_filterMode = fm;
            using FT = juce::dsp::StateVariableTPTFilterType;
            ch->svf.setType (fm == 0 ? FT::lowpass : (fm == 2 ? FT::highpass : FT::bandpass));
        }

        ch->live_mute = ch->muteTarget.load (std::memory_order_relaxed);
        ch->live_solo = ch->soloTarget.load (std::memory_order_relaxed);
        ch->live_cue  = ch->cueTarget.load (std::memory_order_relaxed);
    }

    // Pull master state.
    master.live_gain  = master.gainTarget.load (std::memory_order_relaxed);
    master.live_booth = master.boothTarget.load (std::memory_order_relaxed);
    master.live_mute  = master.muteTarget.load (std::memory_order_relaxed);
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

    // Clear sum + send buffers.
    sumBuffer.clear (0, n);
    for (auto& b : sendBuffers)
        b.clear (0, n);

    //--- Per channel ------------------------------------------------------
    for (auto& chPtr : channels)
    {
        auto& ch = *chPtr;
        const bool gated = ch.live_mute || (anyChannelSoloed && ! ch.live_solo);

        if (gated || ch.busIdx < 0)
        {
            ch.rmsL.store (0.0f, std::memory_order_relaxed);
            ch.rmsR.store (0.0f, std::memory_order_relaxed);
            continue;
        }

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

        // EQ: low -> mid -> high (stereo, sample-by-sample via IIR::Filter)
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

        // State-variable filter (bypass in mode 1).
        if (ch.live_filterMode != 1)
        {
            juce::dsp::AudioBlock<float> blk (channelScratch.getArrayOfWritePointers(),
                                              2, 0, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            ch.svf.process (ctx);
        }

        // Pan + gain into the channel's contribution.
        // Apply linear ramp from lastGain to gain.
        const float lastL = ch.live_lastGain * ch.live_panL;
        const float lastR = ch.live_lastGain * ch.live_panR;
        const float newL  = ch.live_gain * ch.live_panL;
        const float newR  = ch.live_gain * ch.live_panR;

        sumBuffer.addFromWithRamp (0, 0, channelScratch.getReadPointer (0), n, lastL, newL);
        sumBuffer.addFromWithRamp (1, 0, channelScratch.getReadPointer (1), n, lastR, newR);

        // Sends (post-fader, post-EQ, post-filter — the standard).
        const float s1 = ch.send1Target.load (std::memory_order_relaxed) * ch.live_gain;
        const float s2 = ch.send2Target.load (std::memory_order_relaxed) * ch.live_gain;
        const float s3 = ch.send3Target.load (std::memory_order_relaxed) * ch.live_gain;
        if (s1 > 1.0e-5f)
        {
            sendBuffers[0].addFrom (0, 0, channelScratch, 0, 0, n, s1 * ch.live_panL);
            sendBuffers[0].addFrom (1, 0, channelScratch, 1, 0, n, s1 * ch.live_panR);
        }
        if (s2 > 1.0e-5f)
        {
            sendBuffers[1].addFrom (0, 0, channelScratch, 0, 0, n, s2 * ch.live_panL);
            sendBuffers[1].addFrom (1, 0, channelScratch, 1, 0, n, s2 * ch.live_panR);
        }
        if (s3 > 1.0e-5f)
        {
            sendBuffers[2].addFrom (0, 0, channelScratch, 0, 0, n, s3 * ch.live_panL);
            sendBuffers[2].addFrom (1, 0, channelScratch, 1, 0, n, s3 * ch.live_panR);
        }

        ch.live_lastGain = ch.live_gain;

        // Meters (post-fader RMS).
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
        const float gLow = kneeKnobToGain (master.live_isoLow);

        // Hi-pass at 300, then split: low side is just LP@300 already done.
        // For the mid we need HP@300 then LP@3000.
        juce::AudioBuffer<float> midBuf (2, n);
        midBuf.makeCopyOf (sumBuffer, true);
        {
            juce::dsp::AudioBlock<float> blk (midBuf);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            master.xoverHighL.process (ctx);  // HP @ 300
        }
        // Now midBuf = HP@300(sum). Need to LP it at 3000 to get the mid band.
        // We'd need a 4th filter. Cheat: compute high band as HP@3000(sum),
        // and mid = HP@300 - HP@3000 (a band-pass via subtraction is incorrect
        // with LR — but LR4 has the property that LP+HP at the same crossover
        // sums to flat with a phase wrinkle. To get a clean 3-band split we
        // do: low = LP@300; high = HP@3000; mid = sum - low - high.
        juce::AudioBuffer<float> highBuf (2, n);
        highBuf.makeCopyOf (sumBuffer, true);
        {
            juce::dsp::AudioBlock<float> blk (highBuf);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            master.xoverHigh.process (ctx); // HP @ 3000
        }
        const float gHigh = kneeKnobToGain (master.live_isoHigh);
        const float gMid  = kneeKnobToGain (master.live_isoMid);

        // mid = sum - low - high
        for (int c = 0; c < 2; ++c)
        {
            auto* m  = midBuf.getWritePointer (c);
            auto* s  = sumBuffer.getReadPointer (c);
            auto* l  = bandBuffer.getReadPointer (c);
            auto* h  = highBuf.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                m[i] = s[i] - l[i] - h[i];
        }

        sumBuffer.clear (0, n);
        for (int c = 0; c < 2; ++c)
        {
            sumBuffer.addFrom (c, 0, bandBuffer, c, 0, n, gLow);
            sumBuffer.addFrom (c, 0, midBuf,    c, 0, n, gMid);
            sumBuffer.addFrom (c, 0, highBuf,   c, 0, n, gHigh);
        }
    }

    //--- Master output ----------------------------------------------------
    auto masterOut = getBusBuffer<float> (audio, false, kMixerOutMaster);
    auto boothOut  = getBusBuffer<float> (audio, false, kMixerOutBooth);
    masterOut.clear (0, n);
    boothOut.clear (0, n);

    if (! master.live_mute)
    {
        const float gNow = master.live_gain;
        for (int c = 0; c < masterOut.getNumChannels() && c < 2; ++c)
            masterOut.copyFromWithRamp (c, 0, sumBuffer.getReadPointer (c), n,
                                        master.live_lastGain, gNow);
        master.live_lastGain = gNow;
    }

    {
        const float gNow = master.live_booth;
        for (int c = 0; c < boothOut.getNumChannels() && c < 2; ++c)
            boothOut.copyFromWithRamp (c, 0, sumBuffer.getReadPointer (c), n,
                                       master.live_lastBooth, gNow);
        master.live_lastBooth = gNow;
    }

    // Send outputs.
    for (int s = 0; s < kMixerFxSends; ++s)
    {
        auto sendOut = getBusBuffer<float> (audio, false, kMixerOutSendFirst + s);
        sendOut.clear (0, n);
        for (int c = 0; c < sendOut.getNumChannels() && c < 2; ++c)
            sendOut.copyFrom (c, 0, sendBuffers[(size_t) s], c, 0, n);
    }

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

bool AudioMixerProcessor::canApplyBusCountChange (bool isInput, bool isAdding,
                                                  AudioProcessor::BusProperties& outProperties)
{
    if (! isInput)
        return false;
    if (isAdding && ! canAddBus (isInput))
        return false;
    if (! isAdding && ! canRemoveBus (isInput))
        return false;
    if (isAdding)
    {
        outProperties.busName = pendingBusName.isNotEmpty()
                                    ? pendingBusName
                                    : juce::String ("Channel ") + juce::String (getBusCount (true) + 1);
        outProperties.defaultLayout = juce::AudioChannelSet::stereo();
        outProperties.isActivatedByDefault = true;
    }
    return true;
}

//==============================================================================
void AudioMixerProcessor::getStateInformation (juce::MemoryBlock& block)
{
    juce::ValueTree state ("audiomixer");
    state.setProperty ("version", 2, nullptr);
    state.setProperty ("numChannels", (int) channels.size(), nullptr);
    state.setProperty ("masterGain", master.live_gain, nullptr);
    state.setProperty ("masterBooth", master.live_booth, nullptr);
    state.setProperty ("masterMute", master.live_mute, nullptr);
    state.setProperty ("isoLow", master.live_isoLow, nullptr);
    state.setProperty ("isoMid", master.live_isoMid, nullptr);
    state.setProperty ("isoHigh", master.live_isoHigh, nullptr);

    for (auto& chPtr : channels)
    {
        auto& ch = *chPtr;
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
         .setProperty ("send3", ch.send3Target.load(), nullptr);
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
                                           (int) state.getProperty ("numChannels", (int) channels.size()));
    while ((int) channels.size() < wantChannels) addChannel();
    while ((int) channels.size() > wantChannels) removeLastChannel();

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
