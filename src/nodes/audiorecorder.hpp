// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include <element/node.h>
#include <element/processor.hpp>

namespace element {

/** Multi-channel disk recorder.

    Sixteen stereo inputs (32 audio channels). Writes either a single
    multi-channel WAV file containing all pairs interleaved, or one stereo
    WAV file per pair, depending on the `FileMode` setting.

    All file I/O is performed on a background TimeSliceThread via JUCE's
    `AudioFormatWriter::ThreadedWriter`, which is the canonical glitch-free
    pattern for recording to disk from an audio callback: the audio thread
    only pushes samples into a lock-free FIFO; the writer thread drains the
    FIFO and writes to disk asynchronously.

    Pro-quality settings: 24-bit integer PCM or 32-bit float, sample rate
    inherited from the host engine.
*/
class AudioRecorderNode : public Processor
{
public:
    enum class BitDepth
    {
        Int24 = 0,
        Float32 = 1
    };

    enum class FileMode
    {
        OneMultichannelFile = 0, // Single WAV with all 32 channels interleaved
        OneFilePerStereoPair = 1 // 16 WAV files (one per stereo pair)
    };

    static constexpr int numStereoPairs = 16;
    static constexpr int numAudioChannels = numStereoPairs * 2; // 32

    AudioRecorderNode();
    ~AudioRecorderNode() override;

    //==========================================================================
    void prepareToRender (double sampleRate, int maxBlockSize) override;
    void releaseResources() override;

    inline bool wantsContext() const noexcept override { return true; }
    void render (RenderContext&) override;

    //==========================================================================
    /** Begin a new recording. `destinationDirectory` must exist and be writable.
        Returns true if the recording started; false if it was already running
        or if the writers could not be created. Safe to call from the message
        thread only. */
    bool startRecording (const juce::File& destinationDirectory);

    /** Stop the current recording and flush any pending samples to disk. Safe
        to call from the message thread only. */
    void stopRecording();

    /** Returns true while a recording is in progress. */
    bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }

    //==========================================================================
    void setDestinationDirectory (const juce::File&);
    juce::File getDestinationDirectory() const;

    void setBitDepth (BitDepth);
    BitDepth getBitDepth() const noexcept { return bitDepth; }

    void setFileMode (FileMode);
    FileMode getFileMode() const noexcept { return fileMode; }

    /** Display name for the destination folder, suitable for the UI. */
    juce::String getDestinationDisplayName() const;

    /** Approximate elapsed samples since startRecording. Zero when stopped. */
    juce::int64 getElapsedSamples() const noexcept { return elapsedSamples.load (std::memory_order_relaxed); }

    /** Elapsed wall-clock seconds since startRecording. */
    double getElapsedSeconds() const noexcept
    {
        return currentSampleRate > 0.0
                   ? (double) getElapsedSamples() / currentSampleRate
                   : 0.0;
    }

    //==========================================================================
    // Embedding API — used when another processor (e.g. AudioMixerProcessor)
    // wants to drive the recorder from inside its own processBlock instead of
    // through Element's RenderContext graph mechanism.

    /** Push a multichannel audio buffer to the active writers. Channels in
        `buffer` map 1:1 to the recorder's audio channels. Safe to call on
        the audio thread; no allocation, no blocking. */
    void writeAudioBlock (const juce::AudioBuffer<float>& buffer);

    /** Override the per-pair WAV file names. When set, the file names take
        the form "{prefix}-NN-{label}.wav" (with NN = pair number 01..16).
        Pass an empty array to revert to the default "pair01..16" names.
        Must be set before startRecording — changes during a recording are
        ignored. */
    void setStemLabels (const juce::StringArray& labels);

    /** When true, each startRecording() creates a timestamped subfolder
        inside the destination directory and writes the WAVs there. Makes
        keeping multiple takes organised effortless. */
    void setCreateSessionFolder (bool createFolder);
    bool getCreateSessionFolder() const noexcept { return createSessionFolder; }

    /** Limit how many of the 16 pairs are actually written. Useful for
        embedded use where the host only feeds N pairs. Defaults to 16. */
    void setNumActivePairs (int n);
    int  getNumActivePairs() const noexcept { return numActivePairs; }

    /** Returns the most recently created session folder (or destinationDir
        if session folders are off). For UI display. */
    juce::File getLastSessionFolder() const { return lastSessionFolder; }

    //==========================================================================
    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int sizeInBytes) override;

    void getPluginDescription (juce::PluginDescription&) const override;
    void refreshPorts() override;

    int getNumPrograms() const override { return 1; }
    int getCurrentProgram() const override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) const override { return "Recorder"; }

    /** Fires whenever the recorder state changes (recording started/stopped,
        settings updated). UI editors listen to this to refresh. */
    juce::ListenerList<juce::ChangeListener>& getListeners() { return listeners; }
    void addStateListener (juce::ChangeListener* l) { listeners.add (l); }
    void removeStateListener (juce::ChangeListener* l) { listeners.remove (l); }

protected:
    void initialize() override {}

private:
    using ThreadedWriter = juce::AudioFormatWriter::ThreadedWriter;

    bool startMultichannelWriter();
    bool startPairWriters();
    void stopAllWritersInternal();
    juce::String buildBaseFileName() const;
    int getBitsPerSample() const noexcept;
    bool isFloatFormat() const noexcept { return bitDepth == BitDepth::Float32; }

    void notifyListeners();

    // Settings (message thread)
    juce::File destinationDir;
    BitDepth bitDepth = BitDepth::Int24;
    FileMode fileMode = FileMode::OneMultichannelFile;
    juce::StringArray stemLabels;
    bool createSessionFolder = false;
    int  numActivePairs = numStereoPairs;
    juce::File lastSessionFolder;

    // Engine state
    double currentSampleRate = 0.0;
    int currentBlockSize = 0;

    // Recording state
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> elapsedSamples { 0 };

    // Disk-writer thread. Shared across all active ThreadedWriters during a
    // recording session. Owns the background work; we keep it alive for the
    // node's lifetime so starting a recording doesn't pay thread-startup cost.
    juce::TimeSliceThread writerThread { "Element AudioRecorder disk writer" };

    // Active writers. Either `multichannelWriter` is set (OneMultichannelFile
    // mode) or `pairWriters` is populated (OneFilePerStereoPair mode).
    std::unique_ptr<ThreadedWriter> multichannelWriter;
    juce::OwnedArray<ThreadedWriter> pairWriters;

    // Pre-allocated scratch pointer table to feed ThreadedWriter::write() per
    // block without allocating on the audio thread.
    juce::HeapBlock<const float*> scratchChannels;
    int scratchChannelsCount = 0;

    juce::ListenerList<juce::ChangeListener> listeners;
    juce::CriticalSection writerLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRecorderNode)
};

} // namespace element
