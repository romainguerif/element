// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

// Plugin Delay Compensation tests.
//
// Element's GraphBuilder is a direct fork of juce::AudioProcessorGraph's
// RenderSequenceBuilder pattern: per-node latency is accumulated along the
// DAG, and DelayChannelOps are inserted on shorter parallel branches to align
// merges. These tests verify both halves of that:
//
//   1. The graph's getLatencySamples() reports the correct total at the
//      audio-output IONode (matches JUCE 8's PDC test in
//      AudioProcessorGraphTests::"rebuilding the graph recalculates overall
//      latency").
//
//   2. Sample-level alignment: feeding an impulse through a parallel
//      arrangement where one branch reports phantom latency causes the
//      compensation pass-through branch to be delayed to match, so both
//      paths sum at the same absolute output sample.
//
//   3. The Element-specific gap that prompted this work: a sub-graph's
//      latency change propagates up to the parent graph.

#include <boost/test/unit_test.hpp>

#include <element/processor.hpp>

#include "engine/graphnode.hpp"
#include "engine/ionode.hpp"
#include "fixture/PreparedGraph.h"
#include "fixture/LatentTestNode.h"

using namespace element;

namespace {

// Connect every output channel of `src` to the matching input channel of `dst`.
void connectAudioChannels (GraphNode& g, Processor& src, Processor& dst, int numChannels)
{
    for (int ch = 0; ch < numChannels; ++ch)
        BOOST_REQUIRE (g.connectChannels (PortType::Audio,
                                          src.nodeId, ch,
                                          dst.nodeId, ch));
}

// Render one block of `numSamples` with `audio` as both input and (after the
// call) output. Mirrors what host code does at the top of the audio callback.
void renderOneBlock (GraphNode& g, juce::AudioSampleBuffer& audio, int numSamples)
{
    juce::MidiBuffer midi;
    RenderContext rc (audio, audio, midi, numSamples);
    g.render (rc);
}

} // namespace

BOOST_AUTO_TEST_SUITE (PDCTests)

// Serial chain: total latency is the sum of node latencies, and the impulse
// fed through the chain appears at the expected absolute sample on the
// graph's audio output.
BOOST_AUTO_TEST_CASE (SerialChainReportsSumLatency)
{
    PreparedGraph fix (44100.0, 512);
    auto& graph = fix.graph;

    constexpr int channels = 2;
    constexpr int latencyA = 32;
    constexpr int latencyB = 17;

    auto* audioIn  = new IONode (IONode::audioInputNode);
    auto* nodeA    = new LatentTestNode (channels, latencyA, /*actuallyDelay=*/true);
    auto* nodeB    = new LatentTestNode (channels, latencyB, true);
    auto* audioOut = new IONode (IONode::audioOutputNode);

    graph.addNode (audioIn);
    graph.addNode (nodeA);
    graph.addNode (nodeB);
    graph.addNode (audioOut);

    connectAudioChannels (graph, *audioIn,  *nodeA,    channels);
    connectAudioChannels (graph, *nodeA,    *nodeB,    channels);
    connectAudioChannels (graph, *nodeB,    *audioOut, channels);

    graph.rebuild();

    BOOST_REQUIRE_EQUAL (graph.getLatencySamples(), latencyA + latencyB);

    juce::AudioSampleBuffer audio (channels, 512);
    audio.clear();
    audio.setSample (0, 0, 1.0f);

    renderOneBlock (graph, audio, 512);

    const int expectedPos = latencyA + latencyB;
    BOOST_CHECK_CLOSE (audio.getSample (0, expectedPos), 1.0f, 1e-3);
    BOOST_CHECK_SMALL (audio.getSample (0, expectedPos - 1), 1e-6f);
    BOOST_CHECK_SMALL (audio.getSample (0, expectedPos + 1), 1e-6f);
}

// Parallel branches: a real-delay node on one branch (consistent with its
// reported latency) forces a DelayChannelOp on the parallel pass-through
// branch at the merge point. We verify by feeding an impulse and checking
// that BOTH paths arrive at the same output sample (so they sum to 2x there,
// and the direct path's impulse does NOT show up earlier).
BOOST_AUTO_TEST_CASE (ParallelBranchesAreAlignedAtMerge)
{
    PreparedGraph fix (44100.0, 512);
    auto& graph = fix.graph;

    constexpr int channels = 2;
    constexpr int branchLatency = 64;

    auto* audioIn  = new IONode (IONode::audioInputNode);
    // Reports latency AND actually delays by the same amount, like a real plugin.
    auto* delayed  = new LatentTestNode (channels, branchLatency, /*actuallyDelay=*/true);
    // Direct pass-through with no reported latency: PDC must compensate this branch.
    auto* direct   = new LatentTestNode (channels, 0, false);
    auto* audioOut = new IONode (IONode::audioOutputNode);

    graph.addNode (audioIn);
    graph.addNode (delayed);
    graph.addNode (direct);
    graph.addNode (audioOut);

    // Input feeds both branches; both branches feed the output.
    connectAudioChannels (graph, *audioIn, *delayed, channels);
    connectAudioChannels (graph, *audioIn, *direct,  channels);
    connectAudioChannels (graph, *delayed, *audioOut, channels);
    connectAudioChannels (graph, *direct,  *audioOut, channels);

    graph.rebuild();

    BOOST_REQUIRE_EQUAL (graph.getLatencySamples(), branchLatency);

    juce::AudioSampleBuffer audio (channels, 512);
    audio.clear();
    audio.setSample (0, 0, 1.0f);

    renderOneBlock (graph, audio, 512);

    // After compensation, the direct branch is delayed by branchLatency to
    // match the delayed branch. Both impulses land at sample==branchLatency
    // and sum to 2.0. Nothing should appear at sample 0 (which would mean the
    // direct branch was NOT compensated).
    BOOST_CHECK_SMALL (audio.getSample (0, 0), 1e-6f);
    BOOST_CHECK_CLOSE (audio.getSample (0, branchLatency), 2.0f, 1e-3);
}

// A node whose latency changes after the graph is built triggers a rebuild
// (via Processor::setLatencySamples notifying the parent graph) and the
// graph's reported latency updates accordingly.
BOOST_AUTO_TEST_CASE (LatencyChangeTriggersGraphRebuild)
{
    PreparedGraph fix (44100.0, 512);
    auto& graph = fix.graph;

    auto* audioIn  = new IONode (IONode::audioInputNode);
    auto* latent   = new LatentTestNode (2, 50, true);
    auto* audioOut = new IONode (IONode::audioOutputNode);

    graph.addNode (audioIn);
    graph.addNode (latent);
    graph.addNode (audioOut);

    connectAudioChannels (graph, *audioIn,  *latent,   2);
    connectAudioChannels (graph, *latent,   *audioOut, 2);

    graph.rebuild();
    BOOST_REQUIRE_EQUAL (graph.getLatencySamples(), 50);

    // setLatency changes the node's reported latency. Processor::setLatencySamples
    // schedules an async rebuild on the parent graph; rebuild() runs it synchronously.
    latent->setLatency (200);
    graph.rebuild();

    BOOST_REQUIRE_EQUAL (graph.getLatencySamples(), 200);
}

// The Element-specific bug this work fixes: a sub-graph (a GraphNode nested
// inside another GraphNode) whose internal latency changes must propagate its
// new latency up to the parent graph's PDC computation. Without the
// setLatencySamples() -> parent->triggerAsyncUpdate() wiring in
// Processor::setLatencySamples, the parent would keep a stale value here.
BOOST_AUTO_TEST_CASE (SubGraphLatencyPropagatesUpToParent)
{
    auto* sharedContext = element::test::context();
    PreparedGraph fix (44100.0, 512);
    auto& outer = fix.graph;

    // Build the sub-graph first, with its own latent node inside.
    auto* sub = new GraphNode (*sharedContext);
    sub->setNumPorts (PortType::Audio, 2, true,  false);
    sub->setNumPorts (PortType::Audio, 2, false, false);

    auto* subIn  = new IONode (IONode::audioInputNode);
    auto* subLat = new LatentTestNode (2, 75, true);
    auto* subOut = new IONode (IONode::audioOutputNode);
    sub->addNode (subIn);
    sub->addNode (subLat);
    sub->addNode (subOut);
    connectAudioChannels (*sub, *subIn,  *subLat, 2);
    connectAudioChannels (*sub, *subLat, *subOut, 2);

    // Insert the sub-graph into the outer graph, wired straight through.
    auto* outerIn  = new IONode (IONode::audioInputNode);
    auto* outerOut = new IONode (IONode::audioOutputNode);
    outer.addNode (outerIn);
    outer.addNode (sub);
    outer.addNode (outerOut);

    connectAudioChannels (outer, *outerIn, *sub,      2);
    connectAudioChannels (outer, *sub,     *outerOut, 2);

    outer.rebuild();
    sub->rebuild();
    outer.rebuild();

    BOOST_REQUIRE_EQUAL (sub->getLatencySamples(), 75);
    BOOST_REQUIRE_EQUAL (outer.getLatencySamples(), 75);

    // Change the inner node's latency. The chain is:
    //   subLat.setLatencySamples(150) -> parent (sub) triggerAsyncUpdate
    //   sub.handleAsyncUpdate -> buildRenderingSequence
    //   sub.setLatencySamples(150) -> parent (outer) triggerAsyncUpdate
    // We invoke rebuild() at each level to flush the async updates synchronously.
    subLat->setLatency (150);
    sub->rebuild();
    outer.rebuild();

    BOOST_REQUIRE_EQUAL (sub->getLatencySamples(), 150);
    BOOST_REQUIRE_EQUAL (outer.getLatencySamples(), 150);
}

// The RenderSequenceSignature optimisation: rebuilding the graph when nothing
// relevant changed should be a no-op (signature compare returns equal, the
// build is skipped). We verify behaviorally by watching renderingSequenceChanged
// fire only when the signature genuinely changes.
BOOST_AUTO_TEST_CASE (UnchangedRebuildIsSkipped)
{
    PreparedGraph fix (44100.0, 512);
    auto& graph = fix.graph;

    auto* audioIn  = new IONode (IONode::audioInputNode);
    auto* latent   = new LatentTestNode (2, 40, true);
    auto* audioOut = new IONode (IONode::audioOutputNode);

    graph.addNode (audioIn);
    graph.addNode (latent);
    graph.addNode (audioOut);
    connectAudioChannels (graph, *audioIn, *latent,   2);
    connectAudioChannels (graph, *latent,  *audioOut, 2);

    int rebuildCount = 0;
    auto conn = graph.renderingSequenceChanged.connect ([&rebuildCount]() { ++rebuildCount; });

    graph.rebuild();
    const int afterFirstRebuild = rebuildCount;
    BOOST_REQUIRE (afterFirstRebuild >= 1);

    // Same signature -> no rebuild.
    graph.rebuild();
    BOOST_CHECK_EQUAL (rebuildCount, afterFirstRebuild);

    // Genuinely different latency -> rebuild fires again.
    latent->setLatency (100);
    graph.rebuild();
    BOOST_CHECK_GT (rebuildCount, afterFirstRebuild);

    conn.disconnect();
}

BOOST_AUTO_TEST_SUITE_END()
