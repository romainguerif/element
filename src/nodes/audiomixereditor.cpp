// Copyright 2026 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audiomixereditor.hpp"

#include <cmath>

using namespace juce;

namespace element {

namespace {

constexpr int kStripWidth = 78;
constexpr int kReturnWidth = 64;
constexpr int kMasterWidth = 96;
constexpr int kGutter     = 4;
constexpr int kRowH       = 18;
constexpr int kKnobH      = 36;
constexpr int kKnobLabelH = 10;
constexpr int kLevelKnobH = 56;   // LEVEL is bigger and gets an LED ring

const Colour kBg          (0xff0d0d0d);
const Colour kStripBg     (0xff141414);
const Colour kStripBorder (0xff222222);
const Colour kAccent      (0xfffafafa);
const Colour kMuted       (0xff666666);

inline String dBString (float linear, float floorDb = -90.0f)
{
    const float dB = Decibels::gainToDecibels (linear, floorDb);
    return (dB <= floorDb + 0.1f) ? String ("-inf") : (String (dB, 1) + " dB");
}

} // namespace

//==============================================================================
MixerKnobLAF::MixerKnobLAF()
{
    setColour (Slider::rotarySliderFillColourId, kAccent);
    setColour (Slider::rotarySliderOutlineColourId, Colour (0xff2a2a2a));
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxTextColourId, kAccent);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
}

void MixerKnobLAF::drawRotarySlider (Graphics& g, int x, int y, int w, int h,
                                     float sliderPos, float startA, float endA,
                                     Slider& s)
{
    const float r = (float) jmin (w, h) * 0.5f - 4.0f;
    const float cx = (float) x + (float) w * 0.5f;
    const float cy = (float) y + (float) h * 0.5f;
    const float angle = startA + sliderPos * (endA - startA);

    // Outer ring background.
    g.setColour (Colour (0xff1a1a1a));
    g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);

    // Track arc (faint).
    Path track;
    track.addCentredArc (cx, cy, r - 1.0f, r - 1.0f, 0.0f, startA, endA, true);
    g.setColour (Colour (0xff2a2a2a));
    g.strokePath (track, PathStrokeType (1.6f));

    // Value arc.
    Path value;
    const float centerA = startA + (endA - startA) * 0.5f;
    const float fromA = s.getProperties()["bipolar"] ? centerA : startA;
    value.addCentredArc (cx, cy, r - 1.0f, r - 1.0f, 0.0f, fromA, angle, true);
    g.setColour (s.isEnabled() ? kAccent : kMuted);
    g.strokePath (value, PathStrokeType (1.8f));

    // Pointer.
    Path ptr;
    const float pLen = r - 4.0f;
    ptr.addRectangle (-1.0f, -pLen, 2.0f, pLen * 0.65f);
    g.setColour (kAccent);
    g.fillPath (ptr, AffineTransform::rotation (angle).translated (cx, cy));
}

Font MixerKnobLAF::getLabelFont (Label&) { return Font (FontOptions (10.5f)); }

//==============================================================================
double BipolarSnapSlider::snapValue (double attempted, DragMode dragMode)
{
    if (dragMode == notDragging)
        return attempted;
    // Snap window = 3% of the full range. Wide enough to feel a detent,
    // narrow enough to not interfere with deliberately small values.
    const double range  = getMaximum() - getMinimum();
    const double window = range * 0.015;   // ±1.5 % each side
    return (std::abs (attempted) <= window) ? 0.0 : attempted;
}

//==============================================================================
LedMeter::LedMeter()
{
    setOpaque (false);
    startTimerHz (30);
}

void LedMeter::timerCallback()
{
    if (! source)
        return;
    const float v = source();
    // Visual ballistics: fast attack, ~250ms release.
    displayedLevel = v > displayedLevel ? v : displayedLevel + (v - displayedLevel) * 0.15f;
    repaint();
}

void LedMeter::paint (Graphics& g)
{
    const int segs = 14;
    auto r = getLocalBounds().reduced (1);
    const int hPer = r.getHeight() / segs;
    const float dB = Decibels::gainToDecibels (jmax (1.0e-6f, displayedLevel), -90.0f);
    // Map -60..+3 dB to segments 0..segs-1.
    const float t = jlimit (0.0f, 1.0f, (dB + 60.0f) / 63.0f);
    const int lit = (int) std::round (t * segs);

    for (int i = 0; i < segs; ++i)
    {
        Colour c;
        if (i >= segs - 2)       c = Colour (0xffff3030);   // top: red
        else if (i >= segs - 5)  c = Colour (0xffffa030);   // upper: amber
        else                     c = Colour (0xff60c060);   // mid/low: green
        const bool on = i < lit;
        auto seg = Rectangle<int> (r.getX(), r.getBottom() - (i + 1) * hPer, r.getWidth(), hPer - 1);
        g.setColour (on ? c : c.withAlpha (0.12f));
        g.fillRect (seg);
    }
}

//==============================================================================
LevelKnob::LevelKnob (MixerKnobLAF& laf)
    : lookAndFeelRef (laf)
{
    addAndMakeVisible (knob);
    knob.setLookAndFeel (&laf);
    knob.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    setOpaque (false);
    startTimerHz (30);
}

LevelKnob::~LevelKnob()
{
    knob.setLookAndFeel (nullptr);
}

void LevelKnob::timerCallback()
{
    if (! source)
        return;
    const float v = source();
    // Fast attack, ~250 ms release ballistics.
    displayedLevel = v > displayedLevel ? v : displayedLevel + (v - displayedLevel) * 0.15f;
    repaint();
}

void LevelKnob::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float rOuter = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 2.0f;
    const float rInner = rOuter - 5.0f;

    // Same arc range the knob LAF uses (1.25*pi to 2.75*pi).
    const float startA = juce::MathConstants<float>::pi * 1.25f;
    const float endA   = juce::MathConstants<float>::pi * 2.75f;

    // Faint track behind the LEDs.
    Path track;
    track.addCentredArc (cx, cy, (rOuter + rInner) * 0.5f, (rOuter + rInner) * 0.5f,
                         0.0f, startA, endA, true);
    g.setColour (Colour (0xff202020));
    g.strokePath (track, PathStrokeType (4.0f));

    // 24 segments around the arc, colored by level.
    const int   segs = 24;
    const float dB   = Decibels::gainToDecibels (juce::jmax (1.0e-6f, displayedLevel), -90.0f);
    // Map -60..+3 dB to 0..segs-1.
    const float t   = juce::jlimit (0.0f, 1.0f, (dB + 60.0f) / 63.0f);
    const int   lit = (int) std::round (t * segs);

    for (int i = 0; i < segs; ++i)
    {
        const float frac = (float) i / (float) (segs - 1);
        const float a    = startA + frac * (endA - startA);
        const float dotX = cx + std::cos (a) * (rOuter - 2.0f);
        const float dotY = cy + std::sin (a) * (rOuter - 2.0f);

        Colour c;
        if (i >= segs - 3)       c = Colour (0xffff3030); // top red
        else if (i >= segs - 7)  c = Colour (0xffffa030); // amber
        else                     c = Colour (0xff60c060); // green

        const bool on = i < lit;
        g.setColour (on ? c : c.withAlpha (0.10f));
        g.fillEllipse (dotX - 1.8f, dotY - 1.8f, 3.6f, 3.6f);
    }
}

void LevelKnob::resized()
{
    // Knob inset so the LED ring fits around it.
    auto b = getLocalBounds().reduced (8);
    knob.setBounds (b);
}

void LevelKnob::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click on the LED ring area (outside the slider) should still
    // reset the underlying slider. Forward the event explicitly.
    juce::ignoreUnused (e);
    knob.setValue (knob.getDoubleClickReturnValue(), juce::sendNotificationSync);
}

//==============================================================================
class AudioMixerEditor::ChannelStrip : public Component
{
public:
    ChannelStrip (AudioMixerProcessor::Channel& ch, MixerKnobLAF& laf)
        : channel (ch), gainLevelKnob (laf)
    {
        addAndMakeVisible (gainLevelKnob);
        gainLevelKnob.setLevelSource ([this] {
            return juce::jmax (channel.rmsL.load (std::memory_order_relaxed),
                               channel.rmsR.load (std::memory_order_relaxed));
        });
        setOpaque (true);

        addAndMakeVisible (nameLabel);
        nameLabel.setText (channel.name, dontSendNotification);
        nameLabel.setJustificationType (Justification::centred);
        nameLabel.setFont (Font (FontOptions (10.5f, Font::bold)));
        nameLabel.setColour (Label::textColourId, kAccent);
        nameLabel.setEditable (false, true, false);
        nameLabel.onTextChange = [this]{ channel.name = nameLabel.getText(); };

        auto setupKnob = [&] (Slider& k, float min, float max, float def, bool bipolar)
        {
            addAndMakeVisible (k);
            k.setLookAndFeel (&laf);
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
            k.setRange (min, max, 0.0);
            k.setValue (def, dontSendNotification);
            k.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);
            // Double-click on the knob returns it to its design default.
            k.setDoubleClickReturnValue (true, def);
            if (bipolar)
                k.getProperties().set ("bipolar", true);
        };

        // gainLevelKnob's internal slider — set up like the others. The
        // LookAndFeel is already attached by LevelKnob's constructor.
        auto& gk = gainLevelKnob.slider();
        gk.setRange (-90.0, 12.0, 0.0);
        gk.setValue (0.0, dontSendNotification);
        gk.setSkewFactorFromMidPoint (-12.0);
        gk.setDoubleClickReturnValue (true, 0.0);   // 0 dB reset
        gk.onValueChange = [this] {
            channel.gainTarget.store (Decibels::decibelsToGain (
                (float) gainLevelKnob.slider().getValue(), -90.0f));
        };

        for (auto& k : { &eqHigh, &eqMid, &eqLow })
            setupKnob (*k, -1.0f, 1.0f, 0.0f, true);
        eqHigh.onValueChange = [this] { channel.eqHighTarget.store ((float) eqHigh.getValue()); };
        eqMid.onValueChange  = [this] { channel.eqMidTarget.store  ((float) eqMid.getValue()); };
        eqLow.onValueChange  = [this] { channel.eqLowTarget.store  ((float) eqLow.getValue()); };

        setupKnob (filterFreq, 20.0f, 20000.0f, 1000.0f, false);
        filterFreq.setSkewFactorFromMidPoint (1000.0f);
        filterFreq.onValueChange = [this] { channel.filterFreqTarget.store ((float) filterFreq.getValue()); };

        setupKnob (filterReso, 0.0f, 1.0f, 0.5f, false);
        filterReso.onValueChange = [this] { channel.filterResoTarget.store ((float) filterReso.getValue()); };

        addAndMakeVisible (filterMode);
        filterMode.addItem ("LP", 1);
        filterMode.addItem ("--", 2);  // bypass = "--"
        filterMode.addItem ("HP", 3);
        filterMode.setSelectedId (2, dontSendNotification);
        filterMode.setColour (ComboBox::textColourId, kAccent);
        filterMode.setColour (ComboBox::backgroundColourId, Colour (0xff1a1a1a));
        filterMode.setColour (ComboBox::outlineColourId, Colour (0xff2a2a2a));
        filterMode.onChange = [this] {
            // ComboBox IDs are 1..3 -> mode 0..2
            channel.filterModeTarget.store (filterMode.getSelectedId() - 1);
        };

        // Drive amount + Transient before sends so they show next to filter.
        setupKnob (driveAmount, 0.0f, 1.0f, 0.0f, false);
        driveAmount.onValueChange = [this] { channel.driveAmountTarget.store ((float) driveAmount.getValue()); };

        addAndMakeVisible (driveMode);
        driveMode.addItem ("Tape", 1);
        driveMode.addItem ("Tube", 2);
        driveMode.addItem ("Iron", 3);  // = Transformer
        driveMode.addItem ("Clip", 4);
        driveMode.setSelectedId (1, dontSendNotification);
        driveMode.setColour (ComboBox::textColourId, kAccent);
        driveMode.setColour (ComboBox::backgroundColourId, Colour (0xff1a1a1a));
        driveMode.setColour (ComboBox::outlineColourId, Colour (0xff2a2a2a));
        driveMode.onChange = [this] { channel.driveModeTarget.store (driveMode.getSelectedId() - 1); };

        setupKnob (transient, -1.0f, 1.0f, 0.0f, true);
        transient.onValueChange = [this] { channel.transientTarget.store ((float) transient.getValue()); };

        for (auto& k : { &send1, &send2, &send3 })
            setupKnob (*k, 0.0f, 1.0f, 0.0f, false);
        send1.onValueChange = [this] { channel.send1Target.store ((float) send1.getValue()); };
        send2.onValueChange = [this] { channel.send2Target.store ((float) send2.getValue()); };
        send3.onValueChange = [this] { channel.send3Target.store ((float) send3.getValue()); };

        setupKnob (panKnob, -1.0f, 1.0f, 0.0f, true);
        panKnob.onValueChange = [this] { channel.panTarget.store ((float) panKnob.getValue()); };

        for (auto* b : { &muteBtn, &soloBtn, &cueBtn })
        {
            addAndMakeVisible (*b);
            b->setClickingTogglesState (true);
            b->setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
            b->setColour (TextButton::buttonOnColourId, Colour (0xffe0e0e0));
            b->setColour (TextButton::textColourOnId, Colours::black);
            b->setColour (TextButton::textColourOffId, kAccent);
        }
        muteBtn.setButtonText ("M");
        soloBtn.setButtonText ("S");
        cueBtn.setButtonText ("C");
        muteBtn.onClick = [this] { channel.muteTarget.store (muteBtn.getToggleState()); };
        soloBtn.onClick = [this] { channel.soloTarget.store (soloBtn.getToggleState()); };
        cueBtn.onClick  = [this] { channel.cueTarget.store  (cueBtn.getToggleState()); };

        addAndMakeVisible (meterL);
        addAndMakeVisible (meterR);
        meterL.setLevelSource ([this] { return channel.rmsL.load (std::memory_order_relaxed); });
        meterR.setLevelSource ([this] { return channel.rmsR.load (std::memory_order_relaxed); });

        // Initial sync from channel state.
        gainLevelKnob.slider().setValue (
            Decibels::gainToDecibels (channel.gainTarget.load(), -90.0f),
            dontSendNotification);
        panKnob.setValue (channel.panTarget.load(), dontSendNotification);
        eqHigh.setValue (channel.eqHighTarget.load(), dontSendNotification);
        eqMid.setValue (channel.eqMidTarget.load(), dontSendNotification);
        eqLow.setValue (channel.eqLowTarget.load(), dontSendNotification);
        filterFreq.setValue (channel.filterFreqTarget.load(), dontSendNotification);
        filterReso.setValue (channel.filterResoTarget.load(), dontSendNotification);
        filterMode.setSelectedId (channel.filterModeTarget.load() + 1, dontSendNotification);
        send1.setValue (channel.send1Target.load(), dontSendNotification);
        send2.setValue (channel.send2Target.load(), dontSendNotification);
        send3.setValue (channel.send3Target.load(), dontSendNotification);
        driveAmount.setValue (channel.driveAmountTarget.load(), dontSendNotification);
        driveMode.setSelectedId (channel.driveModeTarget.load() + 1, dontSendNotification);
        transient.setValue (channel.transientTarget.load(), dontSendNotification);
        muteBtn.setToggleState (channel.muteTarget.load(), dontSendNotification);
        soloBtn.setToggleState (channel.soloTarget.load(), dontSendNotification);
        cueBtn.setToggleState  (channel.cueTarget.load(),  dontSendNotification);
    }

    ~ChannelStrip() override
    {
        // Mixed Slider / BipolarSnapSlider in a single list — initializer
        // lists can't deduce a common type, so reference each Slider base
        // explicitly via static_cast.
        for (Slider* s : { static_cast<Slider*> (&panKnob),
                           static_cast<Slider*> (&eqHigh),
                           static_cast<Slider*> (&eqMid),
                           static_cast<Slider*> (&eqLow),
                           static_cast<Slider*> (&filterFreq),
                           static_cast<Slider*> (&filterReso),
                           static_cast<Slider*> (&transient),
                           static_cast<Slider*> (&driveAmount),
                           static_cast<Slider*> (&send1),
                           static_cast<Slider*> (&send2),
                           static_cast<Slider*> (&send3) })
            s->setLookAndFeel (nullptr);
        // gainLevelKnob owns its slider; ~LevelKnob() detaches the LAF.
    }

    void paint (Graphics& g) override
    {
        g.fillAll (kStripBg);
        g.setColour (kStripBorder);
        g.drawRect (getLocalBounds(), 1);
    }

    void labelAbove (Graphics& g, Rectangle<int> r, const String& s)
    {
        g.setColour (kAccent.withAlpha (0.55f));
        g.setFont (Font (FontOptions (9.5f)));
        g.drawText (s, r, Justification::centred);
    }

    void paintOverChildren (Graphics& g) override
    {
        // Section labels are drawn over the strip — gives the analog look.
        auto draw = [&] (Component& c, const String& lbl) {
            auto b = c.getBounds();
            labelAbove (g, { b.getX(), b.getY() - 12, b.getWidth(), 11 }, lbl);
        };
        draw (transient,   "DECAY");
        draw (eqHigh,      "HI");
        draw (eqMid,       "MID");
        draw (eqLow,       "LOW");
        draw (filterFreq,  "FREQ");
        draw (filterReso,  "RES");
        draw (driveAmount, "DRIVE");
        draw (send1,       "FX 1");
        draw (send2,       "FX 2");
        draw (send3,       "FX 3");
        draw (panKnob,     "PAN");
        draw (gainLevelKnob, "LEVEL");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        nameLabel.setBounds (r.removeFromTop (16));
        r.removeFromTop (4);

        auto knob = [&] (Component& k) {
            r.removeFromTop (kKnobLabelH);  // label space
            k.setBounds (r.removeFromTop (kKnobH));
            r.removeFromTop (1);
        };

        knob (transient);
        r.removeFromTop (3);
        knob (eqHigh);
        knob (eqMid);
        knob (eqLow);
        r.removeFromTop (3);
        knob (filterFreq);
        knob (filterReso);
        filterMode.setBounds (r.removeFromTop (16));
        r.removeFromTop (3);
        knob (driveAmount);
        driveMode.setBounds (r.removeFromTop (16));
        r.removeFromTop (3);
        knob (send1);
        knob (send2);
        knob (send3);
        r.removeFromTop (3);
        knob (panKnob);
        // LEVEL is the prominent one — taller and with the LED ring overlay.
        r.removeFromTop (kKnobLabelH);
        gainLevelKnob.setBounds (r.removeFromTop (kLevelKnobH));
        r.removeFromTop (3);

        auto btnRow = r.removeFromTop (18);
        const int bw = btnRow.getWidth() / 3;
        muteBtn.setBounds (btnRow.removeFromLeft (bw).reduced (1));
        soloBtn.setBounds (btnRow.removeFromLeft (bw).reduced (1));
        cueBtn.setBounds  (btnRow.reduced (1));

        r.removeFromTop (4);
        auto meterArea = r;
        const int mw = meterArea.getWidth() / 2;
        meterL.setBounds (meterArea.removeFromLeft (mw - 1));
        meterR.setBounds (meterArea.withTrimmedLeft (1));
    }

private:
    AudioMixerProcessor::Channel& channel;
    Label  nameLabel;
    LevelKnob gainLevelKnob;
    BipolarSnapSlider panKnob;
    BipolarSnapSlider eqHigh, eqMid, eqLow;
    Slider filterFreq, filterReso;
    ComboBox filterMode;
    BipolarSnapSlider transient;
    Slider driveAmount;
    ComboBox driveMode;
    Slider send1, send2, send3;
    TextButton muteBtn, soloBtn, cueBtn;
    LedMeter meterL, meterR;
};

//==============================================================================
class AudioMixerEditor::ReturnStrip : public Component
{
public:
    ReturnStrip (AudioMixerProcessor::Return& r, int index, MixerKnobLAF& laf)
        : ret (r)
    {
        setOpaque (true);
        addAndMakeVisible (label);
        label.setText ("RET " + String (index + 1), dontSendNotification);
        label.setJustificationType (Justification::centred);
        label.setFont (Font (FontOptions (10.5f, Font::bold)));
        label.setColour (Label::textColourId, kAccent);

        addAndMakeVisible (level);
        level.setLookAndFeel (&laf);
        level.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        level.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        level.setRange (-90.0, 12.0, 0.0);
        level.setSkewFactorFromMidPoint (-12.0);
        level.setDoubleClickReturnValue (true, 0.0);  // 0 dB reset
        level.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);
        level.setValue (Decibels::gainToDecibels (ret.levelTarget.load(), -90.0f), dontSendNotification);
        level.onValueChange = [this] {
            ret.levelTarget.store (Decibels::decibelsToGain ((float) level.getValue(), -90.0f));
        };

        addAndMakeVisible (muteBtn);
        muteBtn.setClickingTogglesState (true);
        muteBtn.setButtonText ("M");
        muteBtn.setColour (TextButton::buttonOnColourId, Colour (0xffe0e0e0));
        muteBtn.setColour (TextButton::textColourOnId, Colours::black);
        muteBtn.setColour (TextButton::textColourOffId, kAccent);
        muteBtn.setToggleState (ret.muteTarget.load(), dontSendNotification);
        muteBtn.onClick = [this] { ret.muteTarget.store (muteBtn.getToggleState()); };

        addAndMakeVisible (meterL);
        addAndMakeVisible (meterR);
        meterL.setLevelSource ([this] { return ret.rmsL.load (std::memory_order_relaxed); });
        meterR.setLevelSource ([this] { return ret.rmsR.load (std::memory_order_relaxed); });
    }

    ~ReturnStrip() override { level.setLookAndFeel (nullptr); }

    void paint (Graphics& g) override
    {
        g.fillAll (kStripBg);
        g.setColour (kStripBorder);
        g.drawRect (getLocalBounds(), 1);
    }
    void paintOverChildren (Graphics& g) override
    {
        g.setColour (kAccent.withAlpha (0.55f));
        g.setFont (Font (FontOptions (9.5f)));
        g.drawText ("LEVEL", level.getX(), level.getY() - 12, level.getWidth(), 11,
                    Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        label.setBounds (r.removeFromTop (16));
        r.removeFromTop (16);
        level.setBounds (r.removeFromTop (kKnobH));
        r.removeFromTop (6);
        muteBtn.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);
        auto m = r;
        const int mw = m.getWidth() / 2;
        meterL.setBounds (m.removeFromLeft (mw - 1));
        meterR.setBounds (m.withTrimmedLeft (1));
    }

private:
    AudioMixerProcessor::Return& ret;
    Label label;
    Slider level;
    TextButton muteBtn;
    LedMeter meterL, meterR;
};

//==============================================================================
class AudioMixerEditor::MasterStrip : public Component
{
public:
    MasterStrip (AudioMixerProcessor::Master& m, MixerKnobLAF& laf)
        : master (m)
    {
        setOpaque (true);
        addAndMakeVisible (label);
        label.setText ("MASTER", dontSendNotification);
        label.setJustificationType (Justification::centred);
        label.setFont (Font (FontOptions (11.0f, Font::bold)));
        label.setColour (Label::textColourId, kAccent);

        auto setupKnob = [&] (Slider& k, float lo, float hi, float def, bool bipolar) {
            addAndMakeVisible (k);
            k.setLookAndFeel (&laf);
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
            k.setRange (lo, hi, 0.0);
            k.setValue (def, dontSendNotification);
            k.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);
            k.setDoubleClickReturnValue (true, def);
            if (bipolar) k.getProperties().set ("bipolar", true);
        };

        for (auto& k : { &isoHigh, &isoMid, &isoLow })
            setupKnob (*k, -1.0f, 1.0f, 0.0f, true);
        isoHigh.onValueChange = [this] { master.isoHighTarget.store ((float) isoHigh.getValue()); };
        isoMid.onValueChange  = [this] { master.isoMidTarget.store  ((float) isoMid.getValue()); };
        isoLow.onValueChange  = [this] { master.isoLowTarget.store  ((float) isoLow.getValue()); };

        setupKnob (gain, -90.0f, 12.0f, 0.0f, false);
        gain.setSkewFactorFromMidPoint (-12.0f);
        gain.onValueChange = [this] {
            master.gainTarget.store (Decibels::decibelsToGain ((float) gain.getValue(), -90.0f));
        };

        setupKnob (booth, -90.0f, 12.0f, 0.0f, false);
        booth.setSkewFactorFromMidPoint (-12.0f);
        booth.onValueChange = [this] {
            master.boothTarget.store (Decibels::decibelsToGain ((float) booth.getValue(), -90.0f));
        };

        addAndMakeVisible (muteBtn);
        muteBtn.setClickingTogglesState (true);
        muteBtn.setButtonText ("MUTE");
        muteBtn.setColour (TextButton::buttonOnColourId, Colour (0xffff3030));
        muteBtn.setColour (TextButton::textColourOnId, Colours::white);
        muteBtn.setColour (TextButton::textColourOffId, kAccent);
        muteBtn.onClick = [this] { master.muteTarget.store (muteBtn.getToggleState()); };

        addAndMakeVisible (meterL);
        addAndMakeVisible (meterR);
        meterL.setLevelSource ([this] { return master.rmsL.load (std::memory_order_relaxed); });
        meterR.setLevelSource ([this] { return master.rmsR.load (std::memory_order_relaxed); });

        // Sync from state.
        gain.setValue (Decibels::gainToDecibels (master.gainTarget.load(), -90.0f), dontSendNotification);
        booth.setValue (Decibels::gainToDecibels (master.boothTarget.load(), -90.0f), dontSendNotification);
        muteBtn.setToggleState (master.muteTarget.load(), dontSendNotification);
        isoHigh.setValue (master.isoHighTarget.load(), dontSendNotification);
        isoMid.setValue (master.isoMidTarget.load(), dontSendNotification);
        isoLow.setValue (master.isoLowTarget.load(), dontSendNotification);
    }

    ~MasterStrip() override
    {
        for (Slider* s : { static_cast<Slider*> (&isoHigh),
                           static_cast<Slider*> (&isoMid),
                           static_cast<Slider*> (&isoLow),
                           static_cast<Slider*> (&gain),
                           static_cast<Slider*> (&booth) })
            s->setLookAndFeel (nullptr);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (kStripBg);
        g.setColour (kStripBorder);
        g.drawRect (getLocalBounds(), 1);
    }
    void paintOverChildren (Graphics& g) override
    {
        g.setColour (kAccent.withAlpha (0.55f));
        g.setFont (Font (FontOptions (9.5f)));
        auto lbl = [&] (Component& c, const String& s) {
            auto b = c.getBounds();
            g.drawText (s, b.getX(), b.getY() - 12, b.getWidth(), 11, Justification::centred);
        };
        lbl (isoHigh, "ISO HI");
        lbl (isoMid,  "ISO MID");
        lbl (isoLow,  "ISO LOW");
        lbl (gain,    "MASTER");
        lbl (booth,   "BOOTH");
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        label.setBounds (r.removeFromTop (16));
        r.removeFromTop (8);

        auto knob = [&] (Slider& k) {
            r.removeFromTop (12);
            k.setBounds (r.removeFromTop (kKnobH + 4));
            r.removeFromTop (4);
        };
        knob (isoHigh);
        knob (isoMid);
        knob (isoLow);
        r.removeFromTop (4);
        knob (gain);
        knob (booth);
        r.removeFromTop (4);
        muteBtn.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        auto m = r;
        const int mw = m.getWidth() / 2;
        meterL.setBounds (m.removeFromLeft (mw - 1));
        meterR.setBounds (m.withTrimmedLeft (1));
    }

private:
    AudioMixerProcessor::Master& master;
    Label label;
    BipolarSnapSlider isoHigh, isoMid, isoLow;
    Slider gain, booth;
    TextButton muteBtn;
    LedMeter meterL, meterR;
};

//==============================================================================
class AudioMixerEditor::RecorderBar : public juce::Component,
                                      public juce::ChangeListener,
                                      private juce::Timer
{
public:
    explicit RecorderBar (AudioRecorderNode& r) : rec (r)
    {
        setOpaque (true);

        addAndMakeVisible (recBtn);
        recBtn.setClickingTogglesState (false);
        recBtn.setButtonText ("REC");
        recBtn.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
        recBtn.setColour (TextButton::buttonOnColourId, Colour (0xffff2030));
        recBtn.setColour (TextButton::textColourOnId,  Colours::white);
        recBtn.setColour (TextButton::textColourOffId, Colours::white);
        recBtn.onClick = [this] {
            if (rec.isRecording())
                rec.stopRecording();
            else
                rec.startRecording (rec.getDestinationDirectory());
            stabilize();
        };

        addAndMakeVisible (timeLabel);
        timeLabel.setFont (Font (FontOptions (16.0f, Font::bold)));
        timeLabel.setColour (Label::textColourId, Colours::white);
        timeLabel.setJustificationType (Justification::centredLeft);
        timeLabel.setText ("00:00:00", dontSendNotification);

        addAndMakeVisible (folderLabel);
        folderLabel.setFont (Font (FontOptions (10.5f)));
        folderLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.65f));
        folderLabel.setJustificationType (Justification::centredLeft);

        addAndMakeVisible (advancedToggle);
        advancedToggle.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));  // ▾
        advancedToggle.setClickingTogglesState (true);
        advancedToggle.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
        advancedToggle.setColour (TextButton::textColourOffId, Colours::white);
        advancedToggle.onClick = [this] {
            expanded = advancedToggle.getToggleState();
            advancedToggle.setButtonText (expanded ? juce::String::fromUTF8 ("\xe2\x96\xb4")  // ▴
                                                   : juce::String::fromUTF8 ("\xe2\x96\xbe"));// ▾
            stabilize();
            if (auto* parent = getParentComponent())
                parent->resized();
        };

        // Advanced controls (initially hidden).
        addChildComponent (pathLabel);
        pathLabel.setFont (Font (FontOptions (10.5f)));
        pathLabel.setColour (Label::textColourId, Colours::white);
        pathLabel.setJustificationType (Justification::centredLeft);
        pathLabel.setEditable (false, false, false);

        addChildComponent (browseBtn);
        browseBtn.setButtonText ("...");
        browseBtn.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
        browseBtn.setColour (TextButton::textColourOffId, Colours::white);
        browseBtn.onClick = [this] { pickFolder(); };

        addChildComponent (formatCombo);
        formatCombo.addItem ("24-bit", 1);
        formatCombo.addItem ("32-float", 2);
        formatCombo.setSelectedId (rec.getBitDepth() == AudioRecorderNode::BitDepth::Float32 ? 2 : 1,
                                   dontSendNotification);
        formatCombo.setColour (ComboBox::textColourId, Colours::white);
        formatCombo.setColour (ComboBox::backgroundColourId, Colour (0xff1a1a1a));
        formatCombo.setColour (ComboBox::outlineColourId, Colour (0xff2a2a2a));
        formatCombo.onChange = [this] {
            rec.setBitDepth (formatCombo.getSelectedId() == 2
                                 ? AudioRecorderNode::BitDepth::Float32
                                 : AudioRecorderNode::BitDepth::Int24);
        };

        addChildComponent (modeCombo);
        modeCombo.addItem ("One WAV per stem", 1);
        modeCombo.addItem ("Single multichannel", 2);
        modeCombo.setSelectedId (
            rec.getFileMode() == AudioRecorderNode::FileMode::OneMultichannelFile ? 2 : 1,
            dontSendNotification);
        modeCombo.setColour (ComboBox::textColourId, Colours::white);
        modeCombo.setColour (ComboBox::backgroundColourId, Colour (0xff1a1a1a));
        modeCombo.setColour (ComboBox::outlineColourId, Colour (0xff2a2a2a));
        modeCombo.onChange = [this] {
            rec.setFileMode (modeCombo.getSelectedId() == 2
                                 ? AudioRecorderNode::FileMode::OneMultichannelFile
                                 : AudioRecorderNode::FileMode::OneFilePerStereoPair);
        };

        rec.addStateListener (this);
        stabilize();
        startTimerHz (10);
    }

    ~RecorderBar() override
    {
        rec.removeStateListener (this);
    }

    bool isExpanded() const { return expanded; }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { stabilize(); }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff0a0a0a));
        g.setColour (Colour (0xff2a2a2a));
        g.drawHorizontalLine (0, 0, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        // Top row: REC + time + folder + advanced toggle
        auto top = r.removeFromTop (32);
        recBtn.setBounds (top.removeFromLeft (60));
        top.removeFromLeft (10);
        timeLabel.setBounds (top.removeFromLeft (80));
        top.removeFromLeft (10);
        advancedToggle.setBounds (top.removeFromRight (28));
        top.removeFromRight (6);
        folderLabel.setBounds (top);   // takes remaining space

        // Expanded row: path / browse / format / mode
        if (expanded)
        {
            r.removeFromTop (4);
            auto adv = r.removeFromTop (24);
            browseBtn.setBounds  (adv.removeFromLeft (32));
            adv.removeFromLeft (4);
            modeCombo.setBounds   (adv.removeFromRight (160));
            adv.removeFromRight (6);
            formatCombo.setBounds (adv.removeFromRight (90));
            adv.removeFromRight (6);
            pathLabel.setBounds   (adv);
        }
    }

private:
    void timerCallback() override { stabilize(); }

    void stabilize()
    {
        const bool isRec = rec.isRecording();
        recBtn.setToggleState (isRec, dontSendNotification);
        recBtn.setButtonText (isRec ? "■ STOP" : "● REC");

        // Elapsed time — recorder knows its own sample rate.
        const int seconds = (int) rec.getElapsedSeconds();
        const int hh = seconds / 3600;
        const int mm = (seconds / 60) % 60;
        const int ss = seconds % 60;
        timeLabel.setText (juce::String::formatted ("%02d:%02d:%02d", hh, mm, ss), dontSendNotification);

        // Folder label
        const auto base = rec.getDestinationDirectory().getFileName();
        if (isRec && rec.getLastSessionFolder().isDirectory())
            folderLabel.setText ("→ " + rec.getLastSessionFolder().getFileName(), dontSendNotification);
        else
            folderLabel.setText (base.isEmpty() ? rec.getDestinationDirectory().getFullPathName() : base,
                                 dontSendNotification);
        pathLabel.setText (rec.getDestinationDirectory().getFullPathName(), dontSendNotification);

        // Show/hide advanced controls.
        for (auto* c : { (Component*) &pathLabel, (Component*) &browseBtn,
                         (Component*) &formatCombo, (Component*) &modeCombo })
            c->setVisible (expanded);
    }

    void pickFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose recordings folder",
                                                       rec.getDestinationDirectory());
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc) {
                if (fc.getResult().isDirectory())
                    rec.setDestinationDirectory (fc.getResult());
            });
    }

    AudioRecorderNode& rec;
    TextButton recBtn;
    Label timeLabel, folderLabel, pathLabel;
    TextButton advancedToggle, browseBtn;
    ComboBox formatCombo, modeCombo;
    bool expanded = false;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
AudioMixerEditor::AudioMixerEditor (AudioMixerProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p)
{
    setOpaque (true);
    addAndMakeVisible (addBtn);
    addAndMakeVisible (remBtn);
    addBtn.setTooltip ("Add channel");
    remBtn.setTooltip ("Remove last channel");
    addBtn.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
    remBtn.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
    addBtn.setColour (TextButton::textColourOffId, kAccent);
    remBtn.setColour (TextButton::textColourOffId, kAccent);

    addBtn.onClick = [this] {
        processor.addChannel();
        // Defer UI rebuild: the host may react to the bus-count change
        // by tearing down or relaying out the plugin window, and doing
        // the strip rebuild synchronously can race with that.
        Component::SafePointer<AudioMixerEditor> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe) safe->rebuildStrips(); });
    };
    remBtn.onClick = [this] {
        processor.removeLastChannel();
        Component::SafePointer<AudioMixerEditor> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe) safe->rebuildStrips(); });
    };

    // Recorder bar — hidden by default, REC button at top right toggles it.
    addAndMakeVisible (recorderToggle);
    recorderToggle.setClickingTogglesState (true);
    recorderToggle.setButtonText ("REC");
    recorderToggle.setColour (TextButton::buttonColourId, Colour (0xff1a1a1a));
    recorderToggle.setColour (TextButton::buttonOnColourId, Colour (0xffff2030));
    recorderToggle.setColour (TextButton::textColourOffId, kAccent);
    recorderToggle.setColour (TextButton::textColourOnId, Colours::white);
    recorderToggle.onClick = [this] {
        if (recorderBar)
            recorderBar->setVisible (recorderToggle.getToggleState());
        resized();
    };

    rebuildStrips();
    startTimerHz (24);
}

AudioMixerEditor::~AudioMixerEditor()
{
    channelStrips.clear();
    returnStrips.clear();
    masterStrip.reset();
}

void AudioMixerEditor::paint (Graphics& g)
{
    g.fillAll (kBg);
}

void AudioMixerEditor::rebuildStrips()
{
    channelStrips.clear();
    returnStrips.clear();
    masterStrip.reset();

    for (int i = 0; i < processor.getNumChannels(); ++i)
        if (auto* ch = processor.getChannel (i))
        {
            auto* s = new ChannelStrip (*ch, knobLAF);
            addAndMakeVisible (s);
            channelStrips.add (s);
        }

    for (int i = 0; i < kMixerFxReturns; ++i)
        if (auto* r = processor.getReturn (i))
        {
            auto* s = new ReturnStrip (*r, i, knobLAF);
            addAndMakeVisible (s);
            returnStrips.add (s);
        }

    masterStrip = std::make_unique<MasterStrip> (processor.getMaster(), knobLAF);
    addAndMakeVisible (masterStrip.get());

    // (Re)create the recorder bar — hidden by default.
    if (recorderBar == nullptr)
    {
        recorderBar = std::make_unique<RecorderBar> (processor.getRecorder());
        addChildComponent (recorderBar.get());   // not visible until toggled
    }

    // Set window size to fit all strips.
    const int totalW = kGutter
                       + (kStripWidth + kGutter) * processor.getNumChannels()
                       + kGutter
                       + (kReturnWidth + kGutter) * kMixerFxReturns
                       + kMasterWidth + kGutter * 2
                       + 28; // +/- buttons column
    const int totalH = 760;  // tall enough for all knobs + LED-ring LEVEL + meter
    setSize (juce::jmax (640, totalW), totalH);
    resized();
}

void AudioMixerEditor::resized()
{
    auto r = getLocalBounds().reduced (kGutter);

    // Bottom: recorder bar (only takes vertical space when shown).
    if (recorderBar != nullptr && recorderBar->isVisible())
    {
        const int barH = recorderBar->isExpanded() ? 78 : 44;
        recorderBar->setBounds (r.removeFromBottom (barH));
        r.removeFromBottom (4);
    }

    // Right side: master strip.
    masterStrip->setBounds (r.removeFromRight (kMasterWidth));
    r.removeFromRight (kGutter);

    // Add / remove buttons column on the very right (before master).
    auto btnCol = r.removeFromRight (26);
    addBtn.setBounds (btnCol.removeFromTop (24));
    btnCol.removeFromTop (4);
    remBtn.setBounds (btnCol.removeFromTop (24));
    btnCol.removeFromTop (8);
    recorderToggle.setBounds (btnCol.removeFromTop (28));
    r.removeFromRight (kGutter);

    // FX returns from the right of the channels area.
    for (int i = kMixerFxReturns - 1; i >= 0; --i)
    {
        if (auto* s = returnStrips[i])
            s->setBounds (r.removeFromRight (kReturnWidth));
        r.removeFromRight (kGutter);
    }
    r.removeFromRight (kGutter);

    // Channel strips fill from the left.
    for (int i = 0; i < channelStrips.size(); ++i)
    {
        channelStrips[i]->setBounds (r.removeFromLeft (kStripWidth));
        r.removeFromLeft (kGutter);
    }
}

void AudioMixerEditor::timerCallback()
{
    // Strips repaint themselves via internal timers (meters). Nothing to do.
}

} // namespace element
