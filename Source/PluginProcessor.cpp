#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    using Float  = juce::AudioParameterFloat;
    using Bool   = juce::AudioParameterBool;
    using Choice = juce::AudioParameterChoice;
    using Range  = juce::NormalisableRange<float>;
    using FloatAttr = juce::AudioParameterFloatAttributes;

    juce::ParameterID pid (const char* id) { return juce::ParameterID { id, 1 }; }

    FloatAttr withUnit (const char* unit, int decimals = 1)
    {
        const juce::String u (unit);
        return FloatAttr().withStringFromValueFunction ([u, decimals] (float v, int) { return juce::String (v, decimals) + " " + u; });
    }

    FloatAttr asPercent()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout BoomBapMasterProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<Float> (pid (ids::inTrim), "Input Trim", Range (-12.0f, 12.0f, 0.1f), 0.0f, withUnit ("dB")));
    layout.add (std::make_unique<Bool>  (pid (ids::subCut), "Sub Cut (25 Hz)", true));
    layout.add (std::make_unique<Bool>  (pid (ids::monoBass), "Mono Bass", true));
    layout.add (std::make_unique<Float> (pid (ids::monoFreq), "Mono Below", Range (60.0f, 250.0f, 1.0f, 0.6f), 110.0f, withUnit ("Hz", 0)));

    layout.add (std::make_unique<Bool>  (pid (ids::satOn), "Saturation On", true));
    layout.add (std::make_unique<Float> (pid (ids::satAmt), "Saturation", Range (0.0f, 1.0f, 0.01f), 0.25f, asPercent()));

    layout.add (std::make_unique<Bool>  (pid (ids::sp12On), "12-bit / 26 kHz On", true));
    layout.add (std::make_unique<Float> (pid (ids::sp12Mix), "12-bit Mix", Range (0.0f, 1.0f, 0.01f), 1.0f, asPercent()));

    layout.add (std::make_unique<Bool>  (pid (ids::glueOn), "Glue On", true));
    layout.add (std::make_unique<Float> (pid (ids::glueAmt), "Glue Amount", Range (0.0f, 1.0f, 0.01f), 0.5f, asPercent()));
    layout.add (std::make_unique<Float> (pid (ids::glueThresh), "Glue Threshold", Range (-30.0f, 0.0f, 0.1f), -14.0f, withUnit ("dB")));

    layout.add (std::make_unique<Float> (pid (ids::tone), "90s Tone", Range (0.0f, 1.0f, 0.01f), 0.5f, asPercent()));
    layout.add (std::make_unique<Float> (pid (ids::drift), "Drift", Range (0.0f, 1.0f, 0.01f), 0.25f, asPercent()));
    layout.add (std::make_unique<Float> (pid (ids::crackle), "Crackle", Range (0.0f, 1.0f, 0.01f), 0.0f, asPercent()));

    layout.add (std::make_unique<Choice> (pid (ids::levelMode), "Level Mode", juce::StringArray { "Manual", "Auto (ride to target)", "Locked" }, 1));
    layout.add (std::make_unique<Float> (pid (ids::target), "Target Loudness", Range (-24.0f, -6.0f, 0.1f), -14.0f, withUnit ("LUFS")));
    layout.add (std::make_unique<Float> (pid (ids::lockedGain), "Locked Gain", Range (-24.0f, 24.0f, 0.01f), 0.0f, withUnit ("dB", 2)));
    layout.add (std::make_unique<Float> (pid (ids::outGain), "Output Trim", Range (-6.0f, 6.0f, 0.1f), 0.0f, withUnit ("dB")));
    layout.add (std::make_unique<Float> (pid (ids::ceiling), "True-Peak Ceiling", Range (-3.0f, 0.0f, 0.1f), -1.0f, withUnit ("dBTP")));

    return layout;
}

BoomBapMasterProcessor::BoomBapMasterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pInTrim = apvts.getRawParameterValue (ids::inTrim);       pSubCut = apvts.getRawParameterValue (ids::subCut);
    pMonoBass = apvts.getRawParameterValue (ids::monoBass);   pMonoFreq = apvts.getRawParameterValue (ids::monoFreq);
    pSatOn = apvts.getRawParameterValue (ids::satOn);         pSatAmt = apvts.getRawParameterValue (ids::satAmt);
    pSp12On = apvts.getRawParameterValue (ids::sp12On);       pSp12Mix = apvts.getRawParameterValue (ids::sp12Mix);
    pGlueOn = apvts.getRawParameterValue (ids::glueOn);       pGlueAmt = apvts.getRawParameterValue (ids::glueAmt);
    pGlueThresh = apvts.getRawParameterValue (ids::glueThresh);
    pTone = apvts.getRawParameterValue (ids::tone);           pDrift = apvts.getRawParameterValue (ids::drift);
    pCrackle = apvts.getRawParameterValue (ids::crackle);     pLevelMode = apvts.getRawParameterValue (ids::levelMode);
    pTarget = apvts.getRawParameterValue (ids::target);       pLockedGain = apvts.getRawParameterValue (ids::lockedGain);
    pOutGain = apvts.getRawParameterValue (ids::outGain);     pCeiling = apvts.getRawParameterValue (ids::ceiling);
}

bb::Params BoomBapMasterProcessor::readParams() const
{
    bb::Params p;
    p.inputTrimDb  = pInTrim->load();
    p.subCut       = pSubCut->load() > 0.5f;
    p.monoBass     = pMonoBass->load() > 0.5f;
    p.monoFreq     = pMonoFreq->load();
    p.satOn        = pSatOn->load() > 0.5f;
    p.satAmount    = pSatAmt->load();
    p.sp12On       = pSp12On->load() > 0.5f;
    p.sp12Mix      = pSp12Mix->load();
    p.glueOn       = pGlueOn->load() > 0.5f;
    p.glueAmount   = pGlueAmt->load();
    p.glueThreshDb = pGlueThresh->load();
    p.tone         = pTone->load();
    p.drift        = pDrift->load();
    p.crackle      = pCrackle->load();
    p.levelMode    = juce::jlimit (0, 2, (int) std::lround (pLevelMode->load()));
    p.targetLufs   = pTarget->load();
    p.lockedGainDb = pLockedGain->load();
    p.outGainDb    = pOutGain->load();
    p.ceilingDb    = pCeiling->load();
    return p;
}

void BoomBapMasterProcessor::prepareToPlay (double sampleRate, int)
{
    chain.prepare (sampleRate);
    chain.setParams (readParams());
    setLatencySamples (chain.latencySamples());
}

bool BoomBapMasterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void BoomBapMasterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (buffer.getNumChannels() < 2)
        return;

    if (resetRequested.exchange (false))
        chain.resetMeters();

    chain.setParams (readParams());
    chain.process (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());

    preLufs.store   ((float) chain.preIntegratedLufs());
    postLufs.store  ((float) chain.postIntegratedLufs());
    postShort.store ((float) chain.postShortTermLufs());
    truePeak.store  ((float) chain.outTruePeakDb());
    glueGr.store    ((float) chain.grDb());
    rideGain.store  ((float) chain.rideGainDb());
}

void BoomBapMasterProcessor::lockCurrentGain()
{
    const float g = juce::jlimit (-24.0f, 24.0f, rideGain.load());
    if (auto* p = apvts.getParameter (ids::lockedGain))
        p->setValueNotifyingHost (p->convertTo0to1 (g));
    if (auto* m = apvts.getParameter (ids::levelMode))
        m->setValueNotifyingHost (m->convertTo0to1 (2.0f));
}

juce::AudioProcessorEditor* BoomBapMasterProcessor::createEditor()
{
    return new BoomBapMasterEditor (*this);
}

void BoomBapMasterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BoomBapMasterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BoomBapMasterProcessor();
}
