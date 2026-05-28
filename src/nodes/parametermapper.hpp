// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include <element/node.h>
#include <element/processor.hpp>

namespace element {

/** A 64-slot parameter-mapper node, designed around the DJ TechTools
    Midi Fighter Twister's default layout.

    Four banks of sixteen slots each. Each slot maps to one
    AudioProcessorParameter on another node in the same parent graph, via a
    "learn" workflow:

      1. The user clicks the Learn button on the editor.
      2. The user clicks a knob to select it (`beginLearn(slot)`).
      3. The user moves any parameter elsewhere in the graph; the first
         change is captured and stored as the mapping for the selected slot.

    Bidirectional Twister integration on the node's MIDI ports:

      - **Encoder turn (incoming)**: CC 0..63 on channel 1.
        Bank N (1..4) maps to CCs `(N-1)*16 .. (N-1)*16 + 15`.
      - **Encoder LED ring (outgoing)**: same CC, same channel, value 0..127.
        Whenever a slot value changes from a UI action (or any source other
        than incoming MIDI from the Twister), we echo it back to update the
        LED ring position.
      - **Bank change (incoming)**: NoteOn on channel 4, note 0..3 selects
        bank 1..4. NoteOff is sent first; we ignore it and act on NoteOn.
      - **Bank change (outgoing)**: when the UI switches bank, we send
        NoteOn channel 4, note = new bank index, so the Twister's LED bank
        indicator stays in sync.

    Audio: none. The node has a single MIDI input and a single MIDI output;
    the user wires the Twister's MIDI input / output ports through Element's
    MIDI IO nodes to feed this mapper.

    Future extensions (not implemented here): snapshot capture of all 64
    slot values per bank, lerp-animated morphing between snapshots, encoder
    push-button second value, runtime RGB colour control.
*/
class ParameterMapperNode : public Processor,
                            private juce::Timer
{
public:
    static constexpr int kNumBanks   = 4;
    static constexpr int kKnobsPerBank = 16;
    static constexpr int kNumSlots   = kNumBanks * kKnobsPerBank; // 64

    static constexpr int kNumSnapshots = 16;

    struct Slot
    {
        juce::uint32 targetNodeId = 0; // 0 means unmapped
        int targetParamIndex = -1;     // -1 means unmapped
        juce::String label;            // custom label, fallback to the param name
        float value = 0.0f;            // last knob value in [0, 1]
    };

    /** A snapshot captures the full mapper state (64 slot values).

        Use case: the user prepares snapshots for upcoming "places" in a
        live performance, then activates a preview to see, per knob, where
        the target value sits relative to the current one. They transition
        manually -- no automated lerp -- guided by per-knob target markers
        and a proximity halo around each cell. This is intentionally
        non-deterministic: the artist drives the transition by hand. */
    struct Snapshot
    {
        bool hasData = false;
        float values[kNumSlots] = {};
    };

    ParameterMapperNode();
    ~ParameterMapperNode() override;

    //==========================================================================
    void prepareToRender (double, int) override {}
    void releaseResources() override {}
    bool wantsContext() const noexcept override { return true; }
    void render (RenderContext&) override;

    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int) override;

    void getPluginDescription (juce::PluginDescription&) const override;
    void refreshPorts() override;

    int getNumPrograms() const override { return 1; }
    int getCurrentProgram() const override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) const override { return "Mapper"; }

    //==========================================================================
    // Banks.
    int getCurrentBank() const noexcept { return currentBank.load (std::memory_order_acquire); }

    /** Sets the current bank (0..3). If `sendToHardware` is true, also queues
        a NoteOn message on channel 4 so a connected Twister stays in sync. */
    void setCurrentBank (int newBank, bool sendToHardware = true);

    /** Queue CC messages for every slot in the given bank (16 CCs), so the
        Twister's LED rings reflect those values. */
    void pushBankToHardware (int bank);

    /** Queue CC messages for all 64 slots. Used by the editor's "Sync"
        button and after loading a session. */
    void pushAllToHardware();

    //==========================================================================
    // Snapshots & preview mode.

    /** Record the current 64 slot values into snapshot[i]. */
    void recordSnapshot (int snapshotIndex);

    /** Erase a snapshot. */
    void clearSnapshot (int snapshotIndex);

    /** Returns a copy of the snapshot data. `hasData == false` for empty
        slots. */
    Snapshot getSnapshot (int snapshotIndex) const;

    /** Returns true if the slot has data recorded. */
    bool snapshotHasData (int snapshotIndex) const;

    /** Returns the value stored for `slotIndex` inside snapshot
        `snapshotIndex`, or NaN if there's no data. */
    float getSnapshotValue (int snapshotIndex, int slotIndex) const;

    /** Enters preview mode for a given snapshot (must have data). Editors
        listen and draw per-knob target markers + a proximity halo while
        active. Set to -1 (the default) to exit preview mode. */
    void setPreviewSnapshot (int snapshotIndex);

    /** Returns the index of the snapshot currently being previewed, or -1
        when no preview is active. */
    int getPreviewSnapshot() const noexcept { return previewSnapshot.load (std::memory_order_acquire); }

    /** Toggles the editor's "preview mode" flag. When false (default), a
        click on a snapshot ARMS it for application at the next bar
        boundary -- non-destructive, transport-synchronised, no automated
        morph between values. When true, clicks instead enter preview
        visualisation (which doesn't change parameter values). */
    void setPreviewModeEnabled (bool enabled);
    bool isPreviewModeEnabled() const noexcept { return previewModeEnabled.load (std::memory_order_acquire); }

    /** Arm a snapshot to be applied at the next bar boundary. -1 cancels. */
    void armSnapshot (int snapshotIndex);
    int  getArmedSnapshot() const noexcept { return armedSnapshot.load (std::memory_order_acquire); }

    /** Immediately apply a snapshot (used when crossing a bar in render,
        or could be invoked from the UI for instant switching). Writes the
        slot values and pushes them to their bound parameters. */
    void applySnapshot (int snapshotIndex);

    //==========================================================================
    // UI-facing API. All called from the message thread.

    /** Returns a copy of the slot's current state. */
    Slot getSlot (int absoluteIndex) const;

    /** Updates the knob value and, if mapped, pushes the new value to the
        bound parameter. If the change came from the UI, also echoes a CC to
        the MIDI output so a connected Twister updates its LED ring. */
    void setKnobValue (int absoluteIndex, float v);

    /** Replaces the custom label for a slot. */
    void setSlotLabel (int absoluteIndex, const juce::String& newLabel);

    /** Removes the mapping for a slot. */
    void unmap (int absoluteIndex);

    //==========================================================================
    // Learn workflow.
    void beginLearn (int absoluteIndex);
    void cancelLearn();
    bool isLearning() const noexcept { return learningSlot.load (std::memory_order_acquire) >= 0; }
    int  getLearningSlot() const noexcept { return learningSlot.load (std::memory_order_acquire); }

    //==========================================================================
    juce::String getDisplayLabel (int absoluteIndex) const;
    juce::String getMappedParamName (int absoluteIndex) const;

    void addStateListener    (juce::ChangeListener* l) { listeners.add (l); }
    void removeStateListener (juce::ChangeListener* l) { listeners.remove (l); }

protected:
    void initialize() override {}

private:
    struct ParamWatch;
    friend struct ParamWatch;

    void attachLearnListeners();
    void detachLearnListeners();
    juce::AudioProcessorParameter* resolveParameter (juce::uint32 nodeId, int paramIndex) const;
    void notifyListeners();
    void handleCapturedChange (juce::uint32 nodeId, int paramIndex);

    void queueMidiOut (const juce::MidiMessage& msg);

    // Two strategies for the LED-ring guide on the Twister depending on the
    // firmware running on the device:
    //
    //   * Default (factory) mode: ch.1 CCs control both the LED ring AND
    //     the encoder's internal value, so we can't oscillate without
    //     making the encoder jump. We send a *brightness* animation on
    //     ch.6 instead -- the ring pulses to advertise "this encoder is
    //     being guided", path detail only on screen.
    //
    //   * Native mode (Trinitou open-source firmware): encoders send
    //     RELATIVE CCs and the LED ring is purely host-controlled. We
    //     oscillate the displayed position on ch.1 between current and
    //     target at ~11 Hz; the gap between the two perceived positions
    //     IS the path remaining, drawn directly on the white LED ring.
    void updateRingGuide();
    void timerCallback() override;
    bool ringTargetTick = false;

public:
    //==========================================================================
    /** Toggle Trinitou's Native Mode protocol on the connected Twister.
        Sends the activation SysEx (F0 000179 05 00 01 F7) when enabled and
        the deactivation SysEx when disabled. In Native Mode the encoders
        are always relative (63=-1, 65=+1) per spec, and host LED writes no
        longer touch the encoder's internal value -- which is what unlocks
        the safe ring-oscillation guide.

        Native Mode is not persistent on the device: it resets on
        disconnect, so this also re-sends activation any time it's turned
        on (e.g. after a session reload, or if the user toggles it off/on
        after replugging the Twister). */
    void setNativeMode (bool enabled);
    bool isNativeMode() const noexcept { return nativeMode.load (std::memory_order_acquire); }

private:
    std::atomic<bool> nativeMode { false };

    // Wall-clock millisecond counter of the last incoming relative tick from
    // the Twister. The ring-guide timer consults this to pause the
    // oscillation for a short grace period after every user gesture --
    // otherwise our outgoing CCs immediately overwrite the position the
    // user just dialled in, and they perceive the LED as "stuck".
    std::atomic<juce::uint32> lastUserTickMs { 0 };

    Slot slots[kNumSlots];
    Snapshot snapshots[kNumSnapshots];

    std::atomic<int>  learningSlot       { -1 };
    std::atomic<int>  currentBank        { 0 };
    std::atomic<int>  previewSnapshot    { -1 };
    std::atomic<int>  armedSnapshot      { -1 };
    std::atomic<bool> previewModeEnabled { false };

    // Bar-edge tracking for armed snapshot application. Touched only on the
    // audio thread.
    int lastBarSeen = -1;

    juce::OwnedArray<ParamWatch> watches;
    juce::ListenerList<juce::ChangeListener> listeners;

    // Outgoing MIDI buffer. UI / message thread queues messages here; the
    // audio thread drains them in render() into the actual MIDI output
    // buffer. Guarded by a short critical section -- this is not on the hot
    // RT path for inner loops (a handful of CC writes per UI interaction).
    juce::CriticalSection outQueueLock;
    juce::MidiBuffer outQueue;

    juce::WeakReference<ParameterMapperNode>::Master masterReference;
    friend class juce::WeakReference<ParameterMapperNode>;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterMapperNode)
};

} // namespace element
