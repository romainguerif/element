// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/node.h>
#include <element/processor.hpp>

namespace element {

/** A sticky-note annotation node, inspired by Houdini FX's "Sticky Note"
    network markup.

    No audio or MIDI ports -- it exists purely as a labelled, coloured
    rectangle placed in the graph editor canvas to annotate intent or
    document a routing. State stored per-instance: title, body text,
    text size, sticker color.
*/
class NoteNode : public Processor
{
public:
    NoteNode();
    ~NoteNode() override;

    //==========================================================================
    void prepareToRender (double, int) override {}
    void releaseResources() override {}
    bool wantsContext() const noexcept override { return true; }
    void render (RenderContext&) override {} // no audio activity

    //==========================================================================
    juce::String getTitle() const { return title; }
    void setTitle (const juce::String&);

    juce::String getBody() const { return body; }
    void setBody (const juce::String&);

    float getTextSize() const noexcept { return textSize; }
    void setTextSize (float pts);

    juce::Colour getStickerColour() const noexcept { return stickerColour; }
    void setStickerColour (juce::Colour);

    //==========================================================================
    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int) override;

    void getPluginDescription (juce::PluginDescription&) const override;
    void refreshPorts() override;

    int getNumPrograms() const override { return 1; }
    int getCurrentProgram() const override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) const override { return "Note"; }

    void addStateListener (juce::ChangeListener* l) { listeners.add (l); }
    void removeStateListener (juce::ChangeListener* l) { listeners.remove (l); }

protected:
    void initialize() override {}

private:
    void notifyListeners();

    juce::String title { "Note" };
    juce::String body  { "" };
    float textSize { 13.0f };
    juce::Colour stickerColour { 0xfff9d976 }; // warm yellow, Houdini-ish

    juce::ListenerList<juce::ChangeListener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteNode)
};

} // namespace element
