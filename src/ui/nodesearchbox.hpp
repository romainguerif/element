// Copyright 2024 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/audio_processors.hpp>
#include <element/juce/gui_basics.hpp>

namespace element {

/** A Spotlight-style overlay: hit Tab, type a node/plugin name, press Return to
    drop it into the active graph. Lives as a full-bounds child of the content
    component, dimming everything behind a centred search panel. */
class NodeSearchBox : public juce::Component,
                      public juce::KeyListener,
                      public juce::TextEditor::Listener,
                      public juce::ListBoxModel
{
public:
    struct Item
    {
        juce::String name;
        juce::String detail;
        juce::PluginDescription desc;
    };

    NodeSearchBox();
    ~NodeSearchBox() override;

    void setCatalog (juce::Array<Item> items);

    /** Called with the chosen plugin description when the user commits a row. */
    std::function<void (const juce::PluginDescription&)> onChoose;
    /** Called whenever the overlay closes itself. */
    std::function<void()> onDismiss;

    void show();
    void dismiss();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;

    void textEditorTextChanged (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;

private:
    juce::TextEditor search;
    juce::ListBox list;
    juce::Array<Item> catalog;
    juce::Array<int> filtered;

    void updateFilter();
    void moveSelection (int delta);
    void chooseRow (int row);
    juce::Rectangle<int> panelBounds() const;

    static int score (const juce::String& haystack, const juce::String& needle);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeSearchBox)
};

} // namespace element
