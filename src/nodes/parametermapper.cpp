// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/parametermapper.hpp"

#include <element/midipipe.hpp>
#include <element/portcount.hpp>
#include "engine/graphnode.hpp"

namespace element {

namespace {
// DJ TechTools Midi Fighter Twister default MIDI map.
//
//   Encoders & LED rings:  channel 1, CC 0..63   (16 per bank, 4 banks)
//   Bank select:           channel 4, NoteOn  0..3 selects bank 1..4
//                          channel 4, NoteOff is sent for the previous bank;
//                          we ignore it because NoteOn carries the new bank.
//
// JUCE's MidiMessage uses 1-based channels for the constructors but
// `getChannel()` also returns 1-based. We use 1-based throughout.
constexpr int kEncoderChannel = 1;
constexpr int kBankChannel    = 4;
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

void ParameterMapperNode::setKnobValue (int index, float v)
{
    if (index < 0 || index >= kNumSlots)
        return;
    v = juce::jlimit (0.0f, 1.0f, v);
    slots[index].value = v;

    if (auto* param = resolveParameter (slots[index].targetNodeId, slots[index].targetParamIndex))
        param->setValueNotifyingHost (v);

    // Echo to the Twister LED ring. Channel 1, CC = absolute slot index,
    // value 0..127. The Twister doesn't echo CCs it receives, so this is
    // a safe one-way update -- no feedback loop.
    const int cc = index;
    const int midiValue = juce::jlimit (0, 127, (int) std::round (v * 127.0f));
    queueMidiOut (juce::MidiMessage::controllerEvent (kEncoderChannel, cc, midiValue));
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
void ParameterMapperNode::setCurrentBank (int newBank, bool sendToHardware)
{
    newBank = juce::jlimit (0, kNumBanks - 1, newBank);
    if (newBank == currentBank.load (std::memory_order_acquire))
        return;

    currentBank.store (newBank, std::memory_order_release);

    if (sendToHardware)
    {
        // Note: the Twister convention is "NoteOn channel 4, note = bank".
        queueMidiOut (juce::MidiMessage::noteOn (kBankChannel, newBank, (juce::uint8) 127));
    }

    notifyListeners();
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

void ParameterMapperNode::render (RenderContext& rc)
{
    auto* inBuf = rc.midi.getReadBuffer (0);
    auto* outBuf = rc.midi.getWriteBuffer (0);
    const int numSamples = rc.audio.getNumSamples();

    // --- Incoming MIDI from the Twister. ----------------------------------
    if (inBuf != nullptr)
    {
        for (const auto m : *inBuf)
        {
            const auto msg = m.getMessage();
            const int ch = msg.getChannel();

            if (msg.isController() && ch == kEncoderChannel)
            {
                const int cc = msg.getControllerNumber();
                if (cc >= 0 && cc < kNumSlots)
                {
                    const float v = (float) msg.getControllerValue() / 127.0f;
                    // Update the slot. We do NOT call setKnobValue here
                    // because that would queue an echo back to the Twister
                    // (its LED ring already moved physically, no need).
                    slots[cc].value = v;
                    if (auto* p = resolveParameter (slots[cc].targetNodeId, slots[cc].targetParamIndex))
                        p->setValueNotifyingHost (v);
                }
            }
            else if (msg.isNoteOn() && ch == kBankChannel)
            {
                const int note = msg.getNoteNumber();
                if (note >= 0 && note < kNumBanks)
                {
                    // Hardware changed bank: update our state but do NOT
                    // echo a NoteOn back -- the Twister already switched.
                    currentBank.store (note, std::memory_order_release);
                    // We notify listeners asynchronously since render() runs
                    // on the audio thread.
                    const juce::WeakReference<ParameterMapperNode> weak { this };
                    juce::MessageManager::callAsync ([weak]() {
                        if (auto* m = weak.get()) m->notifyListeners();
                    });
                }
            }
        }
    }

    // --- Outgoing MIDI queued by UI / setters. ---------------------------
    if (outBuf != nullptr)
    {
        juce::MidiBuffer drained;
        {
            const juce::ScopedLock sl (outQueueLock);
            drained.swapWith (outQueue);
        }
        if (! drained.isEmpty())
            outBuf->addEvents (drained, 0, numSamples, 0);
    }
}

//==============================================================================
void ParameterMapperNode::getState (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("ParamMapper");
    root.setProperty ("bank", currentBank.load (std::memory_order_acquire), nullptr);
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

    for (int i = 0; i < root.getNumChildren(); ++i)
    {
        auto v = root.getChild (i);
        const int idx = v.getProperty ("i", -1);
        if (idx < 0 || idx >= kNumSlots)
            continue;
        slots[idx].targetNodeId    = (juce::uint32) (int) v.getProperty ("nodeId", 0);
        slots[idx].targetParamIndex = (int) v.getProperty ("paramIndex", -1);
        slots[idx].label           = v.getProperty ("label", juce::String()).toString();
        slots[idx].value           = (float) (double) v.getProperty ("value", 0.0);
    }
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
