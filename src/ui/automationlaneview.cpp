// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/automationlaneview.hpp"

#include <element/context.hpp>
#include <element/session.hpp>
#include <element/node.hpp>
#include <element/node.h>
#include <element/audioengine.hpp>
#include <element/transport.hpp>

#include "nodes/parametermapper.hpp"

namespace element {

namespace {
constexpr int kHeaderH = 28;
constexpr int kRulerH  = 18;
constexpr float kVPad  = 6.0f;  // vertical padding inside the plot
constexpr float kHitR  = 6.0f;  // point hit radius, pixels
constexpr int kUpdateHz = 30;

const juce::Colour kBgTop    { 0xff202327 };
const juce::Colour kBgPlot   { 0xff15171a };
const juce::Colour kGrid     { 0xff2b2f34 };
const juce::Colour kAccent   { 0xff6cc6ff };
const juce::Colour kPlayhead { 0xffff5a3c };
const juce::Colour kText     { 0xffc7ccd1 };

// Recursively searches a node tree for the Parameter Mapper processor.
ParameterMapperNode* searchForMapper (const Node& node)
{
    for (int i = 0; i < node.getNumNodes(); ++i)
    {
        auto child = node.getNode (i);
        if (child.getIdentifier().toString() == EL_NODE_ID_PARAM_MAPPER)
            if (auto* m = dynamic_cast<ParameterMapperNode*> (child.getObject()))
                return m;
        if (child.isGraph())
            if (auto* m = searchForMapper (child))
                return m;
    }
    return nullptr;
}

juce::String formatClock (double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    const int total = (int) seconds;
    const int mm = total / 60;
    const int ss = total % 60;
    return juce::String::formatted ("%d:%02d", mm, ss);
}
} // namespace

AutomationLaneView::AutomationLaneView (Context& c)
    : context_ (c)
{
    setOpaque (true);

    addAndMakeVisible (paramBox);
    paramBox.setTextWhenNothingSelected ("No mapped parameters");
    paramBox.setTextWhenNoChoicesAvailable ("No Parameter Mapper in graph");
    paramBox.onChange = [this]
    {
        activeSlot = paramBox.getSelectedId() - 1;
        repaint();
    };

    addAndMakeVisible (rulerModeButton);
    rulerModeButton.setTooltip ("Toggle ruler between time and bars");
    rulerModeButton.onClick = [this]
    {
        rulerInBars = ! rulerInBars;
        rulerModeButton.setButtonText (rulerInBars ? "Time" : "Bars");
        repaint();
    };

    addAndMakeVisible (zoomInButton);
    zoomInButton.onClick = [this]
    {
        const auto p = plotBounds().toFloat();
        zoomAround (p.getCentreX(), 1.4);
    };
    addAndMakeVisible (zoomOutButton);
    zoomOutButton.onClick = [this]
    {
        const auto p = plotBounds().toFloat();
        zoomAround (p.getCentreX(), 1.0 / 1.4);
    };

    addAndMakeVisible (clearButton);
    clearButton.onClick = [this]
    {
        if (auto* m = findMapper())
            if (activeSlot >= 0)
            {
                m->clearAutomation (activeSlot);
                repaint();
            }
    };

    startTimerHz (kUpdateHz);
}

AutomationLaneView::~AutomationLaneView()
{
    stopTimer();
}

//==============================================================================
ParameterMapperNode* AutomationLaneView::findMapper() const
{
    if (auto s = context_.session())
    {
        auto graph = s->getActiveGraph();
        if (graph.isValid())
            return searchForMapper (graph);
    }
    return nullptr;
}

juce::String AutomationLaneView::computeMappingSignature (ParameterMapperNode* mapper) const
{
    if (mapper == nullptr)
        return {};
    juce::String sig;
    for (int i = 0; i < ParameterMapperNode::kNumSlots; ++i)
    {
        const auto slot = mapper->getSlot (i);
        if (slot.targetNodeId != 0 && slot.targetParamIndex >= 0)
            sig << i << ':' << mapper->getDisplayLabel (i) << ';';
    }
    return sig;
}

void AutomationLaneView::rebuildParamBox (ParameterMapperNode* mapper)
{
    const int previous = activeSlot;
    paramBox.clear (juce::dontSendNotification);

    if (mapper != nullptr)
    {
        for (int i = 0; i < ParameterMapperNode::kNumSlots; ++i)
        {
            const auto slot = mapper->getSlot (i);
            if (slot.targetNodeId == 0 || slot.targetParamIndex < 0)
                continue;
            const int bank = i / ParameterMapperNode::kKnobsPerBank;
            const int knob = i % ParameterMapperNode::kKnobsPerBank;
            auto label = mapper->getDisplayLabel (i);
            paramBox.addItem (juce::String::formatted ("B%d.%02d  ", bank + 1, knob + 1) + label, i + 1);
        }
    }

    // Keep the previous selection if it's still a mapped slot; otherwise pick
    // the first available, or clear.
    if (previous >= 0 && paramBox.indexOfItemId (previous + 1) >= 0)
    {
        paramBox.setSelectedId (previous + 1, juce::dontSendNotification);
        activeSlot = previous;
    }
    else if (paramBox.getNumItems() > 0)
    {
        paramBox.setSelectedItemIndex (0, juce::dontSendNotification);
        activeSlot = paramBox.getSelectedId() - 1;
    }
    else
    {
        activeSlot = -1;
    }
}

//==============================================================================
juce::Rectangle<int> AutomationLaneView::plotBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop (kHeaderH);
    r.removeFromTop (kRulerH);
    return r;
}

float AutomationLaneView::timeToX (double seconds) const
{
    const auto p = plotBounds();
    return (float) p.getX() + (float) ((seconds - viewStart) * pixelsPerSecond);
}

double AutomationLaneView::xToTime (float x) const
{
    const auto p = plotBounds();
    if (pixelsPerSecond <= 0.0)
        return 0.0;
    return viewStart + ((double) x - (double) p.getX()) / pixelsPerSecond;
}

float AutomationLaneView::valueToY (float value) const
{
    const auto p = plotBounds().toFloat();
    const float top = p.getY() + kVPad;
    const float bot = p.getBottom() - kVPad;
    value = juce::jlimit (0.0f, 1.0f, value);
    return bot - value * (bot - top);
}

float AutomationLaneView::yToValue (float y) const
{
    const auto p = plotBounds().toFloat();
    const float top = p.getY() + kVPad;
    const float bot = p.getBottom() - kVPad;
    if (bot <= top)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (bot - y) / (bot - top));
}

void AutomationLaneView::zoomAround (float anchorX, double factor)
{
    const auto p = plotBounds();
    if (pixelsPerSecond <= 0.0 || p.getWidth() <= 0)
        return;

    const double tAnchor = xToTime (anchorX);
    const double maxPps = 400.0;
    const double minPps = (double) p.getWidth() / ParameterMapperNode::kAutomationLengthSeconds;
    pixelsPerSecond = juce::jlimit (minPps, maxPps, pixelsPerSecond * factor);

    viewStart = tAnchor - ((double) anchorX - (double) p.getX()) / pixelsPerSecond;
    const double maxStart = juce::jmax (0.0,
        ParameterMapperNode::kAutomationLengthSeconds - (double) p.getWidth() / pixelsPerSecond);
    viewStart = juce::jlimit (0.0, maxStart, viewStart);
    repaint();
}

void AutomationLaneView::seekToTime (double seconds)
{
    seconds = juce::jlimit (0.0, ParameterMapperNode::kAutomationLengthSeconds, seconds);
    playheadSeconds = seconds;
    if (auto engine = context_.audio())
    {
        double sr = 44100.0;
        if (auto mon = engine->getTransportMonitor())
            sr = mon->sampleRate.get() > 0.0 ? mon->sampleRate.get() : sr;
        engine->seekToAudioFrame ((int64_t) (seconds * sr));
    }
    repaint();
}

//==============================================================================
void AutomationLaneView::resized()
{
    auto header = getLocalBounds().removeFromTop (kHeaderH).reduced (4, 3);

    paramBox.setBounds (header.removeFromLeft (240));
    header.removeFromLeft (6);
    clearButton.setBounds (header.removeFromRight (54));
    header.removeFromRight (4);
    zoomOutButton.setBounds (header.removeFromRight (28));
    zoomInButton.setBounds (header.removeFromRight (28));
    header.removeFromRight (4);
    rulerModeButton.setBounds (header.removeFromRight (48));

    // Default zoom: fit the whole 10-minute timeline to the plot width.
    const auto p = plotBounds();
    if (pixelsPerSecond <= 0.0 && p.getWidth() > 0)
        pixelsPerSecond = (double) p.getWidth() / ParameterMapperNode::kAutomationLengthSeconds;
}

//==============================================================================
void AutomationLaneView::paint (juce::Graphics& g)
{
    g.fillAll (kBgPlot);

    const auto bounds = getLocalBounds();

    // Header strip.
    g.setColour (kBgTop);
    g.fillRect (bounds.withHeight (kHeaderH));

    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || plot.getHeight() <= 0)
        return;

    // ---- Ruler -------------------------------------------------------------
    const auto ruler = juce::Rectangle<int> (plot.getX(), kHeaderH, plot.getWidth(), kRulerH);
    g.setColour (kBgTop.darker (0.25f));
    g.fillRect (ruler);

    double tempo = 120.0, beatsPerBar = 4.0;
    if (auto engine = context_.audio())
        if (auto mon = engine->getTransportMonitor())
        {
            if (mon->tempo.get() > 0.0f) tempo = (double) mon->tempo.get();
            if (mon->beatsPerBar.get() > 0) beatsPerBar = (double) mon->beatsPerBar.get();
        }

    g.setFont (11.0f);
    const double viewEnd = xToTime ((float) plot.getRight());

    if (! rulerInBars)
    {
        // Time ruler: choose a "nice" tick interval that gives ~80px spacing.
        static const double nice[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        const double target = 80.0 / juce::jmax (1.0e-6, pixelsPerSecond);
        double step = nice[0];
        for (double n : nice) { step = n; if (n >= target) break; }

        const double first = std::floor (viewStart / step) * step;
        for (double t = first; t <= viewEnd; t += step)
        {
            const float x = timeToX (t);
            if (x < plot.getX() - 1.0f || x > plot.getRight() + 1.0f)
                continue;
            g.setColour (kGrid);
            g.drawVerticalLine (juce::roundToInt (x), (float) kHeaderH, (float) plot.getBottom());
            g.setColour (kText.withAlpha (0.7f));
            g.drawText (formatClock (t), juce::roundToInt (x) + 3, kHeaderH, 60, kRulerH,
                        juce::Justification::centredLeft, false);
        }
    }
    else
    {
        const double secPerBar = (tempo > 0.0) ? (60.0 / tempo) * beatsPerBar : 2.0;
        // Aim for ~70px between labelled bars; show every Nth bar.
        const double pxPerBar = secPerBar * pixelsPerSecond;
        int barStep = 1;
        while (pxPerBar * barStep < 60.0) barStep *= 2;

        const int firstBar = juce::jmax (0, (int) std::floor (viewStart / secPerBar));
        for (int bar = firstBar - (firstBar % barStep);; bar += barStep)
        {
            const double t = bar * secPerBar;
            const float x = timeToX (t);
            if (x > plot.getRight() + 1.0f)
                break;
            if (x < plot.getX() - 1.0f)
                continue;
            g.setColour (kGrid);
            g.drawVerticalLine (juce::roundToInt (x), (float) kHeaderH, (float) plot.getBottom());
            g.setColour (kText.withAlpha (0.7f));
            g.drawText (juce::String (bar + 1), juce::roundToInt (x) + 3, kHeaderH, 50, kRulerH,
                        juce::Justification::centredLeft, false);
        }
    }

    // ---- Plot grid (value gridlines) --------------------------------------
    g.setColour (kGrid.withAlpha (0.6f));
    for (int i = 0; i <= 4; ++i)
    {
        const float y = valueToY (i / 4.0f);
        g.drawHorizontalLine (juce::roundToInt (y), (float) plot.getX(), (float) plot.getRight());
    }

    // ---- Curve + points ----------------------------------------------------
    auto* mapper = findMapper();
    if (mapper != nullptr && activeSlot >= 0)
    {
        const auto pts = mapper->getAutomation (activeSlot);

        if (pts.empty())
        {
            g.setColour (kText.withAlpha (0.4f));
            g.setFont (13.0f);
            g.drawText ("Click to add an automation point",
                        plot, juce::Justification::centred, false);
        }
        else
        {
            juce::Path path;
            const int x0 = plot.getX();
            const int x1 = plot.getRight();
            bool started = false;
            for (int x = x0; x <= x1; ++x)
            {
                const double t = xToTime ((float) x);
                const float v = ParameterMapperNode::automationValueAt (pts, t);
                const float y = valueToY (v);
                if (! started) { path.startNewSubPath ((float) x, y); started = true; }
                else           { path.lineTo ((float) x, y); }
            }
            g.setColour (kAccent);
            g.strokePath (path, juce::PathStrokeType (1.6f));

            // Soft fill below the curve.
            juce::Path fill = path;
            fill.lineTo ((float) x1, (float) plot.getBottom());
            fill.lineTo ((float) x0, (float) plot.getBottom());
            fill.closeSubPath();
            g.setColour (kAccent.withAlpha (0.08f));
            g.fillPath (fill);

            // Points.
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const float x = timeToX (pts[i].time);
                if (x < plot.getX() - 8.0f || x > plot.getRight() + 8.0f)
                    continue;
                const float y = valueToY (pts[i].value);
                const bool active = ((int) i == dragPoint);
                g.setColour (active ? juce::Colours::white : kAccent);
                g.fillEllipse (x - kHitR, y - kHitR, kHitR * 2.0f, kHitR * 2.0f);
                g.setColour (kBgPlot);
                g.fillEllipse (x - kHitR + 2.0f, y - kHitR + 2.0f, (kHitR - 2.0f) * 2.0f, (kHitR - 2.0f) * 2.0f);
            }
        }
    }
    else
    {
        g.setColour (kText.withAlpha (0.4f));
        g.setFont (13.0f);
        g.drawText (mapper == nullptr ? "Add a Parameter Mapper node to use the automation lane"
                                      : "Map a parameter on the Parameter Mapper to begin",
                    plot, juce::Justification::centred, false);
    }

    // ---- Playhead ----------------------------------------------------------
    {
        const float x = timeToX (playheadSeconds);
        if (x >= plot.getX() - 1.0f && x <= plot.getRight() + 1.0f)
        {
            g.setColour (kPlayhead);
            g.drawVerticalLine (juce::roundToInt (x), (float) kHeaderH, (float) plot.getBottom());
            juce::Path tri;
            tri.addTriangle (x - 5.0f, (float) kHeaderH, x + 5.0f, (float) kHeaderH, x, (float) kHeaderH + 6.0f);
            g.fillPath (tri);
        }
    }

    // ---- Value readout while dragging -------------------------------------
    if (showValue)
    {
        auto text = juce::String (juce::roundToInt (dragValue * 100.0f)) + "%";
        g.setFont (12.0f);
        const int w = juce::jmax (34, g.getCurrentFont().getStringWidth (text) + 12);
        const int h = 18;
        int bx = juce::roundToInt (valueAnchor.x) - w / 2;
        int by = juce::roundToInt (valueAnchor.y) - h - 8;
        bx = juce::jlimit (plot.getX(), plot.getRight() - w, bx);
        by = juce::jmax (kHeaderH + kRulerH, by);
        juce::Rectangle<int> bubble (bx, by, w, h);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.fillRoundedRectangle (bubble.toFloat(), 3.0f);
        g.setColour (juce::Colours::white);
        g.drawText (text, bubble, juce::Justification::centred, false);
    }
}

//==============================================================================
void AutomationLaneView::mouseDown (const juce::MouseEvent& e)
{
    dragMode = Drag::none;
    dragPoint = -1;
    dragSegment = -1;
    didDrag = false;
    addedThisGesture = false;
    showValue = false;

    const auto plot = plotBounds();

    // Ruler: scrub.
    if (e.y >= kHeaderH && e.y < kHeaderH + kRulerH)
    {
        dragMode = Drag::scrub;
        seekToTime (xToTime ((float) e.x));
        return;
    }

    if (! plot.contains (e.getPosition()))
        return;

    auto* mapper = findMapper();
    if (mapper == nullptr || activeSlot < 0)
        return;

    const auto pts = mapper->getAutomation (activeSlot);
    const juce::Point<float> mp = e.position;

    // Alt-drag bends the segment under the cursor.
    if (e.mods.isAltDown() && pts.size() >= 2)
    {
        const double t = xToTime ((float) e.x);
        for (size_t i = 1; i < pts.size(); ++i)
        {
            if (t <= pts[i].time)
            {
                dragSegment = (int) (i - 1);
                bendStartCurve = pts[dragSegment].curve;
                bendStartY = mp.y;
                dragMode = Drag::bend;
                return;
            }
        }
        return;
    }

    // Hit-test existing points.
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const float x = timeToX (pts[i].time);
        const float y = valueToY (pts[i].value);
        if (mp.getDistanceFrom ({ x, y }) <= kHitR + 2.0f)
        {
            dragMode = Drag::move;
            dragPoint = (int) i;
            valueAnchor = { x, y };
            return;
        }
    }

    // Empty space: drop a new point and start moving it immediately.
    const double t = juce::jlimit (0.0, ParameterMapperNode::kAutomationLengthSeconds, xToTime ((float) e.x));
    const float v = yToValue (mp.y);
    const int idx = mapper->addAutomationPoint (activeSlot, t, v);
    if (idx >= 0)
    {
        dragMode = Drag::move;
        dragPoint = idx;
        addedThisGesture = true;
        showValue = true;
        dragValue = v;
        valueAnchor = { timeToX (t), valueToY (v) };
    }
    repaint();
}

void AutomationLaneView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3)
        didDrag = true;

    auto* mapper = findMapper();
    if (mapper == nullptr)
        return;

    switch (dragMode)
    {
        case Drag::scrub:
            seekToTime (xToTime ((float) e.x));
            break;

        case Drag::move:
        {
            if (activeSlot < 0 || dragPoint < 0)
                break;
            const double t = juce::jlimit (0.0, ParameterMapperNode::kAutomationLengthSeconds, xToTime ((float) e.x));
            const float v = yToValue (e.position.y);
            mapper->moveAutomationPoint (activeSlot, dragPoint, t, v);
            showValue = true;
            dragValue = v;
            valueAnchor = { timeToX (t), valueToY (v) };
            repaint();
            break;
        }

        case Drag::bend:
        {
            if (activeSlot < 0 || dragSegment < 0)
                break;
            const float dy = bendStartY - e.position.y; // up => positive curve
            const float c = juce::jlimit (-1.0f, 1.0f, bendStartCurve + dy * 0.01f);
            mapper->setAutomationCurve (activeSlot, dragSegment, c);
            repaint();
            break;
        }

        case Drag::none:
            break;
    }
}

void AutomationLaneView::mouseUp (const juce::MouseEvent&)
{
    // A plain click (no drag) on an existing point deletes it.
    if (dragMode == Drag::move && ! didDrag && ! addedThisGesture && dragPoint >= 0)
        if (auto* mapper = findMapper())
            if (activeSlot >= 0)
                mapper->removeAutomationPoint (activeSlot, dragPoint);

    dragMode = Drag::none;
    dragPoint = -1;
    dragSegment = -1;
    showValue = false;
    repaint();
}

void AutomationLaneView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double factor = wheel.deltaY > 0 ? 1.15 : (wheel.deltaY < 0 ? 1.0 / 1.15 : 1.0);
        if (factor != 1.0)
            zoomAround ((float) e.x, factor);
        return;
    }

    // Plain wheel pans horizontally.
    const auto p = plotBounds();
    if (pixelsPerSecond <= 0.0 || p.getWidth() <= 0)
        return;
    const double visible = (double) p.getWidth() / pixelsPerSecond;
    viewStart -= wheel.deltaY * visible * 0.25;
    const double maxStart = juce::jmax (0.0, ParameterMapperNode::kAutomationLengthSeconds - visible);
    viewStart = juce::jlimit (0.0, maxStart, viewStart);
    repaint();
}

//==============================================================================
void AutomationLaneView::timerCallback()
{
    auto* mapper = findMapper();

    const auto sig = computeMappingSignature (mapper);
    if (sig != mappingSig)
    {
        mappingSig = sig;
        rebuildParamBox (mapper);
    }

    if (auto engine = context_.audio())
        if (auto mon = engine->getTransportMonitor())
            playheadSeconds = mon->getPositionSeconds();

    repaint();
}

} // namespace element
