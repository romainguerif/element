// Copyright 2024 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/ui/style.hpp>

#include "ui/nodesearchbox.hpp"

namespace element {

NodeSearchBox::NodeSearchBox()
    : list ("nodeSearchResults", this)
{
    setOpaque (false);

    search.setTextToShowWhenEmpty ("Search nodes…", juce::Colours::grey);
    search.setJustification (juce::Justification::centredLeft);
    search.setMultiLine (false);
    search.setReturnKeyStartsNewLine (false);
    search.setSelectAllWhenFocused (true);
    search.addListener (this);
    search.addKeyListener (this);
    addAndMakeVisible (search);

    list.setRowHeight (28);
    list.setMultipleSelectionEnabled (false);
    list.addKeyListener (this);
    addAndMakeVisible (list);
}

NodeSearchBox::~NodeSearchBox()
{
    search.removeListener (this);
    search.removeKeyListener (this);
    list.removeKeyListener (this);
}

void NodeSearchBox::setCatalog (juce::Array<Item> items)
{
    catalog = std::move (items);
    updateFilter();
}

void NodeSearchBox::show()
{
    search.setText (juce::String(), juce::dontSendNotification);
    updateFilter();
    setVisible (true);
    toFront (true);
    search.grabKeyboardFocus();
}

void NodeSearchBox::dismiss()
{
    setVisible (false);
    if (onDismiss)
        onDismiss();
}

juce::Rectangle<int> NodeSearchBox::panelBounds() const
{
    const int w = juce::jmin (560, getWidth() - 40);
    const int rows = juce::jlimit (1, 10, juce::jmax (1, filtered.size()));
    const int listH = rows * list.getRowHeight() + 4;
    const int h = 44 + listH;
    return { (getWidth() - w) / 2, getHeight() / 6, w, h };
}

void NodeSearchBox::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.45f));

    const auto pb = panelBounds().toFloat();
    g.setColour (findColour (Style::widgetBackgroundColorId).darker (0.2f));
    g.fillRoundedRectangle (pb, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (pb.reduced (0.5f), 8.0f, 1.0f);
}

void NodeSearchBox::resized()
{
    auto pb = panelBounds();
    search.setBounds (pb.removeFromTop (44).reduced (8, 6));
    list.setBounds (pb.reduced (6, 0).withTrimmedBottom (4));
}

void NodeSearchBox::mouseDown (const juce::MouseEvent& e)
{
    // Clicking outside the panel dismisses, just like Spotlight.
    if (! panelBounds().contains (e.getPosition()))
        dismiss();
}

bool NodeSearchBox::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (key == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        chooseRow (list.getSelectedRow());
        return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        moveSelection (-1);
        return true;
    }
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::tabKey)
    {
        moveSelection (1);
        return true;
    }
    if (key == juce::KeyPress::pageUpKey)
    {
        moveSelection (-8);
        return true;
    }
    if (key == juce::KeyPress::pageDownKey)
    {
        moveSelection (8);
        return true;
    }
    return false;
}

void NodeSearchBox::moveSelection (int delta)
{
    if (filtered.isEmpty())
        return;
    const int cur = juce::jmax (0, list.getSelectedRow());
    const int next = juce::jlimit (0, filtered.size() - 1, cur + delta);
    list.selectRow (next);
}

void NodeSearchBox::textEditorTextChanged (juce::TextEditor&)
{
    updateFilter();
}

void NodeSearchBox::textEditorReturnKeyPressed (juce::TextEditor&)
{
    chooseRow (list.getSelectedRow());
}

void NodeSearchBox::textEditorEscapeKeyPressed (juce::TextEditor&)
{
    dismiss();
}

void NodeSearchBox::chooseRow (int row)
{
    if (! juce::isPositiveAndBelow (row, filtered.size()))
        return;
    const auto& item = catalog.getReference (filtered.getUnchecked (row));
    auto cb = onChoose;
    auto desc = item.desc;
    dismiss();
    if (cb)
        cb (desc);
}

int NodeSearchBox::score (const juce::String& haystack, const juce::String& needle)
{
    if (needle.isEmpty())
        return 1;

    const auto h = haystack.toLowerCase();
    const auto n = needle.toLowerCase();
    if (! h.contains (n))
        return 0;

    int s = 1;
    if (h.startsWith (n))
        s += 100;
    else if (h.contains (" " + n))
        s += 50;
    s += juce::jmax (0, 30 - (h.length() - n.length()));
    return s;
}

void NodeSearchBox::updateFilter()
{
    const auto needle = search.getText().trim();

    struct Scored { int index; int s; juce::String name; };
    juce::Array<Scored> scored;
    for (int i = 0; i < catalog.size(); ++i)
    {
        const auto& it = catalog.getReference (i);
        const int s = juce::jmax (score (it.name, needle),
                                  score (it.detail, needle));
        if (s > 0)
            scored.add ({ i, s, it.name });
    }

    std::sort (scored.begin(), scored.end(), [] (const Scored& a, const Scored& b) {
        if (a.s != b.s)
            return a.s > b.s;
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    filtered.clearQuick();
    for (const auto& sc : scored)
        filtered.add (sc.index);

    list.updateContent();
    if (! filtered.isEmpty())
        list.selectRow (0);

    resized();
    repaint();
}

int NodeSearchBox::getNumRows()
{
    return filtered.size();
}

void NodeSearchBox::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, filtered.size()))
        return;

    const auto& item = catalog.getReference (filtered.getUnchecked (row));

    if (selected)
    {
        g.setColour (findColour (Style::backgroundHighlightColorId));
        g.fillRect (0, 0, w, h);
    }

    auto r = juce::Rectangle<int> (0, 0, w, h).reduced (8, 0);
    g.setColour (findColour (Style::textColorId));
    g.setFont (juce::Font (14.0f));
    g.drawText (item.name, r.removeFromLeft (w / 2), juce::Justification::centredLeft, true);

    g.setColour (findColour (Style::textColorId).withAlpha (0.55f));
    g.setFont (juce::Font (12.0f));
    g.drawText (item.detail, r, juce::Justification::centredRight, true);
}

void NodeSearchBox::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
}

void NodeSearchBox::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    chooseRow (row);
}

void NodeSearchBox::returnKeyPressed (int row)
{
    chooseRow (row);
}

} // namespace element
