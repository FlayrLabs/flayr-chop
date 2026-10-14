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
option(FLAYR_CHOP_BUILD_FUZZERS "Build the fuzz targets in tests/fuzz (ASan + UBSan; a separate non-release build only)" OFF)

include(FetchContent)
# Our patches to JUCE (patches/*.patch; the header of each explains it), applied in this order by
# cmake/ApplyPatch.cmake. The first six are the same files Flayr Tune ships, so both plug-ins carry
# identical fixes to JUCE's shared code; the rest fix JUCE's audio file readers, which only Flayr
# Chop uses.
#  xml-entity-overread: a heap over-read in the XML parser.
#  xml-depth-limit: the XML parser fails beyond 64 nesting levels instead of recursing on.
#  double-exponent-overflow: an int overflow parsing numbers like 1e99999999999.
#  vst3-private-state: the VST3 wrapper's own state trailer (wild read, stack overflow).
#  wrapper-state-checks: unchecked types and sizes in the AU and LV2 state paths.
#  base64-declared-size: base64 decoding allocated whatever size the text claimed.
#  mp3-intensity-stereo: an out-of-bounds write in the MP3 decoder.
#  caf-metadata-loops: endless loops in the CAF metadata reader (macOS).
#  audio-chunk-sizes: WAV/AIFF metadata chunks allocated whatever size their header claimed.
#  aiff-cate-overread: a heap over-read in the AIFF 'cate' chunk reader.
#  wav-no-xml-metadata: WAV iXML/axml metadata is never parsed.
set(FLAYR_JUCE_PATCHES)
foreach(name xml-entity-overread xml-depth-limit double-exponent-overflow vst3-private-state wrapper-state-checks
             base64-declared-size mp3-intensity-stereo caf-metadata-loops audio-chunk-sizes aiff-cate-overread
             wav-no-xml-metadata)
    list(APPEND FLAYR_JUCE_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/juce-${name}.patch)
endforeach()
# The patch contents are part of the patch command, so editing a patch re-runs the step.
set(flayr_patch_hashes "")
foreach(patch IN LISTS FLAYR_JUCE_PATCHES)
    file(SHA256 "${patch}" hash)
    string(APPEND flayr_patch_hashes "${hash}")
endforeach()
string(SHA256 FLAYR_JUCE_PATCH_SET "${flayr_patch_hashes}")
string(REPLACE ";" "|" FLAYR_JUCE_PATCHES "${FLAYR_JUCE_PATCHES}") # survives PATCH_COMMAND
# Dependencies are pinned to exact commits (a tag can be moved; a commit hash can't). When bumping,
# check the new hash with `git ls-remote <repo> refs/tags/<tag>` (the ^{} line for annotated tags).
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ea77444750de45e036e0279bb0291144bf8e3688 # 8.0.16-security (8.0.15 + a juce::String fix for malformed UTF-8)
    PATCH_COMMAND ${CMAKE_COMMAND} "-DPATCHES=${FLAYR_JUCE_PATCHES}" -DPATCH_SET=${FLAYR_JUCE_PATCH_SET}
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake
)
# HQ Stretch engine (MIT), and the linear-algebra library it uses, declared here first so the
# version stretch would fetch by tag is pinned by commit too (stretch 1.4.0 asks for 0.6.4).
FetchContent_Declare(signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG de55e6a50ffcf6f8f43f649692d94691c7025151 # 0.6.4
)
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG a670068d9aeb64913331d5cc29337b19a457a7df # 1.4.0
)
FetchContent_MakeAvailable(JUCE signalsmith-linear signalsmith-stretch)

# A JUCE tree from elsewhere (FETCHCONTENT_SOURCE_DIR_JUCE) skips PATCH_COMMAND: refuse to build
# without the patches, checked by content (see cmake/ApplyPatch.cmake).
execute_process(COMMAND ${CMAKE_COMMAND} "-DPATCHES=${FLAYR_JUCE_PATCHES}" -DCHECK_ONLY=ON
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake
                WORKING_DIRECTORY "${juce_SOURCE_DIR}" RESULT_VARIABLE flayr_patches_missing)
if(flayr_patches_missing)
    message(FATAL_ERROR "JUCE at ${juce_SOURCE_DIR} is missing patches/*.patch; apply them first:\n"
        "  cd ${juce_SOURCE_DIR} && cmake -DPATCHES=<patches, |-separated> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake")
endif()

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

    # Crafted files, states and paths through the real processor (security regressions).
    juce_add_console_app(hardening_test PRODUCT_NAME "hardening_test")
    target_sources(hardening_test PRIVATE tests/hardening_test.cpp ${FLAYR_CHOP_SOURCES})
    target_include_directories(hardening_test PRIVATE Source)
    target_compile_definitions(hardening_test PRIVATE
        JucePlugin_Name="Flayr Chop" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_MODAL_LOOPS_PERMITTED=1
        FLAYR_CHOP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(hardening_test PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_recommended_config_flags
        signalsmith-stretch ${FLAYR_CHOP_STEMS_TARGET})
    add_test(NAME hardening_test COMMAND hardening_test)

    # The built VST3 loaded by a host: crafted states through JUCE's VST3 wrapper (see the test).
    if(TARGET FlayrChop_VST3)
        juce_add_console_app(vst3_wrapper_test PRODUCT_NAME "vst3_wrapper_test")
        target_sources(vst3_wrapper_test PRIVATE tests/vst3_wrapper_test.cpp)
        target_compile_definitions(vst3_wrapper_test PRIVATE
            JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
        target_link_libraries(vst3_wrapper_test PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
        add_dependencies(vst3_wrapper_test FlayrChop_VST3)
        get_target_property(_flayr_chop_vst3 FlayrChop_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
        add_test(NAME vst3_wrapper_test COMMAND vst3_wrapper_test "${_flayr_chop_vst3}")
    endif()

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

if(FLAYR_CHOP_BUILD_FUZZERS)
    include(cmake/Fuzzers.cmake)
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
