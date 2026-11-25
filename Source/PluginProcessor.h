#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP.h"

namespace ids
{
    inline constexpr const char* inTrim     = "inTrim";
    inline constexpr const char* subCut     = "subCut";
    inline constexpr const char* monoBass   = "monoBass";
    inline constexpr const char* monoFreq   = "monoFreq";
    inline constexpr const char* satOn      = "satOn";
    inline constexpr const char* satAmt     = "satAmt";
    inline constexpr const char* sp12On     = "sp12On";
    inline constexpr const char* sp12Mix    = "sp12Mix";
    inline constexpr const char* glueOn     = "glueOn";
    inline constexpr const char* glueAmt    = "glueAmt";
    inline constexpr const char* glueThresh = "glueThresh";
    inline constexpr const char* tone       = "tone";
    inline constexpr const char* drift      = "drift";
    inline constexpr const char* crackle    = "crackle";
    inline constexpr const char* levelMode  = "levelMode";   // 0 Manual, 1 Auto, 2 Locked
    inline constexpr const char* target     = "target";
    inline constexpr const char* lockedGain = "lockedGain";
    inline constexpr const char* outGain    = "outGain";
    inline constexpr const char* ceiling    = "ceiling";
}

class BoomBapMasterProcessor : public juce::AudioProcessor
{
public:
    BoomBapMasterProcessor();
    ~BoomBapMasterProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Boom Bap Master"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // UI helpers (message thread)
    void requestMeterReset() { resetRequested.store (true); }
    // Freezes the gain the auto-ride has found into "Locked" mode, so a render is repeatable.
    void lockCurrentGain();

    juce::AudioProcessorValueTreeState apvts;

    // Meter values, written by the audio thread, read by the editor.
    std::atomic<float> preLufs   { -120.0f };   // integrated, before the level stage
    std::atomic<float> postLufs  { -120.0f };   // integrated, final output
    std::atomic<float> postShort { -120.0f };   // short-term (3 s), final output
    std::atomic<float> truePeak  { -120.0f };   // highest true peak since reset
    std::atomic<float> glueGr    { 0.0f };      // glue compressor gain reduction, dB
    std::atomic<float> rideGain  { 0.0f };      // level-stage gain, dB

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    bb::Params readParams() const;

    bb::MasterChain chain;
    std::atomic<bool> resetRequested { false };

    std::atomic<float>* pInTrim = nullptr;     std::atomic<float>* pSubCut = nullptr;
    std::atomic<float>* pMonoBass = nullptr;   std::atomic<float>* pMonoFreq = nullptr;
    std::atomic<float>* pSatOn = nullptr;      std::atomic<float>* pSatAmt = nullptr;
    std::atomic<float>* pSp12On = nullptr;     std::atomic<float>* pSp12Mix = nullptr;
    std::atomic<float>* pGlueOn = nullptr;     std::atomic<float>* pGlueAmt = nullptr;
    std::atomic<float>* pGlueThresh = nullptr; std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pDrift = nullptr;      std::atomic<float>* pCrackle = nullptr;
    std::atomic<float>* pLevelMode = nullptr;  std::atomic<float>* pTarget = nullptr;
    std::atomic<float>* pLockedGain = nullptr; std::atomic<float>* pOutGain = nullptr;
    std::atomic<float>* pCeiling = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoomBapMasterProcessor)
};
