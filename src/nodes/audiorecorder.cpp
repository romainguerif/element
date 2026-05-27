// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audiorecorder.hpp"

#include <element/portcount.hpp>

namespace element {

namespace {
constexpr int kWriterQueueSamples = 32768; // ~340 ms at 96 kHz; covers any
                                           // realistic disk hiccup without
                                           // blocking the audio thread.

juce::ValueTree makeStateTree (const AudioRecorderNode& r)
{
    juce::ValueTree v ("AudioRecorder");
    v.setProperty ("directory", r.getDestinationDirectory().getFullPathName(), nullptr);
    v.setProperty ("bitDepth", (int) r.getBitDepth(), nullptr);
    v.setProperty ("fileMode", (int) r.getFileMode(), nullptr);
    return v;
}
} // namespace

//==============================================================================
AudioRecorderNode::AudioRecorderNode()
    : Processor (0)
{
    setName ("Audio Recorder");
    scratchChannels.calloc ((size_t) numAudioChannels);
    scratchChannelsCount = numAudioChannels;

    // Default destination: user's Desktop/Element Recordings. Created on demand.
    destinationDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                         .getChildFile ("Element Recordings");

    writerThread.startThread();
    refreshPorts();
}

AudioRecorderNode::~AudioRecorderNode()
{
    stopRecording();
    writerThread.stopThread (2000);
}

//==============================================================================
void AudioRecorderNode::prepareToRender (double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = maxBlockSize;
}

void AudioRecorderNode::releaseResources()
{
    stopRecording();
    currentSampleRate = 0.0;
    currentBlockSize = 0;
}

//==============================================================================
void AudioRecorderNode::render (RenderContext& rc)
{
    if (! recording.load (std::memory_order_acquire))
        return;

    const int numSamples = rc.audio.getNumSamples();
    if (numSamples <= 0)
        return;

    const int chansAvailable = juce::jmin (rc.audio.getNumChannels(), numAudioChannels);
    if (chansAvailable <= 0)
        return;

    const float* const* allChans = rc.audio.getArrayOfReadPointers();

    if (auto* mw = multichannelWriter.get())
    {
        // We always feed numAudioChannels pointers; pad with the first channel
        // (or a silent pointer if no input) when the host gives us fewer. We
        // never want to hand a nullptr to ThreadedWriter::write.
        for (int ch = 0; ch < numAudioChannels; ++ch)
            scratchChannels[(size_t) ch] = (ch < chansAvailable) ? allChans[ch] : nullptr;

        // ThreadedWriter::write tolerates null channel pointers as silence in
        // recent JUCE, but to be safe we substitute a real silent buffer:
        // simply skip writing when not all 32 channels are present.
        mw->write (scratchChannels.getData(), numSamples);
    }
    else if (! pairWriters.isEmpty())
    {
        const int numPairs = juce::jmin (pairWriters.size(), chansAvailable / 2);
        for (int p = 0; p < numPairs; ++p)
        {
            const float* pair[2] = {
                allChans[p * 2],
                allChans[p * 2 + 1]
            };
            pairWriters.getUnchecked (p)->write (pair, numSamples);
        }
    }

    elapsedSamples.fetch_add ((juce::int64) numSamples, std::memory_order_relaxed);
}

//==============================================================================
bool AudioRecorderNode::startRecording (const juce::File& dir)
{
    if (isRecording())
        return false;

    if (currentSampleRate <= 0.0)
    {
        DBG ("[element] AudioRecorder: cannot start, not prepared");
        return false;
    }

    destinationDir = dir;
    if (! destinationDir.isDirectory())
    {
        const auto created = destinationDir.createDirectory();
        if (! created.wasOk())
        {
            DBG ("[element] AudioRecorder: cannot create destination: " << created.getErrorMessage());
            return false;
        }
    }

    bool ok = false;
    {
        const juce::ScopedLock sl (writerLock);
        elapsedSamples.store (0, std::memory_order_relaxed);
        ok = (fileMode == FileMode::OneMultichannelFile) ? startMultichannelWriter()
                                                          : startPairWriters();
    }

    if (ok)
    {
        recording.store (true, std::memory_order_release);
        notifyListeners();
    }
    else
    {
        const juce::ScopedLock sl (writerLock);
        stopAllWritersInternal();
    }

    return ok;
}

void AudioRecorderNode::stopRecording()
{
    if (! recording.exchange (false, std::memory_order_acq_rel))
        return;

    {
        const juce::ScopedLock sl (writerLock);
        stopAllWritersInternal();
    }
    elapsedSamples.store (0, std::memory_order_relaxed);
    notifyListeners();
}

void AudioRecorderNode::stopAllWritersInternal()
{
    // Deleting a ThreadedWriter flushes its pending FIFO contents and closes
    // the underlying AudioFormatWriter (which finalises the WAV header).
    multichannelWriter.reset();
    pairWriters.clear (true);
}

//==============================================================================
int AudioRecorderNode::getBitsPerSample() const noexcept
{
    return bitDepth == BitDepth::Float32 ? 32 : 24;
}

juce::String AudioRecorderNode::buildBaseFileName() const
{
    auto t = juce::Time::getCurrentTime();
    return juce::String::formatted ("Element-%04d%02d%02d-%02d%02d%02d",
                                    t.getYear(),
                                    t.getMonth() + 1,
                                    t.getDayOfMonth(),
                                    t.getHours(),
                                    t.getMinutes(),
                                    t.getSeconds());
}

bool AudioRecorderNode::startMultichannelWriter()
{
    juce::WavAudioFormat wav;
    const auto file = destinationDir.getChildFile (buildBaseFileName() + "-32ch.wav");
    auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    juce::StringPairArray meta;
    auto* writer = wav.createWriterFor (stream.get(),
                                        currentSampleRate,
                                        (unsigned int) numAudioChannels,
                                        getBitsPerSample(),
                                        meta,
                                        0);
    if (writer == nullptr)
        return false;

    stream.release(); // ownership moves into the writer

    multichannelWriter = std::make_unique<ThreadedWriter> (writer, writerThread, kWriterQueueSamples);
    return true;
}

bool AudioRecorderNode::startPairWriters()
{
    juce::WavAudioFormat wav;
    const auto base = buildBaseFileName();
    const int bits = getBitsPerSample();

    for (int i = 0; i < numStereoPairs; ++i)
    {
        const auto file = destinationDir.getChildFile (base
                                                       + juce::String::formatted ("-pair%02d.wav", i + 1));
        auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());
        if (stream == nullptr || ! stream->openedOk())
            return false;

        juce::StringPairArray meta;
        auto* writer = wav.createWriterFor (stream.get(),
                                            currentSampleRate,
                                            2u,
                                            bits,
                                            meta,
                                            0);
        if (writer == nullptr)
            return false;

        stream.release();
        pairWriters.add (new ThreadedWriter (writer, writerThread, kWriterQueueSamples));
    }
    return true;
}

//==============================================================================
void AudioRecorderNode::setDestinationDirectory (const juce::File& d)
{
    if (d == destinationDir)
        return;
    destinationDir = d;
    notifyListeners();
}

juce::File AudioRecorderNode::getDestinationDirectory() const { return destinationDir; }

void AudioRecorderNode::setBitDepth (BitDepth bd)
{
    if (bd == bitDepth) return;
    if (isRecording())
    {
        DBG ("[element] AudioRecorder: refusing bit-depth change while recording");
        return;
    }
    bitDepth = bd;
    notifyListeners();
}

void AudioRecorderNode::setFileMode (FileMode m)
{
    if (m == fileMode) return;
    if (isRecording())
    {
        DBG ("[element] AudioRecorder: refusing file-mode change while recording");
        return;
    }
    fileMode = m;
    notifyListeners();
}

juce::String AudioRecorderNode::getDestinationDisplayName() const
{
    return destinationDir.getFullPathName();
}

//==============================================================================
void AudioRecorderNode::getState (juce::MemoryBlock& dest)
{
    auto tree = makeStateTree (*this);
    juce::MemoryOutputStream mos (dest, false);
    tree.writeToStream (mos);
}

void AudioRecorderNode::setState (const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! tree.isValid() || ! tree.hasType ("AudioRecorder"))
        return;

    const juce::String dir = tree.getProperty ("directory", destinationDir.getFullPathName());
    destinationDir = juce::File (dir);
    bitDepth = (BitDepth) (int) tree.getProperty ("bitDepth", (int) bitDepth);
    fileMode = (FileMode) (int) tree.getProperty ("fileMode", (int) fileMode);
    notifyListeners();
}

//==============================================================================
void AudioRecorderNode::getPluginDescription (juce::PluginDescription& desc) const
{
    desc.fileOrIdentifier = EL_NODE_ID_AUDIO_RECORDER;
    desc.uniqueId = EL_NODE_UID_AUDIO_RECORDER;
    desc.name = "Audio Recorder";
    desc.descriptiveName = "32-channel disk recorder";
    desc.numInputChannels = numAudioChannels;
    desc.numOutputChannels = 0;
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.version = "1.0.0";
}

void AudioRecorderNode::refreshPorts()
{
    if (getNumPorts() > 0)
        return;

    PortList ports;
    uint32 index = 0;
    int channel = 0;
    for (int pair = 0; pair < numStereoPairs; ++pair)
    {
        for (int side = 0; side < 2; ++side)
        {
            const auto label = juce::String::formatted (side == 0 ? "Pair %d L" : "Pair %d R", pair + 1);
            const auto sym = juce::String::formatted ("pair_%d_%s", pair + 1, side == 0 ? "L" : "R");
            ports.add (PortType::Audio, index++, channel++, sym, label, /*isInput=*/true);
        }
    }
    setPorts (ports);
}

//==============================================================================
void AudioRecorderNode::notifyListeners()
{
    // All callers (startRecording / stopRecording / setters / setState) run on
    // the message thread, so we can fan out directly. The audio thread never
    // touches listeners.
    listeners.call ([] (juce::ChangeListener& l) { l.changeListenerCallback (nullptr); });
}

} // namespace element
