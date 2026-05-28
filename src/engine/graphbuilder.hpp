// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "ElementApp.h"

namespace element {

class GraphNode;
class Processor;

// Shared storage for one-block feedback loops. The producer writes its
// freshly-rendered block into `data` after it processes; the consumer
// reads from `data` *before* the producer runs in the next block, so the
// loop is broken with one block of latency (~10 ms at 48 kHz / 512 samples).
class FeedbackBlockStorage
{
public:
    FeedbackBlockStorage()
    {
        data.calloc ((size_t) maxSamples);
    }

    static constexpr int maxSamples = 8192;
    juce::HeapBlock<float> data;
};

class GraphOp
{
public:
    GraphOp() {}
    virtual ~GraphOp() {}

    virtual void perform (AudioSampleBuffer& sharedBufferChans,
                          const OwnedArray<MidiBuffer>& sharedMidiBuffers,
                          const int numSamples) = 0;

private:
    JUCE_LEAK_DETECTOR (GraphOp)
};

// Adjustable-amount delay used to compensate for feedback-edge latency.
// We pre-allocate a generous buffer and lock in the actual delay value
// after all nodes have been scheduled (once the feedback source's true
// latency is known). This lets us insert the op at the correct position
// during pass 1 even though the delay amount is computed in pass 2.
class DeferredDelayOp : public GraphOp
{
public:
    static constexpr int maxDelaySamples = 16384; // ~341 ms at 48 kHz

    explicit DeferredDelayOp (int channel_)
        : channel (channel_), bufferSize (maxDelaySamples + 1)
    {
        buffer.calloc ((size_t) bufferSize);
    }

    void setDelay (int delaySamples) noexcept
    {
        delaySamples = juce::jlimit (0, bufferSize - 1, delaySamples);
        delay = delaySamples;
        for (int i = 0; i < bufferSize; ++i)
            buffer[i] = 0.0f;
        readIndex = 0;
        writeIndex = delaySamples;
    }

    void perform (juce::AudioSampleBuffer& sharedBufferChans,
                  const juce::OwnedArray<juce::MidiBuffer>&,
                  const int numSamples) override
    {
        if (delay <= 0)
            return;

        float* data = sharedBufferChans.getWritePointer (channel, 0);
        for (int i = numSamples; --i >= 0;)
        {
            buffer[writeIndex] = *data;
            *data++ = buffer[readIndex];
            if (++readIndex >= bufferSize) readIndex = 0;
            if (++writeIndex >= bufferSize) writeIndex = 0;
        }
    }

private:
    juce::HeapBlock<float> buffer;
    const int channel;
    const int bufferSize;
    int readIndex = 0, writeIndex = 0;
    int delay = 0;

    JUCE_DECLARE_NON_COPYABLE (DeferredDelayOp)
};

/** Used to calculate the correct sequence of rendering ops needed, based on
    the best re-use of shared buffers at each stage. */
class GraphBuilder
{
public:
    GraphBuilder (GraphNode& graph_,
                  const Array<void*>& orderedNodes_,
                  Array<void*>& renderingOps);

    int buffersNeeded (PortType type);
    int getTotalLatencySamples() const { return totalLatency; }

private:
    //==============================================================================
    GraphNode& graph;
    const Array<void*>& orderedNodes;
    Array<uint32> allNodes[PortType::Unknown];
    Array<uint32> allPorts[PortType::Unknown];

    enum
    {
        freeNodeID = 0xffffffff,
        zeroNodeID = 0xfffffffe,
        anonymousNodeID = 0xfffffffd
    };

    static bool isNodeBusy (uint32 nodeID) noexcept { return nodeID != freeNodeID && nodeID != zeroNodeID; }

    // Latency at each node's output, keyed by node id. Mirrors the
    // std::unordered_map<uint32, int> delays used by JUCE 8's
    // AudioProcessorGraph::RenderSequenceBuilder.
    std::unordered_map<uint32, int> nodeDelays;
    int totalLatency;

    int getNodeDelay (uint32 nodeID) const noexcept;
    void setNodeDelay (uint32 nodeID, int latency);

    int getInputLatency (uint32 nodeID) const;

    void createRenderingOpsForNode (Processor* const node, Array<void*>& renderingOps, const int ourRenderingIndex);

    int getFreeBuffer (PortType type);
    int getReadOnlyEmptyBuffer() const noexcept;
    int getBufferContaining (const PortType type, const uint32 nodeId, const uint32 outputPort) noexcept;
    void markUnusedBuffersFree (const int stepIndex);
    bool isBufferNeededLater (int stepIndexToSearchFrom, uint32 inputChannelOfIndexToIgnore, const uint32 sourceNode, const uint32 outputPortIndex) const;
    void markBufferAsContaining (int bufferNum, PortType type, uint32 nodeId, uint32 portIndex);

    // Feedback-cycle support: when a node consumes a source that hasn't
    // been scheduled yet (a cycle), allocate a buffer that will be filled
    // by the previous block's source output (one-block delay).
    struct PendingFeedback
    {
        uint32 srcNode;
        uint32 srcPort;
        std::shared_ptr<FeedbackBlockStorage> storage;
        uint32 consumerNode; // who set up the feedback (for PDC)
    };
    std::vector<PendingFeedback> pendingFeedbacks;

    // Per-consumer PDC compensation state: tracks the dry-input delay ops
    // that align non-feedback inputs with the feedback path (which arrives
    // 1 block + source.latency late).
    struct ConsumerPDCState
    {
        int maxLatencyAtBuild = 0;
        int additionalDelay = 0;
        std::vector<DeferredDelayOp*> dryDelayOps;
    };
    std::unordered_map<uint32, ConsumerPDCState> consumerPDC;
    int blockSize = 0;

    int setupFeedbackInput (uint32 srcNode, uint32 srcPort, uint32 consumerNode, Array<void*>& renderingOps);
    void resolvePendingFeedbacksForNode (Processor* node, Array<void*>& renderingOps);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphBuilder)
};

} // namespace element
