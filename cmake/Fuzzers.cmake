# Fuzz targets (tests/fuzz), only with -DFLAYR_CHOP_BUILD_FUZZERS=ON. Never part of a release: the
# plug-in targets are untouched (sanitizers and coverage apply to the fuzz executables only), and the
# release script and workflow never turn this on.
#
#   fuzz_sample_load    an audio file through openChecked/readAll/loadSample/buildSource
#   fuzz_state_restore  a project through setStateInformation and the whole restore
#   fuzz_model_parser   the stem model's header/tensor parser (checksum bypassed in the harness only)
#   fuzz_stem_cache     the on-disk stem cache (stems.txt + FLAC stems) through loadCachedStems
#
# Engine: libFuzzer when the compiler has it (clang on Linux, Homebrew/upstream LLVM on macOS),
# otherwise tests/fuzz/FuzzDriver.cpp, a coverage-guided mutator built on SanitizerCoverage (Apple
# clang ships no libFuzzer). Either way the targets run with ASan + UBSan. scripts/fuzz.sh runs them.

include_guard(GLOBAL)

if(MSVC)
    message(FATAL_ERROR "FLAYR_CHOP_BUILD_FUZZERS needs clang (libFuzzer or SanitizerCoverage); MSVC is not supported")
endif()
if(FLAYR_CHOP_COPY_AFTER_BUILD)
    message(FATAL_ERROR "FLAYR_CHOP_BUILD_FUZZERS: configure with -DFLAYR_CHOP_COPY_AFTER_BUILD=OFF (a fuzz build is never installed)")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "FLAYR_CHOP_BUILD_FUZZERS: use a separate RelWithDebInfo or Debug build directory, never a release build")
endif()

set(FLAYR_CHOP_FUZZ_ENGINE "auto" CACHE STRING "Fuzzing engine: auto, libfuzzer or standalone")
set_property(CACHE FLAYR_CHOP_FUZZ_ENGINE PROPERTY STRINGS auto libfuzzer standalone)

include(CheckCXXSourceCompiles)
set(_fuzz_engine "${FLAYR_CHOP_FUZZ_ENGINE}")
if(_fuzz_engine STREQUAL "auto")
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
    check_cxx_source_compiles("
        #include <cstddef>
        #include <cstdint>
        extern \"C\" int LLVMFuzzerTestOneInput (const uint8_t*, size_t) { return 0; }"
        FLAYR_CHOP_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(FLAYR_CHOP_HAVE_LIBFUZZER)
        set(_fuzz_engine "libfuzzer")
    else()
        set(_fuzz_engine "standalone")
    endif()
endif()
message(STATUS "Flayr Chop fuzzers: ${_fuzz_engine} engine, ASan + UBSan")

# UBSan stays recoverable so tests/fuzz/ubsan.supp can skip known third-party patterns at run time;
# halt_on_error=1 (FuzzDriver.cpp's defaults, scripts/fuzz.sh) makes every other report fatal.
set(_fuzz_san -fsanitize=address,undefined -fno-omit-frame-pointer -g)
if(_fuzz_engine STREQUAL "libfuzzer")
    set(_fuzz_compile ${_fuzz_san} -fsanitize=fuzzer-no-link)
    set(_fuzz_link -fsanitize=fuzzer,address,undefined)
else()
    set(_fuzz_compile ${_fuzz_san} -fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp)
    set(_fuzz_link -fsanitize=address,undefined)
    # The engine itself: sanitized, but never instrumented for coverage.
    add_library(flayr_chop_fuzz_driver STATIC "${CMAKE_CURRENT_LIST_DIR}/../tests/fuzz/FuzzDriver.cpp")
    target_compile_options(flayr_chop_fuzz_driver PRIVATE ${_fuzz_san})
    target_compile_features(flayr_chop_fuzz_driver PRIVATE cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(flayr_chop_fuzz_driver PUBLIC Threads::Threads)
endif()

function(_flayr_chop_fuzz_setup target)
    target_compile_options(${target} PRIVATE ${_fuzz_compile})
    target_link_options(${target} PRIVATE ${_fuzz_link})
    if(APPLE)
        set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    if(TARGET flayr_chop_fuzz_driver)
        set_target_properties(flayr_chop_fuzz_driver PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        get_target_property(_type ${target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            # Whole archive is not needed: FuzzDriver.cpp defines main, so the linker always pulls it in.
            target_link_libraries(${target} PRIVATE flayr_chop_fuzz_driver)
        endif()
    endif()
endfunction()

# A sanitized, instrumented copy of the stem engine (the plug-in keeps the plain one).
get_target_property(_stems_sources flayr_chop_stems SOURCES)
add_library(flayr_chop_stems_fuzz STATIC ${_stems_sources})
foreach(_prop INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
              INTERFACE_COMPILE_FEATURES LINK_LIBRARIES INTERFACE_LINK_LIBRARIES POSITION_INDEPENDENT_CODE)
    get_target_property(_value flayr_chop_stems ${_prop})
    if(_value)
        set_property(TARGET flayr_chop_stems_fuzz PROPERTY ${_prop} "${_value}")
    endif()
endforeach()
_flayr_chop_fuzz_setup(flayr_chop_stems_fuzz)

add_executable(fuzz_model_parser "${CMAKE_CURRENT_LIST_DIR}/../tests/fuzz/fuzz_model_parser.cpp")
target_link_libraries(fuzz_model_parser PRIVATE flayr_chop_stems_fuzz)
# The harness parses into preallocated models, so it sees the vendored headers with the library's
# own definitions (Eigen alignment must match).
get_target_property(_stems_inc flayr_chop_stems INCLUDE_DIRECTORIES)
get_target_property(_stems_defs flayr_chop_stems COMPILE_DEFINITIONS)
target_include_directories(fuzz_model_parser SYSTEM PRIVATE ${_stems_inc})
target_compile_definitions(fuzz_model_parser PRIVATE ${_stems_defs})
target_compile_features(fuzz_model_parser PRIVATE cxx_std_17)
_flayr_chop_fuzz_setup(fuzz_model_parser)

foreach(_name sample_load state_restore stem_cache)
    set(_t fuzz_${_name})
    juce_add_console_app(${_t} PRODUCT_NAME "${_t}")
    target_sources(${_t} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../tests/fuzz/${_t}.cpp" ${FLAYR_CHOP_SOURCES})
    target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Source" "${CMAKE_CURRENT_LIST_DIR}/../tests/fuzz")
    target_compile_definitions(${_t} PRIVATE
        JucePlugin_Name="Flayr Chop" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_MODAL_LOOPS_PERMITTED=1
        FLAYR_CHOP_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/../tests/fixtures")
    target_link_libraries(${_t} PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_recommended_config_flags
        signalsmith-stretch flayr_chop_stems_fuzz)
    _flayr_chop_fuzz_setup(${_t})
endforeach()
