// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/ui/navigation.hpp>
#include <element/ui/style.hpp>
#include <element/engine.hpp>

#include "nodes/audiofileplayer.hpp"
#include "nodes/mediaplayer/waveformdisplay.hpp"
#include "crashdiagnostics.hpp"

#include "ui/buttons.hpp"
#include "ui/datapathbrowser.hpp"
#include "ui/viewhelpers.hpp"
#include "ui/audioiopanelview.hpp"
#include "ui/sessiontreepanel.hpp"
#include "ui/pluginspanelview.hpp"
#include "ui/filecombobox.hpp"

#include "utils.hpp"

#include <algorithm>
#include <cmath>

using namespace juce;

namespace element {

namespace {
/// Register every audio format JUCE can read on this platform.
/// `registerBasicFormats()` only registers WAV + AIFF; we also want
/// MP3, FLAC, OGG Vorbis, and CoreAudio (which on macOS gives us
/// M4A/AAC/CAF/MP3/AIFF/AIFC via Apple's decoders, including
/// uppercase .AIF and other variants the strict WAV/AIFF readers miss).
inline void registerAllAudioFormats (juce::AudioFormatManager& fm)
{
    fm.clearFormats();
    // Register CoreAudioFormat FIRST on Apple platforms — JUCE's format
    // manager tries readers in registration order, and CoreAudio is much
    // more tolerant of AIFF/AIFC variants, M4A, AAC, CAF, ALAC, and
    // exotic .aif files than the strict built-in readers. Falling through
    // to JUCE's AiffAudioFormat on a non-standard chunk has been observed
    // to produce a "valid" reader that then crashes on read().
   #if JUCE_MAC || JUCE_IOS
    fm.registerFormat (new juce::CoreAudioFormat(), false);
   #endif
    fm.registerBasicFormats();
   #if JUCE_USE_FLAC
    fm.registerFormat (new juce::FlacAudioFormat(),       false);
   #endif
   #if JUCE_USE_OGGVORBIS
    fm.registerFormat (new juce::OggVorbisAudioFormat(),  false);
   #endif
   #if JUCE_USE_MP3AUDIOFORMAT
    fm.registerFormat (new juce::MP3AudioFormat(),        false);
   #endif
   #if JUCE_USE_WINDOWS_MEDIA_FORMAT
    fm.registerFormat (new juce::WindowsMediaAudioFormat(), false);
   #endif
}
} // namespace

class AudioFilePlayerEditor;

class AudioFilePlayerTransport : public juce::Component
{
public:
    AudioFilePlayerTransport()
    {
        addAndMakeVisible (play);
        addAndMakeVisible (stop);
        addAndMakeVisible (rewind);

        setSize (22 * 3 + 2 * 3, 18);
    }

    ~AudioFilePlayerTransport()
    {
        play.onClick = nullptr;
        stop.onClick = nullptr;
        rewind.onClick = nullptr;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        std::vector<Component*> comps = { &play, &stop, &rewind };
        for (auto* c : comps)
        {
            c->setBounds (r.removeFromLeft (_buttonSize));
            r.removeFromLeft (2);
        }
    }

    int requiredWidth()
    {
        const int btnW = std::max (14, _buttonSize), nbtn = 3, pad = 2;
        return nbtn * btnW + (nbtn - 1) * pad;
    }

    void setButtonSize (int newSize)
    {
        _buttonSize = std::max (14, newSize);
        setSize (requiredWidth(), std::max (14, _buttonSize - 6));
    }

private:
    friend class AudioFilePlayerEditor;
    int _buttonSize = 22;
    PlayButton play { "Play" };
    StopButton stop { "Stop" };
    SeekZeroButton rewind { "Seek to Zero" };
};

//==============================================================================
class AudioFilePlayerEditor : public AudioProcessorEditor,
                              public FileComboBoxListener,
                              public ChangeListener,
                              public DragAndDropTarget,
                              public FileDragAndDropTarget,
                              public Timer
{
public:
    AudioFilePlayerEditor (AudioFilePlayerNode& o)
        : AudioProcessorEditor (&o),
          processor (o),
          waveform (o.getAudioFormatManager())
    {
        setOpaque (true);

        chooser.reset (new FileComboBox ("Audio File",
                                         File(),
                                         false, false, false,
                                         o.getWildcard(),
                                         String(),
                                         TRANS ("Select Audio File")));
        addAndMakeVisible (chooser.get());
        chooser->setShowFullPathName (false);

        addAndMakeVisible (watchButton);
        watchButton.setIcon (Icon (getIcons().fasFolderOpen, Colours::black));

        addAndMakeVisible (waveform);

        addAndMakeVisible (transport);

        addAndMakeVisible (loopToggle);
        loopToggle.setButtonText ("Loop");
        addAndMakeVisible (autoPlayToggle);
        autoPlayToggle.setButtonText ("Auto-Play");
        addAndMakeVisible (tempoSyncToggle);
        tempoSyncToggle.setButtonText ("Tempo Sync");
        addAndMakeVisible (startStopContinueToggle);
        startStopContinueToggle.setButtonText (TRANS ("MIDI S/S/C"));
        addAndMakeVisible (hostToggle);
        hostToggle.setClickingTogglesState (true);
        hostToggle.setButtonText (TRANS ("Host"));

        addAndMakeVisible (qualityCombo);
        qualityCombo.addItem ("Eco", 1);
        qualityCombo.addItem ("HiFi", 2);
        qualityCombo.setSelectedId (1, dontSendNotification);

        addAndMakeVisible (bpmLabel);
        bpmLabel.setEditable (false, true, false);
        bpmLabel.setJustificationType (Justification::centred);
        bpmLabel.setColour (Label::backgroundColourId, Colour (0xff15191e));
        bpmLabel.setColour (Label::textColourId, Colours::white);
        bpmLabel.setFont (Font (FontOptions (14.0f, Font::bold)));

        addAndMakeVisible (volume);
        volume.setSliderStyle (Slider::LinearBar);
        volume.setRange (-60.0, 12.0, 0.1);
        volume.setTextBoxIsEditable (false);

        stabilizeComponents();
        bindHandlers();

        setSize (640, 320);
        startTimer (60);
    }

    ~AudioFilePlayerEditor() noexcept
    {
        stopTimer();
        unbindHandlers();
        chooser = nullptr;
    }

    void addRecentsFrom (const File& recentsDir, bool recursive = true)
    {
        if (recentsDir.isDirectory())
        {
            for (DirectoryEntry entry : RangedDirectoryIterator (recentsDir, recursive, processor.getWildcard()))
            {
                if (entry.getFile().isDirectory())
                    continue;
                chooser->addRecentlyUsedFile (entry.getFile());
            }
            sortRecents();
        }
    }

    void timerCallback() override { stabilizeComponents(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { stabilizeComponents(); }

    void stabilizeComponents()
    {
        if (processor.getWatchDir().isDirectory())
            if (chooser->getRecentlyUsedFilenames().isEmpty())
                addRecentsFrom (processor.getWatchDir());

        if (chooser->getCurrentFile() != processor.getAudioFile())
            if (processor.getAudioFile().existsAsFile())
                chooser->setCurrentFile (processor.getAudioFile(), dontSendNotification);

        transport.play.setToggleState (processor.getPlayer().isPlaying(), dontSendNotification);
        loopToggle.setToggleState (processor.isLooping(), dontSendNotification);
        autoPlayToggle.setToggleState (processor.autoPlaysOnLoad(), dontSendNotification);
        tempoSyncToggle.setToggleState (processor.isTempoSyncEnabled(), dontSendNotification);

        waveform.setPlayheadPosition (processor.getPlayer().getCurrentPosition());
        waveform.setLoopEnabled (processor.isLooping());
        waveform.setLoopRegion (processor.getLoopStart(), processor.getLoopEnd());

        const double bpm = processor.getDetectedBpm();
        if (processor.isAnalyzingTempo())
            bpmLabel.setText ("Analyzing...", dontSendNotification);
        else if (bpm > 0.0)
            bpmLabel.setText (String (bpm, 1) + " BPM", dontSendNotification);
        else
            bpmLabel.setText ("--", dontSendNotification);

        waveform.setBeatGrid (processor.getFirstBeatSeconds(), bpm);

        volume.setValue (
            (double) Decibels::gainToDecibels ((double) processor.getPlayer().getGain(), (double) volume.getMinimum()),
            dontSendNotification);

        startStopContinueToggle.setToggleState (processor.respondsToStartStopContinue(),
                                                dontSendNotification);
        hostToggle.setToggleState (processor.hostSyncEnabled(), dontSendNotification);

        qualityCombo.setSelectedId (
            processor.getStretchQuality() == TimeStretcher::Quality::HiFi ? 2 : 1,
            dontSendNotification);
    }

    void fileComboBoxChanged (FileComboBox*) override
    {
        const auto f1 = chooser->getCurrentFile();
        const auto f2 = processor.getAudioFile();
        if (! f1.isDirectory() && f1 != f2)
            processor.openFile (chooser->getCurrentFile());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);

        // Top row: file chooser + watch button.
        auto top = r.removeFromTop (22);
        watchButton.setBounds (top.removeFromRight (22));
        chooser->setBounds (top);
        r.removeFromTop (4);

        // Waveform fills the middle.
        auto wave = r.removeFromTop (180);
        waveform.setBounds (wave);
        r.removeFromTop (6);

        // Transport row: play/stop/rewind, BPM display, volume.
        auto row = r.removeFromTop (28);
        transport.setButtonSize (24);
        transport.setBounds (row.removeFromLeft (transport.requiredWidth()).withSizeKeepingCentre (transport.requiredWidth(), 18));
        row.removeFromLeft (8);
        bpmLabel.setBounds (row.removeFromLeft (100));
        row.removeFromLeft (8);
        qualityCombo.setBounds (row.removeFromLeft (80));
        row.removeFromLeft (8);
        volume.setBounds (row);
        r.removeFromTop (4);

        // Bottom row: toggles.
        auto toggleRow = r.removeFromTop (24);
        const int n = 5;
        const int w = toggleRow.getWidth() / n;
        autoPlayToggle.setBounds (toggleRow.removeFromLeft (w));
        loopToggle.setBounds (toggleRow.removeFromLeft (w));
        tempoSyncToggle.setBounds (toggleRow.removeFromLeft (w));
        hostToggle.setBounds (toggleRow.removeFromLeft (w));
        startStopContinueToggle.setBounds (toggleRow);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colors::widgetBackgroundColor);
    }

    //=========================================================================
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        if (details.description.toString() == "ccNavConcertinaPanel")
            return true;
        return false;
    }

    void itemDropped (const SourceDetails&) override {}

    bool isInterestedInFileDrag (const StringArray& files) override
    {
        if (! File::isAbsolutePath (files[0]))
            return false;
        return processor.canLoad (File (files[0]));
    }

    void filesDropped (const StringArray& files, int x, int y) override
    {
        ignoreUnused (x, y);
        processor.openFile (File (files[0]));
    }

private:
    AudioFilePlayerNode& processor;
    std::unique_ptr<FileComboBox> chooser;
    AudioFilePlayerTransport transport;
    WaveformDisplay waveform;
    Slider volume;
    IconButton watchButton;
    ToggleButton startStopContinueToggle,
        hostToggle,
        loopToggle,
        autoPlayToggle,
        tempoSyncToggle;
    Label bpmLabel { "bpm", "--" };
    ComboBox qualityCombo;
    SignalConnection stateRestoredConnection;
    SignalConnection tempoAnalyzedConnection;
    SignalConnection fileChangedConnection;

    std::unique_ptr<FileChooser> folderChooser;

    void sortRecents()
    {
        auto names = chooser->getRecentlyUsedFilenames();
        names.sort (false);
        chooser->setRecentlyUsedFilenames (names);
    }

    void bindHandlers()
    {
        processor.getPlayer().addChangeListener (this);
        stateRestoredConnection = processor.restoredState.connect (std::bind (
            &AudioFilePlayerEditor::onStateRestored, this));
        tempoAnalyzedConnection = processor.tempoAnalyzed.connect (std::bind (
            &AudioFilePlayerEditor::onTempoAnalyzed, this));
        fileChangedConnection = processor.fileChanged.connect (std::bind (
            &AudioFilePlayerEditor::onFileChanged, this));

        chooser->addListener (this);
        watchButton.onClick = [this]() {
            folderChooser = std::make_unique<FileChooser> ("Select a folder to watch", File(), "*");
            auto safeThis = Component::SafePointer<AudioFilePlayerEditor> (this);
            int flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories;
            folderChooser->launchAsync (flags, [safeThis] (const FileChooser& fc) {
                if (safeThis != nullptr && fc.getResults().size() > 0)
                {
                    safeThis->processor.setWatchDir (fc.getResult());
                    safeThis->addRecentsFrom (safeThis->processor.getWatchDir());
                }
            });
        };

        transport.play.onClick = [this]() {
            if (auto* p = dynamic_cast<AudioParameterBool*> (processor.getParameters()[AudioFilePlayerNode::Playing]))
            { *p = true; stabilizeComponents(); }
        };
        transport.stop.onClick = [this]() {
            if (auto* p = dynamic_cast<AudioParameterBool*> (processor.getParameters()[AudioFilePlayerNode::Playing]))
            { *p = false; stabilizeComponents(); }
        };
        transport.rewind.onClick = [this]() {
            // Rewind = jump to the loop START handle. Falls back to 0 when
            // no loop region has been set (loopStart defaults to 0 anyway).
            processor.getPlayer().setPosition (processor.getLoopStart());
        };

        loopToggle.onClick = [this]() {
            processor.setLooping (! processor.isLooping());
            stabilizeComponents();
        };
        autoPlayToggle.onClick = [this]() {
            processor.setAutoPlayOnLoad (autoPlayToggle.getToggleState());
        };
        tempoSyncToggle.onClick = [this]() {
            processor.setTempoSyncEnabled (tempoSyncToggle.getToggleState());
        };

        volume.onValueChange = [this]() {
            if (auto* const param = dynamic_cast<AudioParameterFloat*> (processor.getParameters()[AudioFilePlayerNode::Volume]))
                *param = static_cast<float> (volume.getValue());
        };

        startStopContinueToggle.onClick = [this]() {
            processor.setRespondToStartStopContinue (startStopContinueToggle.getToggleState() ? 1 : 0);
        };
        hostToggle.onClick = [this]() { processor.enableHostSync (hostToggle.getToggleState()); };

        qualityCombo.onChange = [this]() {
            processor.setStretchQuality (qualityCombo.getSelectedId() == 2
                                             ? TimeStretcher::Quality::HiFi
                                             : TimeStretcher::Quality::Eco);
        };

        bpmLabel.onTextChange = [this]() {
            const auto v = bpmLabel.getText().retainCharacters ("0123456789.").getDoubleValue();
            if (v >= 30.0 && v <= 300.0)
                processor.setManualBpm (v);
            stabilizeComponents();
        };

        waveform.onLoopChanged = [this] (double s, double e) {
            processor.setLoopRegion (s, e);
        };
        waveform.onSeekRequested = [this] (double t) {
            processor.getPlayer().setPosition (t);
        };
    }

    void unbindHandlers()
    {
        stateRestoredConnection.disconnect();
        tempoAnalyzedConnection.disconnect();
        fileChangedConnection.disconnect();

        transport.play.onClick = nullptr;
        transport.stop.onClick = nullptr;
        transport.rewind.onClick = nullptr;

        loopToggle.onClick = nullptr;
        autoPlayToggle.onClick = nullptr;
        tempoSyncToggle.onClick = nullptr;
        volume.onValueChange = nullptr;
        startStopContinueToggle.onClick = nullptr;
        hostToggle.onClick = nullptr;
        qualityCombo.onChange = nullptr;
        bpmLabel.onTextChange = nullptr;
        waveform.onLoopChanged = nullptr;
        waveform.onSeekRequested = nullptr;

        processor.getPlayer().removeChangeListener (this);
        chooser->removeListener (this);
        watchButton.onClick = nullptr;
    }

    void onStateRestored()
    {
        auto watchDir = processor.getWatchDir();
        if (watchDir.exists() && watchDir.isDirectory())
            addRecentsFrom (watchDir, true);
        waveform.setAudioFile (processor.getAudioFile());
    }

    void onTempoAnalyzed() { stabilizeComponents(); }
    void onFileChanged()   { waveform.setAudioFile (processor.getAudioFile()); stabilizeComponents(); }
};

//==============================================================================
AudioFilePlayerNode::AudioFilePlayerNode()
    : BaseProcessor (BusesProperties()
                         .withOutput ("Main", AudioChannelSet::stereo(), true))
{
    addLegacyParameter (playing       = new AudioParameterBool  ({ "playing", 1 }, "Playing", false));
    addLegacyParameter (slave         = new AudioParameterBool  ({ "slave", 1 }, "Slave", false));
    addLegacyParameter (volume        = new AudioParameterFloat ({ "volume", 1 }, "Volume", -60.f, 12.f, 0.f));
    addLegacyParameter (looping       = new AudioParameterBool  ({ "loop", 1 }, "Loop", false));
    addLegacyParameter (autoPlay      = new AudioParameterBool  ({ "autoPlay", 1 }, "Auto-Play", false));
    addLegacyParameter (tempoSync     = new AudioParameterBool  ({ "tempoSync", 1 }, "Tempo Sync", false));
    addLegacyParameter (loopStartParam= new AudioParameterFloat ({ "loopStart", 1 }, "Loop Start", 0.f, 3600.f, 0.f));
    addLegacyParameter (loopEndParam  = new AudioParameterFloat ({ "loopEnd", 1 }, "Loop End", 0.f, 3600.f, 0.f));

    for (auto* const param : getParameters())
        param->addListener (this);

    registerAllAudioFormats (formats);
}

AudioFilePlayerNode::~AudioFilePlayerNode()
{
    for (auto* const param : getParameters())
        param->removeListener (this);
    analyzer.cancel();
    clearPlayer();
    playing = nullptr;
    slave = nullptr;
    volume = nullptr;
}

void AudioFilePlayerNode::setRespondToStartStopContinue (bool respond)
{
    midiStartStopContinue.set (respond ? 1 : 0);
}

bool AudioFilePlayerNode::respondsToStartStopContinue() const
{
    return midiStartStopContinue.get() == 1;
}

void AudioFilePlayerNode::enableHostSync (bool sync)
{
    juce::ScopedLock sl (getCallbackLock());
    *slave = sync;
}

bool AudioFilePlayerNode::hostSyncEnabled() const noexcept
{
    juce::ScopedLock sl (getCallbackLock());
    return *slave;
}

void AudioFilePlayerNode::setAutoPlayOnLoad (bool yes)  { *autoPlay = yes; }
bool AudioFilePlayerNode::autoPlaysOnLoad() const       { return *autoPlay; }
void AudioFilePlayerNode::setTempoSyncEnabled (bool e)  { *tempoSync = e; }
bool AudioFilePlayerNode::isTempoSyncEnabled() const    { return *tempoSync; }

void AudioFilePlayerNode::setStretchQuality (TimeStretcher::Quality q)
{
    if (q == stretchQuality)
        return;
    stretchQuality = q;
    ScopedLock sl (getCallbackLock());
    if (stretcher.isPrepared())
    {
        stretcher.release();
        stretcher.prepare (currentSampleRate, 2, currentBlockSize, q);
    }
}

void AudioFilePlayerNode::setLoopRegion (double s, double e)
{
    loopStartSec.store (jmax (0.0, s));
    loopEndSec.store (e);
    *loopStartParam = (float) jlimit (0.0, 3600.0, s);
    *loopEndParam   = (float) jlimit (0.0, 3600.0, e);
}

void AudioFilePlayerNode::setManualBpm (double bpm)
{
    if (bpm > 0.0)
        detectedBpm.store (bpm);
    tempoAnalyzed();
}

void AudioFilePlayerNode::fillInPluginDescription (PluginDescription& desc) const
{
    desc.name = getName();
    desc.fileOrIdentifier = EL_NODE_ID_AUDIO_FILE_PLAYER;
    desc.descriptiveName = "A single audio file player";
    desc.numInputChannels = 0;
    desc.numOutputChannels = 2;
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = "Element";
    desc.version = "1.0.0";
    desc.uniqueId = EL_NODE_UID_AUDIO_FILE_PLAYER;
}

void AudioFilePlayerNode::clearPlayer()
{
    player.setSource (nullptr);
    reader.reset();
    *playing = player.isPlaying();
}

void AudioFilePlayerNode::kickOffAnalysis()
{
    if (! audioFile.existsAsFile())
        return;

    detectedBpm.store (0.0);
    firstBeatSec.store (0.0);

    // Capture a weak reference so the callback (which is dispatched on
    // the message thread by the analyzer) never accesses a destroyed
    // node — e.g. if the user removes the node while analysis is in
    // flight.
    juce::WeakReference<AudioFilePlayerNode> weak (this);
    analyzer.analyze (audioFile, formats, [weak] (TempoAnalyzer::Result r) {
        if (auto* self = weak.get())
        {
            if (r.valid)
            {
                self->detectedBpm.store (r.bpm);
                self->firstBeatSec.store (r.firstBeatSeconds);
            }
            self->tempoAnalyzed();
        }
    });
}

void AudioFilePlayerNode::openFile (const File& file)
{
    diagnostics::breadcrumb ("afp", ("openFile: " + file.getFullPathName()).toRawUTF8());
    if (file == audioFile)
    {
        diagnostics::breadcrumb ("afp", "openFile: same file, ignoring");
        return;
    }

    // Create the reader OUTSIDE any locks. This may do I/O.
    AudioFormatReader* newReader = formats.createReaderFor (file);
    if (newReader == nullptr)
    {
        diagnostics::breadcrumb ("afp", "openFile: createReaderFor returned null");
        return;
    }
    {
        char msg[256];
        std::snprintf (msg, sizeof (msg), "createReaderFor ok: ch=%u sr=%g len=%lld",
                       newReader->numChannels,
                       newReader->sampleRate,
                       (long long) newReader->lengthInSamples);
        diagnostics::breadcrumb ("afp", msg);
    }

    // Wire up the new source. We follow the original pattern: clear the
    // old player and install the new source without holding the callback
    // lock, then take the lock briefly only to flip looping / reset the
    // stretcher. Holding the lock across player.setSource has been seen
    // to deadlock against the TimeSliceThread that AudioTransportSource
    // uses for background pre-fetching.
    diagnostics::breadcrumb ("afp", "clearPlayer");
    clearPlayer();
    diagnostics::breadcrumb ("afp", "new AudioFormatReaderSource");
    reader.reset (new AudioFormatReaderSource (newReader, true));
    audioFile = file;
    diagnostics::breadcrumb ("afp", "player.setSource");
    player.setSource (reader.get(), 1024 * 8, &thread, newReader->sampleRate, 2);

    {
        ScopedLock sl (getCallbackLock());
        // Looping is handled entirely in processBlock so the wrap point
        // honours the loopStart handle (JUCE's built-in loop wraps to 0).
        reader->setLooping (false);
        player.setLooping (false);
        if (stretcher.isPrepared())
            stretcher.reset();
    }
    diagnostics::breadcrumb ("afp", "player.setSource done");

    const double lenSec = player.getLengthInSeconds();
    loopStartSec.store (0.0);
    loopEndSec.store (lenSec);
    *loopStartParam = 0.0f;
    *loopEndParam   = (float) jlimit (0.0, 3600.0, lenSec);

    diagnostics::breadcrumb ("afp", "fileChanged()");
    fileChanged();
    diagnostics::breadcrumb ("afp", "kickOffAnalysis");
    kickOffAnalysis();
    diagnostics::breadcrumb ("afp", "kickOffAnalysis done");

    if (*autoPlay)
    {
        diagnostics::breadcrumb ("afp", "autoPlay -> playing=true");
        *playing = true;
    }
    diagnostics::breadcrumb ("afp", "openFile complete");
}

void AudioFilePlayerNode::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = maximumExpectedSamplesPerBlock;

    thread.startThread();
    if (formats.getNumKnownFormats() == 0)
        registerAllAudioFormats (formats);
    player.prepareToPlay (maximumExpectedSamplesPerBlock, sampleRate);

    stretcher.release();
    stretcher.prepare (sampleRate, 2, maximumExpectedSamplesPerBlock, stretchQuality);

    transportScratch.setSize (2, maximumExpectedSamplesPerBlock * 8, false, true, true);

    if (reader)
    {
        double readerSampleRate = sampleRate;
        if (auto* fmtReader = reader->getAudioFormatReader())
            readerSampleRate = fmtReader->sampleRate;

        reader->setLooping (false);
        player.setLooping (false);
        player.setSource (reader.get(), 1024 * 8, &thread, readerSampleRate, 2);
        player.setPosition (jmax (0.0, lastTransportPos));
        if (wasPlaying)
            player.start();
    }
    else
    {
        clearPlayer();
    }
}

void AudioFilePlayerNode::releaseResources()
{
    lastTransportPos = player.getCurrentPosition();
    wasPlaying = player.isPlaying();

    player.stop();
    player.releaseResources();
    player.setSource (nullptr);
    stretcher.release();
    thread.stopThread (14);
}

void AudioFilePlayerNode::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const auto nframes = buffer.getNumSamples();
    for (int c = buffer.getNumChannels(); --c >= 0;)
        buffer.clear (c, 0, nframes);

    ScopedLock sl (getCallbackLock());
    const bool hostSync = *slave;
    const bool useStretch = *tempoSync && detectedBpm.load() > 0.0;

    // Host sync: align position on transport rewind, propagate play state.
    if (hostSync)
    {
        if (auto* const playhead = getPlayHead())
        {
            auto pos = playhead->getPosition();
            if (pos)
            {
                if (pos->getTimeInSamples() == 0 && player.getCurrentPosition() != 0.0)
                    player.setPosition (0.0);

                if (player.isPlaying() != pos->getIsPlaying())
                {
                    midiPlayState.set (pos->getIsPlaying() ? Continue : Stop);
                    triggerAsyncUpdate();
                }
            }
        }
    }

    // Compute time-stretch rate from host BPM vs detected clip BPM.
    if (useStretch)
    {
        double rate = 1.0;
        if (auto* const playhead = getPlayHead())
        {
            if (auto pos = playhead->getPosition())
            {
                if (auto hostBpm = pos->getBpm())
                {
                    if (*hostBpm > 0.0)
                        rate = *hostBpm / detectedBpm.load();
                }
            }
        }
        stretcher.setPlaybackRate (rate);
    }

    // Loop region: jump to loopStart when the NEXT block would cross
    // loopEnd. Threshold = one block's worth of seconds so we wrap
    // sample-clean instead of letting the player run past the end and
    // stop (JUCE's internal looping is disabled so we own this entirely).
    if (*looping && reader != nullptr)
    {
        const double curPos = player.getCurrentPosition();
        const double ls = loopStartSec.load();
        const double le = loopEndSec.load();
        const double blockDur = (currentSampleRate > 0.0)
                                    ? ((double) nframes / currentSampleRate)
                                    : 0.01;
        if (le > ls + 1.0e-3 && curPos >= le - blockDur)
            player.setPosition (ls);
    }

    AudioSourceChannelInfo info;
    int start = 0;

    auto pullFromTransport = [this] (AudioBuffer<float>& dest, int needed) -> int {
        const int avail = jmin (needed, dest.getNumSamples());
        if (avail <= 0)
            return 0;
        AudioSourceChannelInfo pulled;
        pulled.buffer = &dest;
        pulled.startSample = 0;
        pulled.numSamples = avail;
        player.getNextAudioBlock (pulled);
        return avail;
    };

    // MIDI start/stop/continue handling — only when host sync is off and that
    // mode is enabled by the user. We split the block on each MIDI message.
    if (! hostSync && midiStartStopContinue.get() == 1)
    {
        for (auto m : midi)
        {
            const auto msg = m.getMessage();
            const int seg = m.samplePosition - start;
            if (seg > 0)
            {
                if (useStretch)
                {
                    stretcher.process (buffer, start, seg, pullFromTransport);
                }
                else
                {
                    info.buffer = &buffer;
                    info.startSample = start;
                    info.numSamples = seg;
                    player.getNextAudioBlock (info);
                }
            }

            if (msg.isMidiStart())       { midiPlayState.set (Start);    triggerAsyncUpdate(); }
            else if (msg.isMidiContinue()) { midiPlayState.set (Continue); triggerAsyncUpdate(); }
            else if (msg.isMidiStop())   { midiPlayState.set (Stop);     triggerAsyncUpdate(); }

            start = m.samplePosition;
        }
    }

    if (start < nframes)
    {
        const int seg = nframes - start;
        if (useStretch)
        {
            stretcher.process (buffer, start, seg, pullFromTransport);
        }
        else
        {
            info.buffer = &buffer;
            info.startSample = start;
            info.numSamples = seg;
            player.getNextAudioBlock (info);
        }
    }

    midi.clear();
}

void AudioFilePlayerNode::setLooping (const bool shouldLoop)
{
    jassert (looping != nullptr);
    *looping = shouldLoop;
}

bool AudioFilePlayerNode::isLooping() const
{
    return *looping;
}

void AudioFilePlayerNode::handleAsyncUpdate()
{
    switch (midiPlayState.get())
    {
        case Start:    player.setPosition (0.0); player.start(); break;
        case Stop:     player.stop();                            break;
        case Continue: player.start();                           break;
        case None:
        default: break;
    }
    midiPlayState.set (None);
}

AudioProcessorEditor* AudioFilePlayerNode::createEditor()
{
    return new AudioFilePlayerEditor (*this);
}

void AudioFilePlayerNode::getStateInformation (juce::MemoryBlock& destData)
{
    ValueTree state (tags::state);
    state.setProperty ("audioFile", audioFile.getFullPathName(), nullptr)
        .setProperty ("playing", (bool) *playing, nullptr)
        .setProperty ("slave", (bool) *slave, nullptr)
        .setProperty ("loop", (bool) *looping, nullptr)
        .setProperty ("autoPlay", (bool) *autoPlay, nullptr)
        .setProperty ("tempoSync", (bool) *tempoSync, nullptr)
        .setProperty ("loopStart", loopStartSec.load(), nullptr)
        .setProperty ("loopEnd",   loopEndSec.load(), nullptr)
        .setProperty ("bpm",       detectedBpm.load(), nullptr)
        .setProperty ("firstBeat", firstBeatSec.load(), nullptr)
        .setProperty ("stretchQuality", stretchQuality == TimeStretcher::Quality::HiFi ? 1 : 0, nullptr)
        .setProperty ("midiStartStopContinue", midiStartStopContinue.get() == 1, nullptr);

    if (watchDir.exists())
        state.setProperty ("watchDir", watchDir.getFullPathName(), nullptr);

    MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void AudioFilePlayerNode::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! state.isValid())
        return;

    if (File::isAbsolutePath (state["audioFile"].toString()))
        openFile (File (state["audioFile"].toString()));

    *playing   = (bool) state.getProperty ("playing", false);
    *slave     = (bool) state.getProperty ("slave", false);
    *looping   = (bool) state.getProperty ("loop", false);
    *autoPlay  = (bool) state.getProperty ("autoPlay", false);
    *tempoSync = (bool) state.getProperty ("tempoSync", false);

    const double ls = (double) state.getProperty ("loopStart", 0.0);
    const double le = (double) state.getProperty ("loopEnd",   -1.0);
    loopStartSec.store (ls);
    loopEndSec.store (le);

    const double savedBpm = (double) state.getProperty ("bpm", 0.0);
    if (savedBpm > 0.0)
        detectedBpm.store (savedBpm);
    const double savedBeat = (double) state.getProperty ("firstBeat", 0.0);
    firstBeatSec.store (savedBeat);

    stretchQuality = ((int) state.getProperty ("stretchQuality", 0)) == 1
                         ? TimeStretcher::Quality::HiFi
                         : TimeStretcher::Quality::Eco;

    midiStartStopContinue.set ((bool) state.getProperty ("midiStartStopContinue", false) ? 1 : 0);

    if (state.hasProperty ("watchDir"))
    {
        auto watchPath = state["watchDir"].toString();
        if (File::isAbsolutePath (watchPath))
            watchDir = File (watchPath);
    }

    restoredState();
}

void AudioFilePlayerNode::parameterValueChanged (int parameter, float newValue)
{
    ignoreUnused (newValue);

    switch (parameter)
    {
        case Playing:
            if (*playing) player.start();
            else          player.stop();
            break;
        case Slave: break;
        case Volume:
            player.setGain (Decibels::decibelsToGain (volume->get(), volume->range.start));
            break;
        case Looping:
            // No-op on JUCE objects — looping is implemented in processBlock
            // so we can wrap to the loopStart handle, not to absolute 0.
            break;
        case AutoPlay:  break;
        case TempoSync:
            if (! *tempoSync)
                stretcher.reset();
            break;
        case LoopStart:
            loopStartSec.store ((double) loopStartParam->get());
            break;
        case LoopEnd:
            loopEndSec.store ((double) loopEndParam->get());
            break;
    }
}

void AudioFilePlayerNode::parameterGestureChanged (int parameterIndex, bool gestureIsStarting)
{
    ignoreUnused (parameterIndex, gestureIsStarting);
}

bool AudioFilePlayerNode::isBusesLayoutSupported (const BusesLayout& layout) const
{
    if (layout.inputBuses.size() > 0 || layout.outputBuses.size() > 1)
        return false;
    return layout.getMainOutputChannelSet() == AudioChannelSet::stereo()
        || layout.getMainOutputChannelSet() == AudioChannelSet::mono();
}

} // namespace element
