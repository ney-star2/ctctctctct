#pragma once

#include "PluginProcessor.h"

namespace colours
{
    const juce::Colour background { 0xff15120f };
    const juce::Colour panel      { 0xff1e1a16 };
    const juce::Colour border     { 0xff3b3128 };
    const juce::Colour amber      { 0xffe2a23b };
    const juce::Colour cream      { 0xffeadfc9 };
    const juce::Colour dim        { 0xff8d8170 };
    const juce::Colour knobBody   { 0xff2b251f };
}

class BoomBapLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BoomBapLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

class BoomBapMasterEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit BoomBapMasterEditor (BoomBapMasterProcessor&);
    ~BoomBapMasterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label caption;
        std::unique_ptr<SliderAtt> att;   // declared last so it is destroyed first
    };

    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAtt> att;
    };

    void addKnob (Knob&, const juce::String& caption, const char* paramId);
    void addToggle (Toggle&, const juce::String& text, const char* paramId);
    static void placeKnob (Knob&, juce::Rectangle<int>);
    static void placeKnobs (std::initializer_list<Knob*>, juce::Rectangle<int>);
    void drawPanel (juce::Graphics&, juce::Rectangle<int>, const juce::String& title) const;
    void timerCallback() override;

    BoomBapMasterProcessor& proc;
    BoomBapLookAndFeel laf;

    Knob trim, monoFreq, satAmt, sp12Mix, glueAmt, glueThresh, tone, drift, crackle, target, locked, outTrim, ceiling;
    Toggle subCut, monoBass, satOn, sp12On, glueOn;

    juce::ComboBox mode;
    std::unique_ptr<ComboAtt> modeAtt;
    juce::TextButton lockBtn  { "LOCK GAIN" };
    juce::TextButton resetBtn { "RESET METERS" };

    juce::Rectangle<int> pInput, pSat, pSp, pGlue, pMove, pLevel, pMeters, meterText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoomBapMasterEditor)
};
