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
ParameterMapperEditor::ParameterMapperEditor (const Node& node)
    : NodeEditor (node)
{
    mapper = dynamic_cast<ParameterMapperNode*> (node.getObject());

    addAndMakeVisible (learnButton);
    learnButton.setClickingTogglesState (false);
    learnButton.onClick = [this]() { toggleLearnMode(); };

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
        mapper->addStateListener (this);
    }

    setSize (480, 380);
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
    learnButton.setBounds (topRow.removeFromLeft (110));
    topRow.removeFromLeft (12);
    const int bankW = juce::jmin (44, topRow.getWidth() / juce::jmax (1, bankButtons.size()));
    for (auto* b : bankButtons)
        b->setBounds (topRow.removeFromLeft (bankW));

    r.removeFromTop (6);
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

    // Rebind cells to the current bank's slot range.
    for (int i = 0; i < knobs.size(); ++i)
    {
        const int absolute = bank * ParameterMapperNode::kKnobsPerBank + i;
        if (knobs[i]->getSlot() != absolute)
            knobs[i]->setSlot (absolute);
        else
            knobs[i]->refresh();
    }
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
