// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/audio_devices.hpp>

#include <element/midiiomonitor.hpp>
#include <element/runmode.hpp>
#include <element/session.hpp>
#include <element/transport.hpp>

namespace element {

class Context;
class Settings;
class RootGraph;

class AudioEngine final : public juce::ReferenceCountedObject {
public:
    Signal<void()> sampleLatencyChanged;
    AudioEngine (Context&, RunMode mode = RunMode::Standalone);
    virtual ~AudioEngine() noexcept;

    //==========================================================================
    RunMode getRunMode() const { return runMode; }

    //==========================================================================
    void activate();
    void deactivate();

    /** Adds a message to the MIDI input.  This can be used by Controllers and 
        UI components that send MIDI in a non-realtime critical situation. Do 
        not call this from the audio thread.
     
        @param msg                      The MidiMessage to send
        @param handleOnDeviceQueue      When true will treat it as if received 
                                        by a MidiInputDevice callback (don't use 
                                        except for debugging)
     */
    void addMidiMessage (const juce::MidiMessage msg, bool handleOnDeviceQueue = false);

    void applySettings (Settings&);

    bool isUsingExternalClock() const;

    void setSession (SessionPtr);
    void refreshSession();

    bool addGraph (RootGraph* graph);
    bool removeGraph (RootGraph* graph);

    void setCurrentGraph (const int index) { setActiveGraph (index); }
    void setActiveGraph (const int index);
    int getActiveGraph() const;

    RootGraph* getGraph (const int index);

    void setPlaying (const bool shouldBePlaying);
    void setRecording (const bool shouldBeRecording);
    void seekToAudioFrame (const int64_t frame);
    void setMeter (int beatsPerBar, int beatDivisor);

    void togglePlayPause();

    juce::MidiKeyboardState& getKeyboardState();
    Transport::MonitorPtr getTransportMonitor() const;
    juce::AudioIODeviceCallback& getAudioIODeviceCallback();
    juce::MidiInputCallback& getMidiInputCallback();

    /** For use by external systems only! e.g. the AU/VST version of Element and
        possibly things like rendering in the future
     */
    void prepareExternalPlayback (const double sampleRate, const int blockSize, const int numIns, const int numOuts);
    void processExternalBuffers (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);
    void processExternalPlayhead (juce::AudioPlayHead* playhead, const int nframes);
    void releaseExternalResources();
    void updateExternalLatencySamples();
    int getExternalLatencySamples() const;

    Context& context() const;
    MidiIOMonitorPtr getMidiIOMonitor() const;

    struct LevelMeter : public juce::ReferenceCountedObject {
        LevelMeter() noexcept {}
        inline double level() const noexcept { return _level.get(); }

    private:
        friend class AudioEngine;

        juce::Atomic<float> _level { 0 };
        void updateLevel (const float* const*, int numChannels, int numSamples) noexcept;
    };

    using LevelMeterPtr = juce::ReferenceCountedObjectPtr<LevelMeter>;

    LevelMeterPtr getLevelMeter (int channel, bool input);
    int getNumChannels (bool input) const noexcept;

    //==========================================================================
    /** A lock-free tap on the master stereo output, intended for analysis UI
        (LUFS / correlation / spectrum). The audio thread pushes blocks into a
        ring buffer; UI components read recent samples at their own cadence.
        Up to ~4 seconds of history are kept at the engine's current sample
        rate, which is enough for short-term LUFS (3 s window). */
    class MasterTap : public juce::ReferenceCountedObject
    {
    public:
        MasterTap();

        /** Current engine sample rate, or 0.0 if the engine is not running. */
        double sampleRate() const noexcept { return _sr.get(); }

        /** Read up to maxSamples of the most recent stereo audio into dest.
            dest[0]/dest[1] must each have room for maxSamples. Returns the
            number of samples actually written (may be less if the engine has
            produced fewer samples since startup or rate change). Older data
            is at lower indices. Safe to call from the message thread. */
        int readLatest (float* destL, float* destR, int maxSamples) const noexcept;

        /** Approximate number of new samples written since the last call to
            consumeWriteCounter(). UI uses this to decide whether to refresh
            spectrogram columns. */
        juce::int64 totalWritten() const noexcept { return _written.get(); }

    private:
        friend class AudioEngine;

        void prepare (double sr);
        void release();
        void writeBlock (const float* const* data, int numChannels, int numSamples) noexcept;

        juce::Atomic<float> _sr { 0.0f };
        juce::Atomic<juce::int64> _written { 0 };

        // Ring buffer sized for ~4 s at 192 kHz so we never have to reallocate
        // during a run. Power of two so we can mask instead of modulo.
        static constexpr int kBufSize = 1 << 20; // 1,048,576 samples (~5.5 s at 192k)
        static constexpr int kBufMask = kBufSize - 1;

        juce::HeapBlock<float> bufL;
        juce::HeapBlock<float> bufR;
        std::atomic<int> writePos { 0 };
    };

    using MasterTapPtr = juce::ReferenceCountedObjectPtr<MasterTap>;
    MasterTapPtr getMasterTap();

private:
    class Private;
    std::unique_ptr<Private> priv;
    Context& world;
    RunMode runMode;
};

using AudioEnginePtr = juce::ReferenceCountedObjectPtr<AudioEngine>;

} // namespace element
