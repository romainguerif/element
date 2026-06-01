// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/element.hpp>
#include <element/juce/gui_basics.hpp>

namespace element {

class Context;
class ParameterMapperNode;

/** A Serato-Studio-style automation lane that lives in the bottom zone.

    The lane edits the automation curve of ONE of the 64 slots of the
    Parameter Mapper node currently in the active graph. A dropdown picks
    which mapped slot to view/edit. Every slot that has points plays back
    simultaneously (driven from ParameterMapperNode::render()); this view only
    chooses what to draw.

    Interaction (Serato-inspired, free time placement -- no grid snap):
      - Empty lane: a single click drops a point, which (because the curve
        holds flat past its ends) effectively draws a horizontal line across
        the whole timeline at that value.
      - Click-drag a point to move it; the value is shown above the cursor.
      - A plain click on an existing point (press + release without dragging)
        deletes it.
      - Alt-drag on a segment bends its curvature.
      - Drag on the ruler, or on the playhead, to scrub the transport.
      - Cmd/Ctrl + wheel zooms horizontally around the cursor; plain wheel
        pans. */
class AutomationLaneView : public juce::Component,
                           private juce::Timer
{
public:
    explicit AutomationLaneView (Context&);
    ~AutomationLaneView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    // Locates the Parameter Mapper in the active graph (recursively), or null.
    ParameterMapperNode* findMapper() const;

    // Rebuilds the slot dropdown to list every mapped slot of `mapper`.
    void rebuildParamBox (ParameterMapperNode* mapper);
    // A cheap fingerprint of the mapper's slots so we only rebuild the combo
    // (and re-resolve the active slot) when the mapping actually changes.
    juce::String computeMappingSignature (ParameterMapperNode* mapper) const;

    // Geometry helpers (plot area <-> time/value space).
    juce::Rectangle<int> plotBounds() const;
    float timeToX (double seconds) const;
    double xToTime (float x) const;
    float valueToY (float value) const;
    float yToValue (float y) const;

    void zoomAround (float anchorX, double factor);
    void seekToTime (double seconds);

    Context& context_;

    juce::ComboBox paramBox;
    juce::TextButton rulerModeButton { "Bars" };
    juce::TextButton zoomInButton    { "+" };
    juce::TextButton zoomOutButton   { "-" };
    juce::TextButton clearButton     { "Clear" };
    juce::TextButton snapButton      { "Snap" }; // snapshot lane only

    // Snapshot-lane snapping: magnetically locks a point's value onto the
    // nearest snapshot level so the user can build clean step-plateaus, while
    // still placing freely between levels for smooth morphs.
    bool snapToSnapshots = true;
    bool onSnapshotLane() const;
    // Fills `levels`/`nums` (value 0..1 and snapshot number) for each snapshot
    // that holds data; returns the count. Levels are evenly spaced 0..1.
    int  collectSnapshotLevels (float* levels, int* nums, int maxN) const;
    // Snaps `value` to the nearest snapshot level when on the snapshot lane and
    // snapping is on and the cursor is within a few pixels of a level.
    float snapSnapshotValue (float value, float cursorY) const;
    void updateSnapButton();

    int activeSlot = -1;          // 0..63, or -1 when nothing is selected
    juce::String mappingSig;      // last seen mapping fingerprint

    bool rulerInBars = false;     // false = time (mm:ss), true = bars
    double viewStart = 0.0;       // seconds at the left edge of the plot
    double pixelsPerSecond = 0.0; // horizontal zoom; 0 => fit-to-width once laid out

    double playheadSeconds = 0.0;

    enum class Drag { none, move, bend, scrub };
    Drag dragMode = Drag::none;
    int dragPoint = -1;           // index of the point being moved
    int dragSegment = -1;         // index of the point that starts the bent segment
    bool didDrag = false;         // moved beyond a tiny threshold since mouseDown
    bool addedThisGesture = false;// the move started by creating a fresh point
    float bendStartCurve = 0.0f;
    float bendStartY = 0.0f;

    bool showValue = false;       // draw the value readout bubble while dragging
    float dragValue = 0.0f;
    juce::Point<float> valueAnchor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneView)
};

} // namespace element
