cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(FlayrChop VERSION 1.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FLAYR_CHOP_COPY_AFTER_BUILD "Install the plugins into the user's plug-in folders after building" ON)
option(FLAYR_CHOP_BUILD_TOOLS "Build the UI snapshot tool (needs a desktop session)" OFF)
option(FLAYR_CHOP_BUILD_TESTS "Build the test suites" ON)
option(FLAYR_CHOP_TEST_HOOKS "Local UI testing only: FLAYR_CHOP_AUTOLOAD env var loads a track" OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
# HQ Stretch engine (MIT).
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE signalsmith-stretch)

# Third-party headers: keep their warnings out of ours.
foreach(lib signalsmith-stretch signalsmith-linear)
    get_target_property(_inc ${lib} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${lib} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
endforeach()

include(cmake/Stems.cmake)

if(APPLE)
    set(FLAYR_CHOP_FORMATS AU VST3)
elseif(WIN32)
    set(FLAYR_CHOP_FORMATS VST3)
else()
    set(FLAYR_CHOP_FORMATS VST3 LV2)
endif()

juce_add_plugin(FlayrChop
    COMPANY_NAME "Flayr Labs"
    COMPANY_WEBSITE "https://github.com/FlayrLabs/flayr-chop"
    BUNDLE_ID com.flayrlabs.flayrchop
    PLUGIN_MANUFACTURER_CODE Flyr
    PLUGIN_CODE FChp
    PRODUCT_NAME "Flayr Chop"
    DESCRIPTION "Sample chopper with stems and HQ Stretch"
    FORMATS ${FLAYR_CHOP_FORMATS}
    LV2URI "https://github.com/FlayrLabs/flayr-chop"
    VST3_CATEGORIES Instrument Sampler
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${FLAYR_CHOP_COPY_AFTER_BUILD})

set(FLAYR_CHOP_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Renderer.cpp
    Source/SampleLoader.cpp)

target_sources(FlayrChop PRIVATE ${FLAYR_CHOP_SOURCES})
target_include_directories(FlayrChop PRIVATE Source)
if(FLAYR_CHOP_TEST_HOOKS)
    target_compile_definitions(FlayrChop PRIVATE FLAYR_CHOP_TEST_HOOKS=1)
endif()

target_compile_definitions(FlayrChop PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FlayrChop
    PRIVATE
        juce::juce_audio_utils
        juce::juce_cryptography
        signalsmith-stretch
        ${FLAYR_CHOP_STEMS_TARGET}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(FLAYR_CHOP_BUILD_TESTS)
    enable_testing()
    add_executable(analysis_test tests/analysis_test.cpp)
    target_include_directories(analysis_test PRIVATE Source)
    add_test(NAME analysis_test COMMAND analysis_test)

    add_executable(sampler_test tests/sampler_test.cpp)
    target_include_directories(sampler_test PRIVATE Source)
    target_link_libraries(sampler_test PRIVATE signalsmith-stretch)
    add_test(NAME sampler_test COMMAND sampler_test)

    # Project save/restore through the real processor.
    juce_add_console_app(processor_state_test PRODUCT_NAME "processor_state_test")
    target_sources(processor_state_test PRIVATE tests/processor_state_test.cpp ${FLAYR_CHOP_SOURCES})
    target_include_directories(processor_state_test PRIVATE Source)
    target_compile_definitions(processor_state_test PRIVATE
        JucePlugin_Name="Flayr Chop" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(processor_state_test PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_recommended_config_flags
        signalsmith-stretch ${FLAYR_CHOP_STEMS_TARGET})
    add_test(NAME processor_state_test COMMAND processor_state_test)

    # Opens the editor during playback and reports what changes frame to frame.
    juce_add_console_app(FlayrChopOpenTest PRODUCT_NAME "FlayrChopOpenTest")
    target_sources(FlayrChopOpenTest PRIVATE tools/opentest.cpp ${FLAYR_CHOP_SOURCES})
    target_include_directories(FlayrChopOpenTest PRIVATE Source)
    target_compile_definitions(FlayrChopOpenTest PRIVATE
        JucePlugin_Name="Flayr Chop" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(FlayrChopOpenTest PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_recommended_config_flags
        signalsmith-stretch ${FLAYR_CHOP_STEMS_TARGET})
endif()

if(FLAYR_CHOP_BUILD_TOOLS)
    # UI snapshot without a DAW: FlayrChopSnapshot out.png [file.wav]
    juce_add_console_app(FlayrChopSnapshot PRODUCT_NAME "FlayrChopSnapshot")
    target_sources(FlayrChopSnapshot PRIVATE tools/snapshot.cpp ${FLAYR_CHOP_SOURCES})
    target_include_directories(FlayrChopSnapshot PRIVATE Source)
    target_compile_definitions(FlayrChopSnapshot PRIVATE
        JucePlugin_Name="Flayr Chop" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(FlayrChopSnapshot PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_recommended_config_flags
        signalsmith-stretch ${FLAYR_CHOP_STEMS_TARGET})


    # A tiny real-window host for checking the editor as a DAW shows it.
    juce_add_gui_app(FlayrChopMiniHost PRODUCT_NAME "FlayrChopMiniHost")
    target_sources(FlayrChopMiniHost PRIVATE tools/minihost.cpp)
    target_compile_definitions(FlayrChopMiniHost PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(FlayrChopMiniHost PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
