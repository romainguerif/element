// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/parametermappereditor.hpp"
#include "nodes/parametermapper.hpp"

namespace element {

//==============================================================================
class ParameterMapperEditor::KnobCell : public juce::Component
{
public:
    KnobCell (ParameterMapperNode& m, int absoluteIndex)
        : mapper (m), slot (absoluteIndex)
    {
        addAndMakeVisible (label);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        label.setEditable (false, true, false);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
        label.onTextChange = [this]() {
            mapper.setSlotLabel (slot, label.getText());
        };

        addAndMakeVisible (knob);
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange (0.0, 1.0);
        knob.setDoubleClickReturnValue (true, 0.0);
        knob.onValueChange = [this]() {
            mapper.setKnobValue (slot, (float) knob.getValue());
            valueText.setText (juce::String (knob.getValue(), 2), juce::dontSendNotification);
        };

        addAndMakeVisible (valueText);
        valueText.setJustificationType (juce::Justification::centred);
        valueText.setFont (juce::Font (juce::FontOptions (10.0f)));
        valueText.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));

        refresh();
    }

    /** Re-binds this cell to a different absolute slot (used when the bank
        changes -- we re-use the same 16 cells across all 4 banks rather than
        rebuilding them). */
    void setSlot (int absoluteIndex)
    {
        slot = absoluteIndex;
        refresh();
    }

    int getSlot() const noexcept { return slot; }

    void refresh()
    {
        const auto s = mapper.getSlot (slot);
        knob.setValue (s.value, juce::dontSendNotification);
        label.setText (mapper.getDisplayLabel (slot), juce::dontSendNotification);
        valueText.setText (juce::String (s.value, 2), juce::dontSendNotification);

        const bool learning = mapper.isLearning();
        knob.setInterceptsMouseClicks (! learning, ! learning);
        label.setInterceptsMouseClicks (! learning, ! learning);
        valueText.setInterceptsMouseClicks (false, false);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool isLearningHere = mapper.isLearning() && mapper.getLearningSlot() == slot;
        if (isLearningHere)
        {
            g.setColour (juce::Colours::red.withAlpha (0.18f));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
            g.setColour (juce::Colours::red.withAlpha (0.85f));
            g.drawRoundedRectangle (getLocalBounds().reduced (1).toFloat(), 4.0f, 1.5f);
            return;
        }

        const bool isMapped = mapper.getSlot (slot).targetParamIndex >= 0;
        g.setColour (juce::Colour (isMapped ? 0xff202327u : 0xff181a1cu));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colours::white.withAlpha (isMapped ? 0.15f : 0.06f));
        g.drawRoundedRectangle (getLocalBounds().reduced (1).toFloat(), 4.0f, 1.0f);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const int previewIdx = mapper.getPreviewSnapshot();
        if (previewIdx < 0)
            return;
        const float target = mapper.getSnapshotValue (previewIdx, slot);
        if (std::isnan (target))
            return;

        // Draw a circular arc that traces the "remaining path" between the
        // current knob value and the target. The arc lives on a track inset
        // from the rotary's main arc (so it never overlaps the rotary's own
        // value indicator) and uses a contrasting colour. As the user
        // physically moves the knob toward the target, the arc shrinks to
        // nothing -- visual feedback for a manual transition.
        const auto kb = knob.getBoundsInParent().toFloat();
        const auto cx = kb.getCentreX();
        const auto cy = kb.getCentreY();
        const float outerR = juce::jmin (kb.getWidth(), kb.getHeight()) * 0.5f - 4.0f;
        if (outerR <= 6.0f)
            return;

        // Rotary geometry: matches JUCE's default rotary start/end angles.
        constexpr float startAng = juce::MathConstants<float>::pi * 1.2f;
        constexpr float endAng   = juce::MathConstants<float>::pi * 2.8f;

        const float current = mapper.getSlot (slot).value;
        const float curAng  = startAng + current * (endAng - startAng);
        const float tgtAng  = startAng + target  * (endAng - startAng);

        const float radius = outerR + 2.0f; // outset slightly so we sit just outside the rotary's ring
        const float thickness = 2.6f;

        const float a0 = juce::jmin (curAng, tgtAng);
        const float a1 = juce::jmax (curAng, tgtAng);

        juce::Path arc;
        arc.addCentredArc (cx, cy, radius, radius, 0.0f,
                            a0, a1, true);

        const juce::Colour targetCol (0xff10cce8); // bright cyan
        g.setColour (targetCol.withAlpha (0.95f));
        g.strokePath (arc, juce::PathStrokeType (thickness,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        // Mark the target end of the arc with a small dot so the user can
        // tell which direction they should turn.
        const float ta = tgtAng - juce::MathConstants<float>::halfPi;
        const float tx = cx + std::cos (ta) * radius;
        const float ty = cy + std::sin (ta) * radius;
        g.fillEllipse (tx - 2.5f, ty - 2.5f, 5.0f, 5.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        label.setBounds (r.removeFromTop (16));
        valueText.setBounds (r.removeFromBottom (14));
        knob.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            const bool mapped = mapper.getSlot (slot).targetParamIndex >= 0;
            m.addItem (1, "Learn", true);
            m.addItem (2, "Clear mapping", mapped);
            m.addItem (3, "Rename", true);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                             [this] (int r) {
                                 if (r == 1) mapper.beginLearn (slot);
                                 else if (r == 2) mapper.unmap (slot);
                                 else if (r == 3) label.showEditor();
                             });
            return;
        }

        if (mapper.isLearning())
        {
            mapper.beginLearn (slot);
            return;
        }
    }

private:
    ParameterMapperNode& mapper;
    int slot;

    juce::Label label;
    juce::Slider knob;
    juce::Label valueText;
};

//==============================================================================
class ParameterMapperEditor::SnapshotButton : public juce::Component
{
public:
    SnapshotButton (ParameterMapperNode& m, int index_)
        : mapper (m), index (index_) {}

    void paint (juce::Graphics& g) override
    {
        const bool hasData   = mapper.snapshotHasData (index);
        const bool isPreview = mapper.getPreviewSnapshot() == index;
        const bool isArmed   = mapper.getArmedSnapshot() == index;

        juce::Colour bg = hasData ? juce::Colour (0xff353a3f)
                                  : juce::Colour (0xff1a1c1f);
        if (isPreview) bg = juce::Colour (0xff124a52);
        if (isArmed)   bg = juce::Colour (0xff4a3a12);

        g.setColour (bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

        // Highlight border (priority: armed > preview > default).
        if (isArmed)
        {
            g.setColour (juce::Colour (0xfff5b400).withAlpha (0.95f)); // amber
            g.drawRoundedRectangle (getLocalBounds().reduced (1).toFloat(), 3.0f, 1.6f);
        }
        else if (isPreview)
        {
            g.setColour (juce::Colours::cyan.withAlpha (0.90f));
            g.drawRoundedRectangle (getLocalBounds().reduced (1).toFloat(), 3.0f, 1.5f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (hasData ? 0.20f : 0.08f));
            g.drawRoundedRectangle (getLocalBounds().reduced (1).toFloat(), 3.0f, 1.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (hasData ? 0.90f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (juce::String (index + 1), getLocalBounds(), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isShiftDown())
        {
            mapper.recordSnapshot (index);
            return;
        }
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            const bool has = mapper.snapshotHasData (index);
            m.addItem (1, "Record (current state)", true);
            m.addItem (2, "Apply now (no bar-sync)", has);
            m.addItem (3, "Clear", has);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                             [this] (int r) {
                                 if (r == 1) mapper.recordSnapshot (index);
                                 else if (r == 2) mapper.applySnapshot (index);
                                 else if (r == 3) mapper.clearSnapshot (index);
                             });
            return;
        }
        if (! mapper.snapshotHasData (index))
            return;

        if (mapper.isPreviewModeEnabled())
        {
            // Preview mode: visual diff only, no parameter changes.
            if (mapper.getPreviewSnapshot() == index)
                mapper.setPreviewSnapshot (-1); // toggle off
            else
                mapper.setPreviewSnapshot (index);
        }
        else
        {
            // Default mode: arm for next-bar apply (or cancel if already armed).
            if (mapper.getArmedSnapshot() == index)
                mapper.armSnapshot (-1);
            else
                mapper.armSnapshot (index);
        }
    }

private:
    ParameterMapperNode& mapper;
    int index;
};

//==============================================================================
ParameterMapperEditor::ParameterMapperEditor (const Node& node)
    : NodeEditor (node)
{
    mapper = dynamic_cast<ParameterMapperNode*> (node.getObject());

    addAndMakeVisible (learnButton);
    learnButton.setClickingTogglesState (false);
    learnButton.onClick = [this]() { toggleLearnMode(); };

    addAndMakeVisible (syncButton);
    syncButton.setTooltip ("Push all 64 knob values to the Twister so its LED rings match the host state.");
    syncButton.onClick = [this]() {
        if (mapper) mapper->pushAllToHardware();
    };

    addAndMakeVisible (previewButton);
    previewButton.setClickingTogglesState (true);
    previewButton.setTooltip ("Toggle preview-only mode: snapshot clicks show the visual diff instead of arming for the next bar.");
    previewButton.onClick = [this]() {
        if (mapper) mapper->setPreviewModeEnabled (previewButton.getToggleState());
    };

    addAndMakeVisible (nativeButton);
    nativeButton.setClickingTogglesState (true);
    nativeButton.setTooltip ("Activate Twister Native Mode protocol (SysEx). Requires native firmware to be flashed. Resets on Twister disconnect; toggle off/on to re-arm.");
    nativeButton.onClick = [this]() {
        if (mapper) mapper->setNativeMode (nativeButton.getToggleState());
    };

    // Four bank buttons across the top, mirroring the Twister's banks.
    for (int b = 0; b < ParameterMapperNode::kNumBanks; ++b)
    {
        auto* btn = bankButtons.add (new juce::TextButton ("B" + juce::String (b + 1)));
        btn->setRadioGroupId (1);
        btn->setClickingTogglesState (true);
        btn->setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        addAndMakeVisible (btn);
        btn->onClick = [this, b]() {
            if (mapper) mapper->setCurrentBank (b, /*sendToHardware=*/true);
        };
    }

    if (mapper)
    {
        for (int i = 0; i < ParameterMapperNode::kKnobsPerBank; ++i)
            knobs.add (new KnobCell (*mapper,
                                     mapper->getCurrentBank() * ParameterMapperNode::kKnobsPerBank + i));
        for (auto* k : knobs)
            addAndMakeVisible (k);

        for (int i = 0; i < ParameterMapperNode::kNumSnapshots; ++i)
            snapshotButtons.add (new SnapshotButton (*mapper, i));
        for (auto* sb : snapshotButtons)
            addAndMakeVisible (sb);

        mapper->addStateListener (this);
    }

    setSize (480, 380);
    refreshFromNode();

    // Belt-and-braces refresh: even with notifyListeners on bank changes,
    // a slow stream of CC updates from the Twister should not flood the
    // message thread. We poll lightly here -- this is just UI sync.
    startTimerHz (30);
}

void ParameterMapperEditor::timerCallback()
{
    refreshFromNode();
}

ParameterMapperEditor::~ParameterMapperEditor()
{
    if (mapper)
        mapper->removeStateListener (this);
}

//==============================================================================
void ParameterMapperEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1c1f));
}

void ParameterMapperEditor::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto topRow = r.removeFromTop (28);
    learnButton.setBounds (topRow.removeFromLeft (90));
    topRow.removeFromLeft (4);
    syncButton.setBounds (topRow.removeFromLeft (54));
    topRow.removeFromLeft (4);
    previewButton.setBounds (topRow.removeFromLeft (68));
    topRow.removeFromLeft (4);
    nativeButton.setBounds (topRow.removeFromLeft (62));
    topRow.removeFromLeft (10);
    const int bankW = juce::jmin (40, topRow.getWidth() / juce::jmax (1, bankButtons.size()));
    for (auto* b : bankButtons)
        b->setBounds (topRow.removeFromLeft (bankW));

    r.removeFromTop (6);

    // Reserve a bottom strip for the 16 snapshot buttons.
    constexpr int snapStripH = 26;
    if (! snapshotButtons.isEmpty())
    {
        auto strip = r.removeFromBottom (snapStripH);
        r.removeFromBottom (4);
        const int nb = snapshotButtons.size();
        const int btnW = juce::jmax (16, strip.getWidth() / nb);
        for (int i = 0; i < nb; ++i)
            snapshotButtons[i]->setBounds (strip.getX() + i * btnW + 1,
                                            strip.getY(),
                                            btnW - 2,
                                            strip.getHeight());
    }

    if (knobs.isEmpty())
        return;

    constexpr int cols = 4;
    constexpr int rows = 4;
    const int cellW = r.getWidth() / cols;
    const int cellH = r.getHeight() / rows;
    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            const int idx = y * cols + x;
            if (idx >= knobs.size()) break;
            knobs[idx]->setBounds (r.getX() + x * cellW + 2,
                                   r.getY() + y * cellH + 2,
                                   cellW - 4,
                                   cellH - 4);
        }
    }
}

//==============================================================================
void ParameterMapperEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromNode();
}

void ParameterMapperEditor::refreshFromNode()
{
    if (! mapper)
        return;

    const bool learning = mapper->isLearning();
    learnButton.setButtonText (learning ? "Cancel learn" : "Learn");
    learnButton.setColour (juce::TextButton::buttonColourId,
                           learning ? juce::Colours::darkred : juce::Colour (0xff2d3034));

    const int bank = mapper->getCurrentBank();
    if (bank >= 0 && bank < bankButtons.size())
        bankButtons[bank]->setToggleState (true, juce::dontSendNotification);

    previewButton.setToggleState (mapper->isPreviewModeEnabled(), juce::dontSendNotification);
    previewButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff124a52));
    nativeButton.setToggleState (mapper->isNativeMode(), juce::dontSendNotification);
    nativeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff4a3a12));

    // Rebind cells to the current bank's slot range.
    for (int i = 0; i < knobs.size(); ++i)
    {
        const int absolute = bank * ParameterMapperNode::kKnobsPerBank + i;
        if (knobs[i]->getSlot() != absolute)
            knobs[i]->setSlot (absolute);
        else
            knobs[i]->refresh();
    }

    for (auto* sb : snapshotButtons)
        sb->repaint();
}

void ParameterMapperEditor::toggleLearnMode()
{
    if (! mapper) return;
    if (mapper->isLearning())
        mapper->cancelLearn();
    else
    {
        // Default to the first knob of the current bank.
        const int firstInBank = mapper->getCurrentBank() * ParameterMapperNode::kKnobsPerBank;
        mapper->beginLearn (firstInBank);
    }
}

} // namespace element
