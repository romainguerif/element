// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "nodes/mediaplayer/tempoanalyzer.hpp"
#include "nodes/mediaplayer/timestretcher.hpp"
#include <element/signals.hpp>

#include <atomic>

namespace element {

class AudioFilePlayerNode : public BaseProcessor,
                            public juce::AudioProcessorParameter::Listener,
                            public juce::AsyncUpdater
{
public:
    enum Parameters
    {
        Playing = 0,
        Slave,
        Volume,
        Looping,
        AutoPlay,
        TempoSync,
        LoopStart,
        LoopEnd
    };
    enum MidiPlayState
    {
        None = 0,
        Start,
        Stop,
        Continue
    };

    AudioFilePlayerNode();
    virtual ~AudioFilePlayerNode();

    AudioFormatManager& getAudioFormatManager() { return formats; }
    void setWatchDir (const File& newWatchDir)
    {
        watchDir = newWatchDir;
        jassert (newWatchDir.isDirectory());
    }
    File getWatchDir() const { return watchDir; }

    void handleAsyncUpdate() override;

    void setLooping (const bool shouldLoop);
    bool isLooping() const;

    void setAutoPlayOnLoad (bool shouldAutoPlay);
    bool autoPlaysOnLoad() const;

    void setTempoSyncEnabled (bool enabled);
    bool isTempoSyncEnabled() const;

    void setStretchQuality (TimeStretcher::Quality q);
    TimeStretcher::Quality getStretchQuality() const noexcept { return stretchQuality; }

    /// Loop region in seconds. End <= 0 means loop the whole file.
    void setLoopRegion (double startSec, double endSec);
    double getLoopStart() const noexcept { return loopStartSec.load(); }
    double getLoopEnd() const noexcept   { return loopEndSec.load(); }

    /// Detected (or manually set) tempo in BPM, plus the position of the first beat.
    double getDetectedBpm() const noexcept { return detectedBpm.load(); }
    double getFirstBeatSeconds() const noexcept { return firstBeatSec.load(); }
    void setManualBpm (double bpm);

    bool isAnalyzingTempo() const noexcept { return analyzer.isBusy(); }

    void openFile (const File& file);
    const File& getAudioFile() const { return audioFile; }
    String getWildcard() const { return formats.getWildcardForAllFormats(); }

    bool canLoad (const File& file)
    {
        std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (file));
        return reader != nullptr;
    }

    void fillInPluginDescription (PluginDescription& desc) const override;

    void setRespondToStartStopContinue (bool);
    bool respondsToStartStopContinue() const;

    void enableHostSync (bool sync);
    bool hostSyncEnabled() const noexcept;

    const String getName() const override { return "Audio File Player"; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override;

    bool canAddBus (bool isInput) const override
    {
        ignoreUnused (isInput);
        return false;
    }
    bool canRemoveBus (bool isInput) const override
    {
        ignoreUnused (isInput);
        return false;
    }

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool supportsMPE() const override { return false; }
    bool isMidiEffect() const override { return false; }

    int getNumPrograms() override { return 1; };
    int getCurrentProgram() override { return 0; };
    void setCurrentProgram (int index) override { ignoreUnused (index); };
    const String getProgramName (int index) override
    {
        ignoreUnused (index);
        return getName();
    }
    void changeProgramName (int index, const String& newName) override { ignoreUnused (index, newName); }

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;

    AudioTransportSource& getPlayer() { return player; }

    Signal<void()> restoredState;
    Signal<void()> tempoAnalyzed;
    Signal<void()> fileChanged;

protected:
    bool isBusesLayoutSupported (const BusesLayout&) const override;

private:
    friend class AudioFilePlayerEditor;

    TimeSliceThread thread { "MediaPlayer" };
    std::unique_ptr<AudioFormatReaderSource> reader;
    AudioFormatManager formats;
    AudioTransportSource player;

    AudioParameterBool*  slave { nullptr };
    AudioParameterBool*  playing { nullptr };
    AudioParameterFloat* volume { nullptr };
    AudioParameterBool*  looping { nullptr };
    AudioParameterBool*  autoPlay { nullptr };
    AudioParameterBool*  tempoSync { nullptr };
    AudioParameterFloat* loopStartParam { nullptr };
    AudioParameterFloat* loopEndParam { nullptr };

    File audioFile;
    Atomic<int> midiStartStopContinue;
    Atomic<int> midiPlayState { None };

    bool wasPlaying { false };
    double lastTransportPos { 0.0 };

    File watchDir;

    TempoAnalyzer analyzer;
    TimeStretcher stretcher;
    TimeStretcher::Quality stretchQuality { TimeStretcher::Quality::Eco };

    std::atomic<double> detectedBpm { 0.0 };
    std::atomic<double> firstBeatSec { 0.0 };
    std::atomic<double> loopStartSec { 0.0 };
    std::atomic<double> loopEndSec   { -1.0 };
    std::atomic<bool>   shouldAutoPlay { false };

    juce::AudioBuffer<float> transportScratch;
    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 512 };

    void clearPlayer();
    void kickOffAnalysis();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioFilePlayerNode)
};

} // namespace element
