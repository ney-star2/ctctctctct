#include "PluginEditor.h"

//==============================================================================
BoomBapLookAndFeel::BoomBapLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, colours::cream);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, colours::dim);
    setColour (juce::ComboBox::backgroundColourId, colours::knobBody);
    setColour (juce::ComboBox::textColourId, colours::cream);
    setColour (juce::ComboBox::outlineColourId, colours::border);
    setColour (juce::ComboBox::arrowColourId, colours::amber);
    setColour (juce::PopupMenu::backgroundColourId, colours::panel);
    setColour (juce::PopupMenu::textColourId, colours::cream);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colours::amber.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedTextColourId, colours::cream);
    setColour (juce::TextButton::buttonColourId, colours::knobBody);
    setColour (juce::TextButton::buttonOnColourId, colours::amber);
    setColour (juce::TextButton::textColourOffId, colours::cream);
    setColour (juce::TextButton::textColourOnId, colours::background);
}

void BoomBapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                           float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (7.0f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float alpha = s.isEnabled() ? 1.0f : 0.35f;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
    g.setColour (colours::border.withAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, angle, true);
    g.setColour (colours::amber.withAlpha (alpha));
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float br = r * 0.74f;
    g.setColour (colours::knobBody.withAlpha (alpha));
    g.fillEllipse (c.x - br, c.y - br, br * 2.0f, br * 2.0f);
    g.setColour (colours::border.withAlpha (alpha));
    g.drawEllipse (c.x - br, c.y - br, br * 2.0f, br * 2.0f, 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -br + 3.0f, 3.0f, br * 0.45f, 1.5f);
    g.setColour (colours::cream.withAlpha (alpha));
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void BoomBapLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto led = r.removeFromLeft (r.getHeight()).reduced (6.0f);
    const bool on = b.getToggleState();

    g.setColour (on ? colours::amber : juce::Colour (0xff3a342c));
    g.fillEllipse (led);
    g.setColour (colours::border);
    g.drawEllipse (led, 1.0f);
    if (on)
    {
        g.setColour (colours::amber.withAlpha (0.25f));
        g.fillEllipse (led.expanded (3.0f));
    }

    g.setColour (colours::cream.withAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.setFont (12.5f);
    g.drawText (b.getButtonText(), r.toNearestInt(), juce::Justification::centredLeft);
}

//==============================================================================
BoomBapMasterEditor::BoomBapMasterEditor (BoomBapMasterProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);

    addKnob (trim,       "TRIM",       ids::inTrim);
    addKnob (monoFreq,   "MONO BELOW", ids::monoFreq);
    addKnob (satAmt,     "AMOUNT",     ids::satAmt);
    addKnob (sp12Mix,    "MIX",        ids::sp12Mix);
    addKnob (glueAmt,    "AMOUNT",     ids::glueAmt);
    addKnob (glueThresh, "THRESHOLD",  ids::glueThresh);
    addKnob (tone,       "90s TONE",   ids::tone);
    addKnob (drift,      "DRIFT",      ids::drift);
    addKnob (crackle,    "CRACKLE",    ids::crackle);
    addKnob (target,     "TARGET",     ids::target);
    addKnob (locked,     "LOCKED GAIN", ids::lockedGain);
    addKnob (outTrim,    "OUT TRIM",   ids::outGain);
    addKnob (ceiling,    "CEILING",    ids::ceiling);

    addToggle (subCut,   "Sub cut 25 Hz", ids::subCut);
    addToggle (monoBass, "Mono bass",     ids::monoBass);
    addToggle (satOn,    "ON",            ids::satOn);
    addToggle (sp12On,   "ON",            ids::sp12On);
    addToggle (glueOn,   "ON",            ids::glueOn);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (ids::levelMode)))
        mode.addItemList (choice->choices, 1);
    addAndMakeVisible (mode);
    modeAtt = std::make_unique<ComboAtt> (proc.apvts, ids::levelMode, mode);

    lockBtn.setTooltip ("Play the whole beat once in Auto mode, then press this to freeze the gain it found.");
    lockBtn.onClick = [this] { proc.lockCurrentGain(); };
    resetBtn.onClick = [this] { proc.requestMeterReset(); };
    addAndMakeVisible (lockBtn);
    addAndMakeVisible (resetBtn);

    setSize (800, 520);
    startTimerHz (15);
}

BoomBapMasterEditor::~BoomBapMasterEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void BoomBapMasterEditor::addKnob (Knob& k, const juce::String& caption, const char* paramId)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 16);
    k.slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    k.slider.setPopupDisplayEnabled (false, false, nullptr);
    k.caption.setText (caption, juce::dontSendNotification);
    k.caption.setJustificationType (juce::Justification::centred);
    k.caption.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.caption);
    k.att = std::make_unique<SliderAtt> (proc.apvts, paramId, k.slider);
}

void BoomBapMasterEditor::addToggle (Toggle& t, const juce::String& text, const char* paramId)
{
    t.button.setButtonText (text);
    addAndMakeVisible (t.button);
    t.att = std::make_unique<ButtonAtt> (proc.apvts, paramId, t.button);
}

void BoomBapMasterEditor::placeKnob (Knob& k, juce::Rectangle<int> area)
{
    k.caption.setBounds (area.removeFromTop (16));
    k.slider.setBounds (area);
}

void BoomBapMasterEditor::placeKnobs (std::initializer_list<Knob*> knobs, juce::Rectangle<int> area)
{
    const int w = area.getWidth() / (int) knobs.size();
    for (auto* k : knobs)
        placeKnob (*k, area.removeFromLeft (w));
}

void BoomBapMasterEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (46);                                   // header

    auto row1 = area.removeFromTop (196);
    area.removeFromTop (12);
    auto row2 = area;

    auto take = [] (juce::Rectangle<int>& r, int w) { auto p = r.removeFromLeft (w); r.removeFromLeft (8); return p; };

    pInput = take (row1, 236);  pSat = take (row1, 148);  pSp = take (row1, 148);  pGlue = row1;
    pMove  = take (row2, 236);  pLevel = take (row2, 304);                         pMeters = row2;

    // INPUT
    {
        auto in = pInput.reduced (10);
        in.removeFromTop (22);
        auto toggles = in.removeFromTop (26);
        subCut.button.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
        monoBass.button.setBounds (toggles);
        placeKnobs ({ &trim, &monoFreq }, in);
    }

    auto titledToggle = [] (juce::Rectangle<int> panel, Toggle& t) -> juce::Rectangle<int>
    {
        auto in = panel.reduced (10);
        auto head = in.removeFromTop (22);
        t.button.setBounds (head.removeFromRight (58));
        return in;
    };

    placeKnobs ({ &satAmt },  titledToggle (pSat, satOn));
    placeKnobs ({ &sp12Mix }, titledToggle (pSp, sp12On));
    placeKnobs ({ &glueAmt, &glueThresh }, titledToggle (pGlue, glueOn));

    // TONE & MOVEMENT
    {
        auto in = pMove.reduced (10);
        in.removeFromTop (22);
        placeKnobs ({ &tone, &drift, &crackle }, in);
    }

    // LEVEL
    {
        auto in = pLevel.reduced (10);
        in.removeFromTop (22);
        mode.setBounds (in.removeFromTop (28));
        in.removeFromTop (6);
        placeKnobs ({ &target, &locked, &outTrim, &ceiling }, in);
    }

    // METERS
    {
        auto in = pMeters.reduced (10);
        in.removeFromTop (22);
        auto buttons = in.removeFromBottom (64);
        lockBtn.setBounds (buttons.removeFromTop (30));
        buttons.removeFromTop (4);
        resetBtn.setBounds (buttons.removeFromTop (30));
        meterText = in;
    }
}

void BoomBapMasterEditor::drawPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title) const
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);
    g.setColour (colours::amber);
    g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    g.drawText (title, r.getX() + 12, r.getY() + 8, r.getWidth() - 24, 16, juce::Justification::centredLeft);
}

static juce::String fmt (float v, const char* unit)
{
    return v <= -100.0f ? juce::String ("--") : juce::String (v, 1) + " " + unit;
}

void BoomBapMasterEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    g.setColour (colours::cream);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("BOOM BAP MASTER", 16, 12, 400, 30, juce::Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("finished-record finishing chain  |  12-bit / 26 kHz  |  true-peak safe", 300, 16, 484, 24, juce::Justification::centredRight);

    drawPanel (g, pInput,  "INPUT  /  LOW END");
    drawPanel (g, pSat,    "SATURATION");
    drawPanel (g, pSp,     "12-BIT / 26 kHz");
    drawPanel (g, pGlue,   "GLUE COMPRESSOR");
    drawPanel (g, pMove,   "TONE  /  MOVEMENT");
    drawPanel (g, pLevel,  "LEVEL");
    drawPanel (g, pMeters, "METERS");

    struct Row { const char* label; juce::String value; };
    const Row rows[] =
    {
        { "Input (integrated)",  fmt (proc.preLufs.load(),   "LUFS") },
        { "Output (integrated)", fmt (proc.postLufs.load(),  "LUFS") },
        { "Output (short-term)", fmt (proc.postShort.load(), "LUFS") },
        { "True peak (max)",     fmt (proc.truePeak.load(),  "dBTP") },
        { "Glue reduction",      juce::String (proc.glueGr.load(), 1) + " dB" },
        { "Level gain",          juce::String (proc.rideGain.load(), 1) + " dB" },
    };

    auto r = meterText;
    const int rowH = juce::jmax (16, r.getHeight() / (int) std::size (rows));
    for (const auto& row : rows)
    {
        auto line = r.removeFromTop (rowH);
        g.setColour (colours::dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (row.label, line, juce::Justification::centredLeft);
        g.setColour (colours::cream);
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText (row.value, line, juce::Justification::centredRight);
    }
}

void BoomBapMasterEditor::timerCallback()
{
    const int m = juce::jlimit (0, 2, (int) std::lround (proc.apvts.getRawParameterValue (ids::levelMode)->load()));

    target.slider.setEnabled (m == 1);
    locked.slider.setEnabled (m == 2);
    lockBtn.setEnabled (m == 1);

    repaint (meterText);
}
