cmake_minimum_required (VERSION 3.22)

# Static C runtime on Windows: the .vst3 runs without installing the VC++ redistributable.
set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# macOS: universal binary (Apple Silicon + Intel).
set (CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set (CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")

project (BoomBapMaster VERSION 1.0.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

include (FetchContent)
FetchContent_Declare (JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable (JUCE)

juce_add_plugin (BoomBapMaster
    COMPANY_NAME                "BoomBap DSP"
    PLUGIN_MANUFACTURER_CODE    Bbdp
    PLUGIN_CODE                 Bbmm
    FORMATS                     VST3
    PRODUCT_NAME                "Boom Bap Master"
    VST3_CATEGORIES             Fx Dynamics Mastering
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources (BoomBapMaster PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories (BoomBapMaster PRIVATE Source)

target_compile_definitions (BoomBapMaster PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries (BoomBapMaster
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Optional: the JUCE-free DSP tests (also run by CI on Linux).
option (BBM_BUILD_TESTS "Build the DSP unit tests" OFF)
if (BBM_BUILD_TESTS)
    add_executable (BoomBapDspTest Tests/DspTest.cpp)
    target_include_directories (BoomBapDspTest PRIVATE Source)
endif()
