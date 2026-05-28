// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "nodes/audiorecorder.hpp"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>

namespace element {

// Default sizes for the upgraded analog-style mixer.
constexpr int kMixerDefaultChannels = 6;
constexpr int kMixerMaxChannels     = 16;
constexpr int kMixerFxReturns       = 3;  // fixed
constexpr int kMixerFxSends         = 3;  // fixed, matches returns
// Output bus indices (declared at construction in the same order):
//   0 = Master   1 = Booth   2..4 = FX sends 1..3
constexpr int kMixerOutMaster       = 0;
constexpr int kMixerOutBooth        = 1;
constexpr int kMixerOutSendFirst    = 2;
constexpr int kMixerOutBusCount     = 2 + kMixerFxSends;

class AudioMixerProcessor : public BaseProcessor
{
public:
    //==========================================================================
    /// Per-channel state. The audio thread reads `live` values; the UI writes
    /// the `*_target` atomics. The audio thread pulls those into `live` once
    /// per block (with smoothing where it matters).
    struct Channel
    {
        // Identity
        int          index   = -1;
        int          busIdx  = -1;  ///< Input bus index (stereo)
        juce::String name;

        //-- UI -> audio (atomic) --
        std::atomic<float> gainTarget    { 1.0f };   // linear
        std::atomic<float> panTarget     { 0.0f };   // -1..+1
        std::atomic<float> eqLowTarget   { 0.0f };   // -1..+1 (0 = unity)
        std::atomic<float> eqMidTarget   { 0.0f };
        std::atomic<float> eqHighTarget  { 0.0f };
        std::atomic<float> filterFreqTarget { 20000.0f }; // Hz — knob at full
                                                          // right = effectively
                                                          // open. User sweeps
                                                          // down to add the
                                                          // LP effect.
        std::atomic<float> filterResoTarget { 0.0f };     // 0..1 — flat by default
        std::atomic<int>   filterModeTarget { 0 };        // 0=LP, 1=Bypass, 2=HP
                                                          // — default LP so the
                                                          // filter is "armed".
        std::atomic<float> send1Target   { 0.0f };
        std::atomic<float> send2Target   { 0.0f };
        std::atomic<float> send3Target   { 0.0f };
        std::atomic<bool>  muteTarget    { false };
        std::atomic<bool>  soloTarget    { false };
        std::atomic<bool>  cueTarget     { false };
        // Drive & Transient
        std::atomic<float> driveAmountTarget { 0.0f };  // 0..1
        std::atomic<int>   driveModeTarget   { 0 };     // 0=Tape, 1=Tube, 2=Xformer, 3=SoftClip
        std::atomic<float> transientTarget   { 0.0f };  // -1..+1

        //-- audio -> UI (atomic, for meters) --
        std::atomic<float> rmsL { 0.0f };
        std::atomic<float> rmsR { 0.0f };

        //-- Live (audio thread only) --
        float live_gain     = 1.0f;
        float live_lastGain = 1.0f;
        float live_panL     = std::sqrt (0.5f);
        float live_panR     = std::sqrt (0.5f);
        float live_eqLow = 0.f, live_eqMid = 0.f, live_eqHigh = 0.f;
        float live_filterFreq = 20000.f, live_filterReso = 0.0f;
        int   live_filterMode = 0;   // LP by default, fully open
        bool  live_mute = false;
        bool  live_solo = false;
        bool  live_cue  = false;
        float live_driveAmount = 0.0f;
        int   live_driveMode   = 0;
        float live_transient   = 0.0f;

        //-- DSP per channel (stereo) --
        // Each filter instance handles one audio channel; we keep two for L/R.
        using IIR    = juce::dsp::IIR::Filter<float>;
        using IIRCoef= juce::dsp::IIR::Coefficients<float>;

        std::array<IIR, 2> eqLowFilter;
        std::array<IIR, 2> eqMidFilter;
        std::array<IIR, 2> eqHighFilter;
        juce::dsp::StateVariableTPTFilter<float> svf;  // handles N channels itself

        // Transient shaper — Tale/Saike "Transience" two-envelope model.
        // Stereo-summed sidechain so L and R get the same gain modulation
        // (preserves stereo image). Hand-rolled one-pole peak followers
        // (we don't use juce::dsp::BallisticsFilter because we need full
        // control of the sign convention on the OUTPUT smoother).
        //
        // envFast: 5 ms attack, 50 ms release   — tracks the peak.
        // envSlow: 50 ms attack, 200 ms release — tracks the recent
        //                                          mean / "envelope".
        // sustain detector = max(0, slow_dB - fast_dB) — positive
        //                                                throughout the
        //                                                tail of every
        //                                                transient.
        float envFastState = 0.0f, envSlowState = 0.0f;
        float envFastAtk = 0.0f, envFastRel = 0.0f;
        float envSlowAtk = 0.0f, envSlowRel = 0.0f;
        float transientGainState = 0.0f;
        float transientGainAlpha = 0.0f;

        // Drive — common scaffold, mode-specific behaviour selected at runtime.
        // 2x IIR oversampling for low-latency live use.
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
        std::array<IIR, 2> drivePreShelf;     // Tape/Tube pre-EQ
        std::array<IIR, 2> drivePostShelf;    // Tape post-EQ (de-emphasis)
        std::array<IIR, 2> driveDcBlock;      // post-curve HPF (Tube, Xformer)
        std::array<IIR, 2> driveXoverLow;     // Xformer band split
        std::array<IIR, 2> driveXoverHigh;

        // Per-mode cached coefficient pointers — built once in prepare() so
        // updateDriveFilters() can swap them by index without allocating.
        // 4 modes: 0=Tape, 1=Tube, 2=Xformer, 3=SoftClip.
        IIRCoef::Ptr cachedPre  [4];
        IIRCoef::Ptr cachedPost [4];
        IIRCoef::Ptr cachedDc;        // shared (5 Hz HPF)
        IIRCoef::Ptr cachedXLow;
        IIRCoef::Ptr cachedXHigh;

        float drive_memory[2]  = { 0.0f, 0.0f };       // Xformer lazy hysteresis
        int   drive_lastModeApplied = -1;

        // Pre-allocated scratch for wet/dry blend (real-time safe — no
        // heap allocation in processBlock).
        juce::AudioBuffer<float> dryScratch;

        // Linear-smoothed values for click-free parameter changes (~10 ms).
        // Each one represents the LIVE/ramped value the audio uses, NOT
        // the *Target atomic written from the UI thread.
        juce::LinearSmoothedValue<float> sendSmooth1, sendSmooth2, sendSmooth3;
        juce::LinearSmoothedValue<float> driveAmountSmooth;
        juce::LinearSmoothedValue<float> gateSmooth;  // mute/solo/cue collapsed gain

        void prepare (double sampleRate, int blockSize, int numChannels);
        void updateFilters (double sampleRate);  // recompute coefficients
        void updateDriveFilters (double sampleRate);
    };

    //==========================================================================
    /// 3-band master isolator (LR4 24dB/oct crossovers at 300Hz and 3kHz)
    /// + master / booth gains.
    struct Master
    {
        std::atomic<float> gainTarget   { 1.0f };
        std::atomic<float> boothTarget  { 1.0f };
        std::atomic<bool>  muteTarget   { false };
        std::atomic<float> isoLowTarget { 0.0f };   // -1..+1
        std::atomic<float> isoMidTarget { 0.0f };
        std::atomic<float> isoHighTarget{ 0.0f };

        std::atomic<float> rmsL { 0.0f };
        std::atomic<float> rmsR { 0.0f };
        std::atomic<float> peakL { 0.0f };
        std::atomic<float> peakR { 0.0f };

        // Audio-thread state
        float live_gain = 1.0f, live_lastGain = 1.0f;
        float live_booth = 1.0f, live_lastBooth = 1.0f;
        bool  live_mute = false;
        float live_isoLow = 0.f, live_isoMid = 0.f, live_isoHigh = 0.f;

        // Smoothed master controls. All gain-like values go through 10 ms
        // ramps so the user can slam the fader without zipper / click on
        // the output bus.
        juce::LinearSmoothedValue<float> gainSmooth;
        juce::LinearSmoothedValue<float> boothSmooth;
        juce::LinearSmoothedValue<float> muteGainSmooth;

        // LR4 crossovers — we cascade two LinkwitzRiley LP/HP filters at 300Hz
        // and 3kHz to extract three bands.
        juce::dsp::LinkwitzRileyFilter<float> xoverLow;   // LP @ 300Hz
        juce::dsp::LinkwitzRileyFilter<float> xoverHighL; // HP @ 300Hz then LP @ 3kHz for mid
        juce::dsp::LinkwitzRileyFilter<float> xoverHigh;  // HP @ 3kHz

        void prepare (double sampleRate, int blockSize, int numChannels);
    };

    //==========================================================================
    /// One FX return: stereo input from a return bus + level + to-master.
    struct Return
    {
        int busIdx = -1;
        std::atomic<float> levelTarget { 1.0f };
        std::atomic<bool>  toMasterTarget { true };
        std::atomic<bool>  muteTarget { false };

        std::atomic<float> rmsL { 0.0f };
        std::atomic<float> rmsR { 0.0f };

        float live_level = 1.0f, live_lastLevel = 1.0f;
        bool  live_toMaster = true;
        bool  live_mute = false;
    };

    //==========================================================================
    AudioMixerProcessor (int numTracks = kMixerDefaultChannels,
                         double sampleRate = 44100.0,
                         int bufferSize = 1024);
    ~AudioMixerProcessor() override;

private:
    // Helper to build the initial bus layout with proper names.
    // BusesProperties is protected on juce::AudioProcessor, so this must
    // live inside the class.
    static BusesProperties makeInitialBuses (int numTracks);

public:

    //-- API ----------------------------------------------------------------
    const juce::String getName() const override { return "Audio Mixer"; }
    void fillInPluginDescription (PluginDescription& desc) const override;

    int  getNumChannels() const noexcept;        // active channels
    int  getMaxChannels() const noexcept { return kMixerMaxChannels; }
    Channel* getChannel (int i) const noexcept;
    Master&  getMaster()        noexcept { return master; }
    Return*  getReturn (int i)   noexcept;

    /// Access the embedded multi-track recorder (master + 6 tracks + 3 FX
    /// sends = 10 stereo pairs). Used by the editor's recorder bar.
    AudioRecorderNode& getRecorder() noexcept { return recorder; }

    /// Increases the active channel count by 1 (no bus changes — all
    /// buses are pre-allocated at construction). Returns the new
    /// channel index, or -1 if max reached.
    int  addChannel();
    /// Decreases the active channel count by 1.
    void removeLastChannel();

    //-- AudioProcessor overrides ------------------------------------------
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

    bool isBusesLayoutSupported (const BusesLayout& layout) const override;
    bool canAddBus    (bool /*isInput*/) const override { return false; }
    bool canRemoveBus (bool /*isInput*/) const override { return false; }
    bool canApplyBusCountChange (bool isInput, bool isAdding,
                                 AudioProcessor::BusProperties& outProperties) override;

    bool hasEditor() const override { return true; }
    AudioProcessorEditor* createEditor() override;

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms() override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return getName(); }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

private:
    // Master parameter exposed at the AudioProcessor level so it can be
    // automated by the host. Per-channel state is not exposed as
    // AudioParameter; we keep that internal to avoid a 90-parameter list.
    juce::AudioParameterFloat* masterVolumeParam { nullptr };
    juce::AudioParameterBool*  masterMuteParam   { nullptr };

    // All Channel instances are pre-allocated (sized to kMixerMaxChannels)
    // and never resized at runtime — `activeChannels` tracks how many
    // are currently shown/processed. Bus layout is static, no addBus
    // calls happen after construction. This avoids the host's plugin
    // graph getting confused by a moving target.
    std::vector<std::unique_ptr<Channel>> channels;
    std::atomic<int> activeChannels { kMixerDefaultChannels };
    std::array<Return, kMixerFxReturns> returns;
    Master master;

    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 1024 };

    // Scratch buffers for the sum bus and each send.
    juce::AudioBuffer<float> sumBuffer;
    juce::AudioBuffer<float> sendBuffers[kMixerFxSends];
    juce::AudioBuffer<float> channelScratch;   // re-used per channel
    juce::AudioBuffer<float> bandBuffer;       // isolator: low band
    juce::AudioBuffer<float> midBuffer;        // isolator: mid band
    juce::AudioBuffer<float> highBuffer;       // isolator: high band

    // Embedded multi-track recorder. Captures stems:
    //   pairs 1-6  = post-fader per-channel signal (tracks 1..6)
    //   pairs 7-9  = FX sends 1..3
    //   pair  10   = master output (post-isolator, post-master-gain)
    AudioRecorderNode recorder;
    juce::AudioBuffer<float> recordBuffer;  // 20 channels x blockSize


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioMixerProcessor)
};

} // namespace element
