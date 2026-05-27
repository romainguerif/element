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
class ParameterMapperNode : public Processor
{
public:
    static constexpr int kNumBanks   = 4;
    static constexpr int kKnobsPerBank = 16;
    static constexpr int kNumSlots   = kNumBanks * kKnobsPerBank; // 64

    struct Slot
    {
        juce::uint32 targetNodeId = 0; // 0 means unmapped
        int targetParamIndex = -1;     // -1 means unmapped
        juce::String label;            // custom label, fallback to the param name
        float value = 0.0f;            // last knob value in [0, 1]
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

    Slot slots[kNumSlots];

    std::atomic<int> learningSlot { -1 };
    std::atomic<int> currentBank  { 0 };

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
