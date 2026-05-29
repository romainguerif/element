// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/parametermapper.hpp"

#include <algorithm>

#include <element/midipipe.hpp>
#include <element/portcount.hpp>
#include "crashdiagnostics.hpp"
#include "engine/graphnode.hpp"

namespace element {

namespace {
// DJ TechTools Midi Fighter Twister default firmware MIDI map.
constexpr int kEncoderChannel  = 1; // encoder turn (absolute) + LED ring position
constexpr int kRingAnimChannel = 6; // LED ring brightness animation
constexpr int kBankChannel     = 4; // NoteOn 0..3 = bank 1..4

// Trinitou open-source "Native Mode" firmware MIDI map (subset we use):
//   ch.1 in : knob twist, CC 0..15, value 63=left tick, 65=right tick
//   ch.1 out: LED ring position, CC 0..15, value 0..127 (host-controlled,
//             does NOT touch the encoder's internal value)
//   ch.3 in : side button, CC 0..5, value 127=pressed
constexpr int kNativeSideButtonChannel = 3;
constexpr int kNativeSideBtnPrev = 1; // LH middle: previous bank
constexpr int kNativeSideBtnNext = 4; // RH middle: next bank

// Animation values picked for the default-mode guide.
constexpr int kAnimOff           = 0;
constexpr int kAnimArmedGate     = 4;   // gate every 1/2 beat
constexpr int kAnimPreviewPulse  = 14;  // pulse every 1/2 beat

// Native-mode LED oscillation tick. 120 ms (~8 Hz) keeps the oscillation
// readable while leaving headroom on the Twister's USB pipe so incoming
// encoder ticks from the user aren't starved by our outgoing LED traffic.
constexpr int kNativeOscillationMs = 120;

// Native-mode relative encoder step. 1/64 (instead of 1/127) gives ~6.4
// turns to traverse the full 0..1 range, which feels closer to the
// resolution a user expects from the Twister's hardware detents.
constexpr float kNativeRelativeStep = 1.0f / 64.0f;

// How long the LED-ring oscillation pauses after the user moves an
// encoder, so the user sees their physical movement on the ring without
// the oscillation overwriting it every 120 ms.
constexpr juce::uint32 kUserInteractionGraceMs = 350;
} // namespace

struct ParameterMapperNode::ParamWatch : public juce::AudioProcessorParameter::Listener
{
    ParamWatch (ParameterMapperNode& m,
                juce::uint32 nodeId,
                int paramIndex_,
                juce::AudioProcessorParameter* p)
        : mapper (&m), source (nodeId), paramIndex (paramIndex_), param (p)
    {
        if (param)
            param->addListener (this);
    }

    ~ParamWatch() override
    {
        if (param)
            param->removeListener (this);
    }

    void parameterValueChanged (int, float) override
    {
        const juce::WeakReference<ParameterMapperNode> weak { mapper.get() };
        const auto srcNode = source;
        const auto srcParam = paramIndex;
        juce::MessageManager::callAsync ([weak, srcNode, srcParam]() {
            if (auto* m = weak.get())
                m->handleCapturedChange (srcNode, srcParam);
        });
    }

    void parameterGestureChanged (int, bool) override {}

    juce::WeakReference<ParameterMapperNode> mapper;
    juce::uint32 source;
    int paramIndex;
    juce::AudioProcessorParameter* param;
};

//==============================================================================
ParameterMapperNode::ParameterMapperNode()
    : Processor (0)
{
    setName ("Parameter Mapper");
    for (auto& v : lastAutoValue)
        v = std::numeric_limits<float>::quiet_NaN();
    refreshPorts();
}

ParameterMapperNode::~ParameterMapperNode()
{
    masterReference.clear();
    detachLearnListeners();
}

//==============================================================================
ParameterMapperNode::Slot ParameterMapperNode::getSlot (int index) const
{
    if (index < 0 || index >= kNumSlots)
        return {};
    return slots[index];
}

bool ParameterMapperNode::syncValuesFromTargets()
{
    bool changed = false;
    for (int i = 0; i < kNumSlots; ++i)
    {
        if (slots[i].targetParamIndex < 0)
            continue;
        // A slot with automation is driven by render() from its curve, which
        // is the authoritative (smooth) value. Reading the value back from
        // the parameter here would fight that — and for quantised targets
        // (choice/bool) the snapped read-back makes the knob jitter — so
        // leave automated slots to render(). (No lock needed: the points are
        // only edited on this, the message, thread; render() only reads them.)
        if (! automation[i].empty())
            continue;
        if (auto* param = resolveParameter (slots[i].targetNodeId, slots[i].targetParamIndex))
        {
            const float v = juce::jlimit (0.0f, 1.0f, param->getValue());
            if (! juce::approximatelyEqual (slots[i].value, v))
            {
                slots[i].value = v;
                changed = true;
            }
        }
    }
    return changed;
}

void ParameterMapperNode::setKnobValue (int index, float v)
{
    if (index < 0 || index >= kNumSlots)
        return;
    v = juce::jlimit (0.0f, 1.0f, v);
    slots[index].value = v;

    if (auto* param = resolveParameter (slots[index].targetNodeId, slots[index].targetParamIndex))
        param->setValueNotifyingHost (v);

    // Echo the new value to the Twister's LED ring. CC numbering differs
    // by firmware mode -- default uses absolute 0..63, native uses 0..15
    // and only addresses the currently-visible bank.
    const int midiValue = juce::jlimit (0, 127, (int) std::round (v * 127.0f));
    if (nativeMode.load (std::memory_order_acquire))
    {
        const int bank = currentBank.load (std::memory_order_acquire);
        const int firstSlot = bank * kKnobsPerBank;
        if (index >= firstSlot && index < firstSlot + kKnobsPerBank)
            queueMidiOut (juce::MidiMessage::controllerEvent (
                kEncoderChannel, index - firstSlot, midiValue));
    }
    else
    {
        queueMidiOut (juce::MidiMessage::controllerEvent (
            kEncoderChannel, index, midiValue));
    }
}

void ParameterMapperNode::setSlotLabel (int index, const juce::String& newLabel)
{
    if (index < 0 || index >= kNumSlots)
        return;
    if (slots[index].label == newLabel)
        return;
    slots[index].label = newLabel;
    notifyListeners();
}

void ParameterMapperNode::unmap (int index)
{
    if (index < 0 || index >= kNumSlots)
        return;
    slots[index].targetNodeId = 0;
    slots[index].targetParamIndex = -1;
    notifyListeners();
}

//==============================================================================
std::vector<ParameterMapperNode::AutoPoint> ParameterMapperNode::getAutomation (int slot) const
{
    if (slot < 0 || slot >= kNumSlots)
        return {};
    const juce::ScopedLock sl (automationLock);
    return automation[slot];
}

bool ParameterMapperNode::slotHasAutomation (int slot) const
{
    if (slot < 0 || slot >= kNumSlots)
        return false;
    const juce::ScopedLock sl (automationLock);
    return ! automation[slot].empty();
}

int ParameterMapperNode::addAutomationPoint (int slot, double time, float value)
{
    if (slot < 0 || slot >= kNumSlots)
        return -1;
    time  = juce::jlimit (0.0, kAutomationLengthSeconds, time);
    value = juce::jlimit (0.0f, 1.0f, value);

    const juce::ScopedLock sl (automationLock);
    auto& pts = automation[slot];
    auto it = std::lower_bound (pts.begin(), pts.end(), time,
                                [] (const AutoPoint& p, double t) { return p.time < t; });
    const auto inserted = pts.insert (it, AutoPoint { time, value, 0.0f });
    return (int) (inserted - pts.begin());
}

void ParameterMapperNode::moveAutomationPoint (int slot, int index, double time, float value)
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    const juce::ScopedLock sl (automationLock);
    auto& pts = automation[slot];
    if (index < 0 || index >= (int) pts.size())
        return;

    // Clamp between neighbours so the point can't reorder (keeps its index
    // stable during a drag).
    const double lo = (index > 0) ? pts[(size_t) index - 1].time + 1.0e-3 : 0.0;
    const double hi = (index < (int) pts.size() - 1) ? pts[(size_t) index + 1].time - 1.0e-3
                                                      : kAutomationLengthSeconds;
    pts[(size_t) index].time  = juce::jlimit (lo, juce::jmax (lo, hi), time);
    pts[(size_t) index].value = juce::jlimit (0.0f, 1.0f, value);
}

void ParameterMapperNode::setAutomationCurve (int slot, int index, float curve)
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    const juce::ScopedLock sl (automationLock);
    auto& pts = automation[slot];
    if (index < 0 || index >= (int) pts.size())
        return;
    pts[(size_t) index].curve = juce::jlimit (-1.0f, 1.0f, curve);
}

void ParameterMapperNode::removeAutomationPoint (int slot, int index)
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    const juce::ScopedLock sl (automationLock);
    auto& pts = automation[slot];
    if (index < 0 || index >= (int) pts.size())
        return;
    pts.erase (pts.begin() + index);
}

void ParameterMapperNode::clearAutomation (int slot)
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    const juce::ScopedLock sl (automationLock);
    automation[slot].clear();
}

//==============================================================================
void ParameterMapperNode::setCurrentBank (int newBank, bool sendToHardware)
{
    newBank = juce::jlimit (0, kNumBanks - 1, newBank);
    if (newBank == currentBank.load (std::memory_order_acquire))
        return;

    currentBank.store (newBank, std::memory_order_release);

    if (sendToHardware)
    {
        // Default firmware needs a bank-switch notification; in native mode
        // the host owns the bank concept (the Twister doesn't track it)
        // so we just resend the new bank's LED positions.
        if (! nativeMode.load (std::memory_order_acquire))
            queueMidiOut (juce::MidiMessage::noteOn (kBankChannel, newBank, (juce::uint8) 127));

        pushBankToHardware (newBank);
        updateRingGuide();
    }

    notifyListeners();
}

void ParameterMapperNode::pushBankToHardware (int bank)
{
    bank = juce::jlimit (0, kNumBanks - 1, bank);
    const int firstSlot = bank * kKnobsPerBank;
    const bool nm = nativeMode.load (std::memory_order_acquire);

    for (int i = 0; i < kKnobsPerBank; ++i)
    {
        const int v = juce::jlimit (0, 127, (int) std::round (slots[firstSlot + i].value * 127.0f));
        // Default firmware uses CC = absolute slot 0..63 across banks.
        // Native firmware only knows the 16 visible encoders, so CC = 0..15.
        const int cc = nm ? i : (firstSlot + i);
        queueMidiOut (juce::MidiMessage::controllerEvent (kEncoderChannel, cc, v));
    }
}

void ParameterMapperNode::pushAllToHardware()
{
    for (int b = 0; b < kNumBanks; ++b)
        pushBankToHardware (b);
}

//==============================================================================
void ParameterMapperNode::recordSnapshot (int idx)
{
    if (idx < 0 || idx >= kNumSnapshots)
        return;
    auto& s = snapshots[idx];
    s.hasData = true;
    for (int i = 0; i < kNumSlots; ++i)
        s.values[i] = slots[i].value;
    notifyListeners();
}

void ParameterMapperNode::clearSnapshot (int idx)
{
    if (idx < 0 || idx >= kNumSnapshots)
        return;
    snapshots[idx].hasData = false;
    for (auto& v : snapshots[idx].values)
        v = 0.0f;
    // If the cleared snapshot was being previewed, exit preview.
    if (previewSnapshot.load (std::memory_order_acquire) == idx)
        previewSnapshot.store (-1, std::memory_order_release);
    notifyListeners();
}

ParameterMapperNode::Snapshot ParameterMapperNode::getSnapshot (int idx) const
{
    if (idx < 0 || idx >= kNumSnapshots)
        return {};
    return snapshots[idx];
}

bool ParameterMapperNode::snapshotHasData (int idx) const
{
    if (idx < 0 || idx >= kNumSnapshots)
        return false;
    return snapshots[idx].hasData;
}

float ParameterMapperNode::getSnapshotValue (int snapshotIdx, int slotIdx) const
{
    if (snapshotIdx < 0 || snapshotIdx >= kNumSnapshots
        || slotIdx < 0 || slotIdx >= kNumSlots
        || ! snapshots[snapshotIdx].hasData)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return snapshots[snapshotIdx].values[slotIdx];
}

void ParameterMapperNode::setPreviewSnapshot (int idx)
{
    if (idx < -1 || idx >= kNumSnapshots)
        return;
    if (idx >= 0 && ! snapshots[idx].hasData)
        idx = -1;

    if (idx == previewSnapshot.load (std::memory_order_acquire))
        return;
    previewSnapshot.store (idx, std::memory_order_release);
    updateRingGuide();
    notifyListeners();
}

void ParameterMapperNode::setPreviewModeEnabled (bool enabled)
{
    if (enabled == previewModeEnabled.load (std::memory_order_acquire))
        return;
    previewModeEnabled.store (enabled, std::memory_order_release);
    if (! enabled)
        previewSnapshot.store (-1, std::memory_order_release); // also exit any active preview
    notifyListeners();
}

void ParameterMapperNode::armSnapshot (int idx)
{
    if (idx < -1 || idx >= kNumSnapshots)
        return;
    if (idx >= 0 && ! snapshots[idx].hasData)
        return;

    armedSnapshot.store (idx, std::memory_order_release);
    updateRingGuide();
    notifyListeners();
}

void ParameterMapperNode::applySnapshot (int idx)
{
    if (idx < 0 || idx >= kNumSnapshots || ! snapshots[idx].hasData)
        return;

    const auto& snap = snapshots[idx];
    for (int i = 0; i < kNumSlots; ++i)
    {
        slots[i].value = snap.values[i];
        if (auto* p = resolveParameter (slots[i].targetNodeId, slots[i].targetParamIndex))
            p->setValueNotifyingHost (snap.values[i]);
    }
    // Mirror the new values back to the Twister LED rings, and stop the
    // ring-guide oscillation: the armed transition is done.
    pushAllToHardware();
    // Note: timer is on the message thread so we can't simply call
    // stopTimer() from the audio thread. updateRingGuide() is a
    // message-thread call; we schedule it from render() via callAsync.
}

//==============================================================================
void ParameterMapperNode::beginLearn (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kNumSlots)
        return;

    detachLearnListeners();
    learningSlot.store (slotIndex, std::memory_order_release);
    attachLearnListeners();
    notifyListeners();
}

void ParameterMapperNode::cancelLearn()
{
    if (! isLearning())
        return;
    detachLearnListeners();
    learningSlot.store (-1, std::memory_order_release);
    notifyListeners();
}

void ParameterMapperNode::attachLearnListeners()
{
    auto* graph = getParentGraph();
    if (graph == nullptr)
        return;

    for (int i = 0; i < graph->getNumNodes(); ++i)
    {
        auto* proc = graph->getNode (i);
        if (proc == nullptr || proc == this)
            continue;
        auto* ap = proc->getAudioProcessor();
        if (ap == nullptr)
            continue;

        const auto& params = ap->getParameters();
        for (int p = 0; p < params.size(); ++p)
            watches.add (new ParamWatch (*this, proc->nodeId, p, params[p]));
    }
}

void ParameterMapperNode::detachLearnListeners()
{
    watches.clear (true);
}

juce::AudioProcessorParameter* ParameterMapperNode::resolveParameter (juce::uint32 nodeId, int paramIndex) const
{
    if (nodeId == 0 || paramIndex < 0)
        return nullptr;
    auto* graph = const_cast<ParameterMapperNode*> (this)->getParentGraph();
    if (graph == nullptr)
        return nullptr;
    auto* proc = graph->getNodeForId (nodeId);
    if (proc == nullptr)
        return nullptr;
    auto* ap = proc->getAudioProcessor();
    if (ap == nullptr)
        return nullptr;
    const auto& params = ap->getParameters();
    return paramIndex < params.size() ? params[paramIndex] : nullptr;
}

void ParameterMapperNode::handleCapturedChange (juce::uint32 nodeId, int paramIndex)
{
    const int slot = learningSlot.load (std::memory_order_acquire);
    if (slot < 0)
        return;

    detachLearnListeners();
    learningSlot.store (-1, std::memory_order_release);

    slots[slot].targetNodeId = nodeId;
    slots[slot].targetParamIndex = paramIndex;

    if (auto* param = resolveParameter (nodeId, paramIndex))
        slots[slot].value = juce::jlimit (0.0f, 1.0f, param->getValue());

    notifyListeners();
}

//==============================================================================
juce::String ParameterMapperNode::getMappedParamName (int index) const
{
    if (index < 0 || index >= kNumSlots)
        return {};
    if (auto* p = resolveParameter (slots[index].targetNodeId, slots[index].targetParamIndex))
        return p->getName (32);
    return {};
}

juce::String ParameterMapperNode::getDisplayLabel (int index) const
{
    if (index < 0 || index >= kNumSlots)
        return "—";
    if (slots[index].label.isNotEmpty())
        return slots[index].label;
    const auto pn = getMappedParamName (index);
    if (pn.isNotEmpty())
        return pn;
    return "—";
}

//==============================================================================
void ParameterMapperNode::queueMidiOut (const juce::MidiMessage& msg)
{
    const juce::ScopedLock sl (outQueueLock);
    outQueue.addEvent (msg, 0);
}

void ParameterMapperNode::updateRingGuide()
{
    const int preview = previewSnapshot.load (std::memory_order_acquire);
    const int armed   = armedSnapshot.load (std::memory_order_acquire);
    const int active  = (preview >= 0) ? preview : armed;

    if (nativeMode.load (std::memory_order_acquire))
    {
        // Native firmware: run the position-oscillation timer that shows
        // the path on the LED ring. Safe here because the encoder's value
        // is host-tracked (relative input) -- the LED is purely a display.
        if (active >= 0 && snapshots[active].hasData)
        {
            if (! isTimerRunning())
                startTimer (kNativeOscillationMs);
        }
        else
        {
            if (isTimerRunning())
                stopTimer();
            // Resync the rings to the actual current bank values when the
            // guide is cleared.
            const int b = currentBank.load (std::memory_order_acquire);
            const int firstSlot = b * kKnobsPerBank;
            for (int i = 0; i < kKnobsPerBank; ++i)
            {
                const int v = juce::jlimit (0, 127,
                    (int) std::round (slots[firstSlot + i].value * 127.0f));
                queueMidiOut (juce::MidiMessage::controllerEvent (
                    kEncoderChannel, i, v));
            }
        }
        return;
    }

    // Default-mode firmware: ring brightness animation only.
    if (isTimerRunning())
        stopTimer();

    const int firstSlot = currentBank.load (std::memory_order_acquire) * kKnobsPerBank;
    int animValue = kAnimOff;
    if      (preview >= 0) animValue = kAnimPreviewPulse;
    else if (armed   >= 0) animValue = kAnimArmedGate;

    for (int i = 0; i < kKnobsPerBank; ++i)
        queueMidiOut (juce::MidiMessage::controllerEvent (
            kRingAnimChannel, firstSlot + i, animValue));
}

void ParameterMapperNode::timerCallback()
{
    if (! nativeMode.load (std::memory_order_acquire))
    {
        stopTimer();
        return;
    }

    const int preview = previewSnapshot.load (std::memory_order_acquire);
    const int armed   = armedSnapshot.load (std::memory_order_acquire);
    const int active  = (preview >= 0) ? preview : armed;
    if (active < 0 || ! snapshots[active].hasData)
    {
        stopTimer();
        return;
    }

    ringTargetTick = ! ringTargetTick;

    // Honour the post-interaction grace period: if the user moved an
    // encoder very recently, skip this tick entirely. They get to see the
    // raw position they just dialled in via the echo we forward via the
    // graph (see render's per-tick echo).
    const auto now = juce::Time::getMillisecondCounter();
    const auto lastTick = lastUserTickMs.load (std::memory_order_acquire);
    if (lastTick != 0 && (now - lastTick) < kUserInteractionGraceMs)
        return;

    const int firstSlot = currentBank.load (std::memory_order_acquire) * kKnobsPerBank;
    const auto& snap = snapshots[active];

    // Tolerance below which we consider current == target and stop sending
    // oscillation messages for that slot. Saves USB pipe and stops the
    // ring from flickering when the user has reached the target.
    constexpr float kMatchTol = 0.01f;

    for (int i = 0; i < kKnobsPerBank; ++i)
    {
        const int slotIdx = firstSlot + i;
        const float cur = slots[slotIdx].value;
        const float tgt = snap.values[slotIdx];
        if (std::abs (cur - tgt) < kMatchTol)
            continue;

        const float v = ringTargetTick ? tgt : cur;
        const int midiV = juce::jlimit (0, 127, (int) std::round (v * 127.0f));
        // CC index 0..15 addresses the 16 currently-visible LED rings.
        queueMidiOut (juce::MidiMessage::controllerEvent (kEncoderChannel,
                                                            i,
                                                            midiV));
    }
}

void ParameterMapperNode::setNativeMode (bool enabled)
{
    if (enabled == nativeMode.load (std::memory_order_acquire))
        return;
    nativeMode.store (enabled, std::memory_order_release);

    // Send the protocol activation/deactivation SysEx. From the Trinitou
    // native-mode spec:
    //   Activate:   F0 000179 05 00 01 F7
    //   Deactivate: F0 000179 05 00 00 F7
    // The body between F0 and F7 is: manufacturer ID (00 01 79) + native
    // marker (05) + commandId (00) + content (00=inactive, 01=active).
    const juce::uint8 body[] = {
        0x00, 0x01, 0x79, // manufacturer ID
        0x05,             // native mode prefix
        0x00,             // commandId = set native mode active
        enabled ? (juce::uint8) 0x01 : (juce::uint8) 0x00
    };
    queueMidiOut (juce::MidiMessage::createSysExMessage (body, (int) sizeof (body)));

    updateRingGuide();
    notifyListeners();
}


void ParameterMapperNode::render (RenderContext& rc)
{
    if (rc.midi.getNumBuffers() <= 0)
        return;

    // MidiPipe returns the same physical buffer for read & write at a given
    // index (in/out share when a port's in/out channels match). Use the
    // writable accessor so we can clear it before adding the outgoing
    // queue's contents.
    auto* buf = rc.midi.getWriteBuffer (0);
    const int numSamples = rc.audio.getNumSamples();

    bool anySlotChanged = false;
    bool bankChanged = false;
    bool snapshotApplied = false;

    // --- Bar-edge sync for armed snapshots. ------------------------------
    // When a snapshot is armed (user clicked a snapshot button outside of
    // preview mode), wait until the transport crosses the next bar
    // boundary before swapping the parameter values in. This gives a
    // musically-aligned switch instead of an arbitrary mid-bar jolt.
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            const bool playing = pos->getIsPlaying();
            const double ppq = pos->getPpqPosition().orFallback (0.0);
            double beatsPerBar = 4.0;
            if (auto ts = pos->getTimeSignature())
                beatsPerBar = (double) ts->numerator;
            const int currentBar = (int) std::floor (ppq / juce::jmax (beatsPerBar, 1.0));

            if (playing)
            {
                if (lastBarSeen < 0)
                {
                    lastBarSeen = currentBar;
                }
                else if (currentBar > lastBarSeen)
                {
                    const int armed = armedSnapshot.exchange (-1, std::memory_order_acq_rel);
                    if (armed >= 0)
                    {
                        applySnapshot (armed);
                        snapshotApplied = true;
                    }
                    lastBarSeen = currentBar;
                }
            }
            else
            {
                // Transport stopped: reset the seen-bar so the next play
                // session doesn't fire spuriously on rewind.
                lastBarSeen = -1;
            }

            // --- Automation playback. ------------------------------------
            // Evaluate every slot that has automation at the current transport
            // time and push the value to its mapped parameter. Runs whether
            // playing or stopped so scrubbing the playhead previews the curve.
            // try-lock only: if the UI is mid-edit we simply skip this block.
            const double timeSec = pos->getTimeInSeconds().orFallback (-1.0);
            if (timeSec >= 0.0)
            {
                const juce::ScopedTryLock stl (automationLock);
                if (stl.isLocked())
                {
                    for (int i = 0; i < kNumSlots; ++i)
                    {
                        if (automation[i].empty())
                        {
                            lastAutoValue[i] = std::numeric_limits<float>::quiet_NaN();
                            continue;
                        }
                        const float v = automationValueAt (automation[i], timeSec);
                        if (! std::isnan (lastAutoValue[i]) && std::abs (v - lastAutoValue[i]) < 1.0e-4f)
                            continue;
                        lastAutoValue[i] = v;
                        slots[i].value = v;
                        if (auto* p = resolveParameter (slots[i].targetNodeId, slots[i].targetParamIndex))
                            p->setValueNotifyingHost (v);
                    }
                }
            }
        }
    }

    // --- Incoming MIDI from the Twister. ----------------------------------
    if (buf != nullptr && ! buf->isEmpty())
    {
        // Take a copy so writes back to the buffer don't race the iterator.
        juce::MidiBuffer incoming;
        incoming.addEvents (*buf, 0, numSamples, 0);
        // Clear the upstream MIDI; we explicitly choose what to forward via
        // outQueue (typically just the echo CCs we want to feed the Twister).
        buf->clear();

        const bool nm = nativeMode.load (std::memory_order_acquire);

        for (const auto m : incoming)
        {
            const auto msg = m.getMessage();
            const int ch = msg.getChannel();

            if (msg.isController() && ch == kEncoderChannel)
            {
                const int cc = msg.getControllerNumber();
                if (nm)
                {
                    // Native mode (active via SysEx F0 000179 05 00 01 F7):
                    // encoders ALWAYS send relative Binary Offset ticks
                    // (63 = -1, 65 = +1) per Trinitou's native mode spec,
                    // and host LED writes no longer touch encoder values.
                    if (cc < 0 || cc >= kKnobsPerBank)
                        continue;
                    const int rel = msg.getControllerValue();
                    if (rel != 63 && rel != 65)
                        continue;
                    const float delta = (rel == 65) ? +kNativeRelativeStep
                                                     : -kNativeRelativeStep;
                    const int slotIdx = currentBank.load (std::memory_order_acquire) * kKnobsPerBank + cc;
                    const float nv = juce::jlimit (0.0f, 1.0f,
                                                     slots[slotIdx].value + delta);
                    if (nv == slots[slotIdx].value)
                        continue;
                    slots[slotIdx].value = nv;
                    anySlotChanged = true;
                    // Record the interaction so the LED guide timer can
                    // back off and let the user see their own movement
                    // on the ring instead of fighting it.
                    lastUserTickMs.store (juce::Time::getMillisecondCounter(),
                                           std::memory_order_release);
                    if (auto* p = resolveParameter (slots[slotIdx].targetNodeId,
                                                      slots[slotIdx].targetParamIndex))
                        p->setValueNotifyingHost (nv);

                    // Always echo the new absolute position back to the
                    // LED ring in native mode -- the encoder value is
                    // host-tracked so this only moves the LED, not the
                    // logical value. The oscillation timer respects a
                    // post-interaction grace period (see timerCallback)
                    // so the user's own movement isn't immediately
                    // overwritten by the next oscillation tick.
                    queueMidiOut (juce::MidiMessage::controllerEvent (
                        kEncoderChannel, cc,
                        juce::jlimit (0, 127, (int) std::round (nv * 127.0f))));
                }
                else if (cc >= 0 && cc < kNumSlots)
                {
                    // Default firmware: absolute CC 0..63 across the 4 banks.
                    const float v = (float) msg.getControllerValue() / 127.0f;
                    slots[cc].value = v;
                    anySlotChanged = true;
                    if (auto* p = resolveParameter (slots[cc].targetNodeId, slots[cc].targetParamIndex))
                        p->setValueNotifyingHost (v);
                }
            }
            else if (nm && msg.isController() && ch == kNativeSideButtonChannel)
            {
                // Native firmware: side buttons send CC 0..5 ch.3. We use
                // LH middle (1) for previous bank and RH middle (4) for
                // next bank. Only react to "pressed" (value 127).
                if (msg.getControllerValue() < 64)
                    continue;
                const int cc = msg.getControllerNumber();
                int newBank = currentBank.load (std::memory_order_acquire);
                if (cc == kNativeSideBtnPrev) newBank = juce::jmax (0, newBank - 1);
                else if (cc == kNativeSideBtnNext) newBank = juce::jmin (kNumBanks - 1, newBank + 1);
                else continue;
                if (newBank != currentBank.load (std::memory_order_acquire))
                {
                    currentBank.store (newBank, std::memory_order_release);
                    bankChanged = true;
                }
            }
            else if (! nm && msg.isNoteOn() && ch == kBankChannel)
            {
                // Default firmware bank change: NoteOn ch.4, note 0..3.
                const int note = msg.getNoteNumber();
                if (note >= 0 && note < kNumBanks)
                {
                    currentBank.store (note, std::memory_order_release);
                    bankChanged = true;
                }
            }
        }
    }

    // --- Outgoing MIDI queued by UI / setters. ---------------------------
    if (buf != nullptr)
    {
        juce::MidiBuffer drained;
        {
            const juce::ScopedLock sl (outQueueLock);
            drained.swapWith (outQueue);
        }
        if (! drained.isEmpty())
            buf->addEvents (drained, 0, numSamples, 0);
    }

    // --- Notify the UI on the message thread if state changed. -----------
    if (anySlotChanged || bankChanged || snapshotApplied)
    {
        const bool needGuideRefresh = bankChanged || snapshotApplied;
        const juce::WeakReference<ParameterMapperNode> weak { this };
        juce::MessageManager::callAsync ([weak, needGuideRefresh]() {
            if (auto* m = weak.get())
            {
                if (needGuideRefresh)
                    m->updateRingGuide();
                m->notifyListeners();
            }
        });
    }
}

//==============================================================================
void ParameterMapperNode::getState (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("ParamMapper");
    root.setProperty ("bank", currentBank.load (std::memory_order_acquire), nullptr);
    root.setProperty ("nativeMode", nativeMode.load (std::memory_order_acquire), nullptr);
    for (int i = 0; i < kNumSlots; ++i)
    {
        juce::ValueTree v ("slot");
        v.setProperty ("i", i, nullptr);
        v.setProperty ("nodeId", (int) slots[i].targetNodeId, nullptr);
        v.setProperty ("paramIndex", slots[i].targetParamIndex, nullptr);
        v.setProperty ("label", slots[i].label, nullptr);
        v.setProperty ("value", (double) slots[i].value, nullptr);
        root.appendChild (v, nullptr);
    }
    for (int i = 0; i < kNumSnapshots; ++i)
    {
        if (! snapshots[i].hasData)
            continue;
        juce::ValueTree s ("snapshot");
        s.setProperty ("i", i, nullptr);
        // Pack the 64 floats into a single CSV-ish property so we don't add
        // 64 child elements per snapshot.
        juce::String packed;
        for (int k = 0; k < kNumSlots; ++k)
        {
            if (k > 0) packed << ' ';
            packed << juce::String (snapshots[i].values[k], 4);
        }
        s.setProperty ("v", packed, nullptr);
        root.appendChild (s, nullptr);
    }
    {
        const juce::ScopedLock sl (automationLock);
        for (int i = 0; i < kNumSlots; ++i)
        {
            if (automation[i].empty())
                continue;
            juce::ValueTree a ("auto");
            a.setProperty ("i", i, nullptr);
            // Pack points as "time,value,curve" triples separated by spaces.
            juce::String packed;
            for (size_t k = 0; k < automation[i].size(); ++k)
            {
                const auto& p = automation[i][k];
                if (k > 0) packed << ' ';
                packed << juce::String (p.time, 6) << ','
                       << juce::String (p.value, 6) << ','
                       << juce::String (p.curve, 4);
            }
            a.setProperty ("pts", packed, nullptr);
            root.appendChild (a, nullptr);
        }
    }
    juce::MemoryOutputStream mos (dest, false);
    root.writeToStream (mos);
}

void ParameterMapperNode::setState (const void* data, int sizeInBytes)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! root.isValid() || ! root.hasType ("ParamMapper"))
        return;

    currentBank.store (juce::jlimit (0, kNumBanks - 1,
                                     (int) root.getProperty ("bank", 0)),
                       std::memory_order_release);
    const bool savedNative = (bool) root.getProperty ("nativeMode", false);
    nativeMode.store (savedNative, std::memory_order_release);
    if (savedNative)
    {
        // Re-send activation SysEx: native mode is not persistent on the
        // device, so a restored session has to re-arm it.
        const juce::uint8 body[] = { 0x00, 0x01, 0x79, 0x05, 0x00, 0x01 };
        queueMidiOut (juce::MidiMessage::createSysExMessage (body, (int) sizeof (body)));
    }

    // Clear before loading so missing children leave defaults.
    for (auto& s : snapshots) { s.hasData = false; for (auto& v : s.values) v = 0.0f; }
    {
        const juce::ScopedLock sl (automationLock);
        for (auto& a : automation) a.clear();
    }

    for (int i = 0; i < root.getNumChildren(); ++i)
    {
        auto v = root.getChild (i);
        if (v.hasType ("slot"))
        {
            const int idx = v.getProperty ("i", -1);
            if (idx < 0 || idx >= kNumSlots)
                continue;
            slots[idx].targetNodeId     = (juce::uint32) (int) v.getProperty ("nodeId", 0);
            slots[idx].targetParamIndex = (int) v.getProperty ("paramIndex", -1);
            slots[idx].label            = v.getProperty ("label", juce::String()).toString();
            slots[idx].value            = (float) (double) v.getProperty ("value", 0.0);
        }
        else if (v.hasType ("snapshot"))
        {
            const int idx = v.getProperty ("i", -1);
            if (idx < 0 || idx >= kNumSnapshots)
                continue;
            const juce::String packed = v.getProperty ("v", juce::String()).toString();
            juce::StringArray parts;
            parts.addTokens (packed, " ", "");
            parts.removeEmptyStrings();
            const int n = juce::jmin (parts.size(), kNumSlots);
            for (int k = 0; k < n; ++k)
                snapshots[idx].values[k] = (float) parts[k].getDoubleValue();
            snapshots[idx].hasData = true;
        }
        else if (v.hasType ("auto"))
        {
            const int idx = v.getProperty ("i", -1);
            if (idx < 0 || idx >= kNumSlots)
                continue;
            const juce::String packed = v.getProperty ("pts", juce::String()).toString();
            juce::StringArray triples;
            triples.addTokens (packed, " ", "");
            triples.removeEmptyStrings();
            std::vector<AutoPoint> pts;
            pts.reserve ((size_t) triples.size());
            for (const auto& tri : triples)
            {
                juce::StringArray f;
                f.addTokens (tri, ",", "");
                if (f.size() < 2)
                    continue;
                AutoPoint p;
                p.time  = f[0].getDoubleValue();
                p.value = juce::jlimit (0.0f, 1.0f, (float) f[1].getDoubleValue());
                p.curve = (f.size() >= 3) ? juce::jlimit (-1.0f, 1.0f, (float) f[2].getDoubleValue()) : 0.0f;
                pts.push_back (p);
            }
            std::sort (pts.begin(), pts.end(),
                       [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
            const juce::ScopedLock sl (automationLock);
            automation[idx] = std::move (pts);
        }
    }

    // After restoring state, push everything to the Twister so the hardware
    // matches what was just loaded.
    pushAllToHardware();
    notifyListeners();
}

//==============================================================================
void ParameterMapperNode::getPluginDescription (juce::PluginDescription& desc) const
{
    desc.fileOrIdentifier = EL_NODE_ID_PARAM_MAPPER;
    desc.uniqueId = EL_NODE_UID_PARAM_MAPPER;
    desc.name = "Parameter Mapper";
    desc.descriptiveName = "64-knob parameter mapper (4 banks, Midi Fighter Twister)";
    desc.numInputChannels = 0;
    desc.numOutputChannels = 0;
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.version = "1.1.0";
}

void ParameterMapperNode::refreshPorts()
{
    if (getNumPorts() > 0)
        return;
    PortList ports;
    ports.add (PortType::Midi, 0, 0, "midi_in",  "MIDI In",  true);
    ports.add (PortType::Midi, 1, 0, "midi_out", "MIDI Out", false);
    setPorts (ports);
}

void ParameterMapperNode::notifyListeners()
{
    listeners.call ([] (juce::ChangeListener& l) { l.changeListenerCallback (nullptr); });
}

} // namespace element
