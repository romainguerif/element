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
    };
    std::vector<PendingFeedback> pendingFeedbacks;

    int setupFeedbackInput (uint32 srcNode, uint32 srcPort, Array<void*>& renderingOps);
    void resolvePendingFeedbacksForNode (Processor* node, Array<void*>& renderingOps);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphBuilder)
};

} // namespace element
