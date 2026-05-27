// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/note.hpp"

#include <element/portcount.hpp>

namespace element {

NoteNode::NoteNode()
    : Processor (0)
{
    setName ("Note");
    refreshPorts();
}

NoteNode::~NoteNode() = default;

//==============================================================================
void NoteNode::setTitle (const juce::String& s)
{
    if (s == title) return;
    title = s;
    notifyListeners();
}

void NoteNode::setBody (const juce::String& s)
{
    if (s == body) return;
    body = s;
    notifyListeners();
}

void NoteNode::setTextSize (float pts)
{
    pts = juce::jlimit (8.0f, 96.0f, pts);
    if (juce::approximatelyEqual (pts, textSize)) return;
    textSize = pts;
    notifyListeners();
}

void NoteNode::setStickerColour (juce::Colour c)
{
    if (c == stickerColour) return;
    stickerColour = c;
    notifyListeners();
}

//==============================================================================
void NoteNode::getState (juce::MemoryBlock& dest)
{
    juce::ValueTree v ("Note");
    v.setProperty ("title", title, nullptr);
    v.setProperty ("body", body, nullptr);
    v.setProperty ("textSize", (double) textSize, nullptr);
    v.setProperty ("colour", stickerColour.toString(), nullptr);

    juce::MemoryOutputStream mos (dest, false);
    v.writeToStream (mos);
}

void NoteNode::setState (const void* data, int sizeInBytes)
{
    auto v = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! v.isValid() || ! v.hasType ("Note"))
        return;

    title = v.getProperty ("title", title);
    body = v.getProperty ("body", body);
    textSize = (float) (double) v.getProperty ("textSize", (double) textSize);
    stickerColour = juce::Colour::fromString (v.getProperty ("colour", stickerColour.toString()).toString());
    notifyListeners();
}

//==============================================================================
void NoteNode::getPluginDescription (juce::PluginDescription& desc) const
{
    desc.fileOrIdentifier = EL_NODE_ID_NOTE;
    desc.uniqueId = EL_NODE_UID_NOTE;
    desc.name = "Note";
    desc.descriptiveName = "Sticky note annotation";
    desc.numInputChannels = 0;
    desc.numOutputChannels = 0;
    desc.hasSharedContainer = false;
    desc.isInstrument = false;
    desc.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.version = "1.0.0";
}

void NoteNode::refreshPorts()
{
    if (getNumPorts() > 0)
        return;
    setPorts (PortList()); // no I/O at all
}

void NoteNode::notifyListeners()
{
    listeners.call ([] (juce::ChangeListener& l) { l.changeListenerCallback (nullptr); });
}

} // namespace element
