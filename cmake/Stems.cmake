# Flayr Chop stem separation engine (Demucs v4 via a vendored, optimized copy
# of sevagh/demucs.cpp). Framework-free C++17, no JUCE.
#
# Defines:
#   flayr_chop_stems           static library, public header Source/stems/Stems.h
#   FLAYR_CHOP_STEMS_TARGET    set to the library target name (link this)
#   stems_cli                  tool: stems_cli in.wav outdir [model] [threads]
#   stems_test                 ctest, only when FLAYR_CHOP_BUILD_TESTS is ON
#
# Dependencies: Eigen master dd8c71e6 (2023-11, MPL2, header-only; 3.4.0 lacks Tensor APIs demucs.cpp uses) fetched at configure time.
# Set FLAYR_CHOP_EIGEN_DIR to a local Eigen checkout to build offline.
# macOS links Accelerate (system framework, nothing to bundle). Windows and
# Linux use Eigen's own kernels. No OpenMP, no dylibs.

include_guard(GLOBAL)
include(FetchContent)

set(FLAYR_CHOP_STEMS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(FLAYR_CHOP_EIGEN_DIR "" CACHE PATH "Optional local Eigen source dir (commit dd8c71e6) (skips download)")
option(FLAYR_CHOP_STEMS_ACCELERATE "macOS: route Eigen GEMMs to Accelerate (system framework)" ON)
option(FLAYR_CHOP_STEMS_AVX2 "x86_64 (non-Apple): compile the stems engine with AVX2+FMA (faster, needs a 2013+ CPU)" OFF)

if(FLAYR_CHOP_EIGEN_DIR)
    set(_flayr_chop_eigen_inc "${FLAYR_CHOP_EIGEN_DIR}")
else()
    FetchContent_Declare(flayr_chop_eigen
        URL https://gitlab.com/libeigen/eigen/-/archive/dd8c71e62852b2fe429edb6682ac91fd1c578a26/eigen-dd8c71e62852b2fe429edb6682ac91fd1c578a26.tar.gz
        URL_HASH SHA256=e8cbc003b1fa5e0d0d142c58155aaf341f87783d213d31a605a19b2f13d27acc
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # Headers only: populate without running Eigen's own CMakeLists.
    FetchContent_GetProperties(flayr_chop_eigen)
    if(NOT flayr_chop_eigen_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(PUSH)
            cmake_policy(SET CMP0169 OLD)
            FetchContent_Populate(flayr_chop_eigen)
            cmake_policy(POP)
        else()
            FetchContent_Populate(flayr_chop_eigen)
        endif()
    endif()
    set(_flayr_chop_eigen_inc "${flayr_chop_eigen_SOURCE_DIR}")
endif()

set(_demucs_dir "${FLAYR_CHOP_STEMS_ROOT}/Source/stems/third_party/demucs.cpp")

add_library(flayr_chop_stems STATIC
    "${FLAYR_CHOP_STEMS_ROOT}/Source/stems/Stems.cpp"
    "${FLAYR_CHOP_STEMS_ROOT}/Source/stems/Resampler.cpp"
    "${_demucs_dir}/crosstransformer.cpp"
    "${_demucs_dir}/dsp.cpp"
    "${_demucs_dir}/encdec.cpp"
    "${_demucs_dir}/layers.cpp"
    "${_demucs_dir}/lstm.cpp"
    "${_demucs_dir}/model_inference.cpp"
    "${_demucs_dir}/model_load.cpp"
    "${FLAYR_CHOP_STEMS_ROOT}/Source/stems/Stems.h"
    "${FLAYR_CHOP_STEMS_ROOT}/Source/stems/Resampler.h")

target_compile_features(flayr_chop_stems PUBLIC cxx_std_17)
set_target_properties(flayr_chop_stems PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(flayr_chop_stems PUBLIC "${FLAYR_CHOP_STEMS_ROOT}/Source")
target_include_directories(flayr_chop_stems PRIVATE "${_demucs_dir}")
target_include_directories(flayr_chop_stems SYSTEM PRIVATE "${_flayr_chop_eigen_inc}")

target_compile_definitions(flayr_chop_stems PRIVATE
    EIGEN_MPL2_ONLY            # refuse any LGPL Eigen code
    EIGEN_DONT_PARALLELIZE     # we thread across segments ourselves
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
    $<$<NOT:$<CONFIG:Debug>>:EIGEN_NO_DEBUG>)

# The network is unusably slow unoptimized (minutes per segment), so the
# engine is always built optimized, even inside a Debug plugin build.
if(MSVC)
    target_compile_options(flayr_chop_stems PRIVATE /bigobj /fp:fast $<$<NOT:$<CONFIG:Debug>>:/O2>)
    if(FLAYR_CHOP_STEMS_AVX2)
        target_compile_options(flayr_chop_stems PRIVATE /arch:AVX2)
    endif()
else()
    target_compile_options(flayr_chop_stems PRIVATE -O3 -fno-math-errno -Wno-unused-parameter
        -Wno-unused-variable -Wno-sign-compare -Wno-deprecated-declarations)
    if(FLAYR_CHOP_STEMS_AVX2 AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(flayr_chop_stems PRIVATE -mavx2 -mfma)
    endif()
endif()

if(APPLE AND FLAYR_CHOP_STEMS_ACCELERATE)
    # Accelerate sgemm (AMX on Apple silicon). Fine for arm64 and x86_64 slices.
    target_compile_definitions(flayr_chop_stems PRIVATE EIGEN_USE_BLAS)
    target_link_libraries(flayr_chop_stems PUBLIC "-framework Accelerate")
endif()

find_package(Threads REQUIRED)
target_link_libraries(flayr_chop_stems PRIVATE Threads::Threads)

set(FLAYR_CHOP_STEMS_TARGET flayr_chop_stems)

# Command-line separator (no JUCE): stems_cli in.wav outdir [model] [threads]
add_executable(stems_cli "${FLAYR_CHOP_STEMS_ROOT}/tools/stems_cli.cpp")
target_link_libraries(stems_cli PRIVATE flayr_chop_stems)

if(FLAYR_CHOP_BUILD_TESTS)
    enable_testing()
    add_executable(stems_test "${FLAYR_CHOP_STEMS_ROOT}/tests/stems_test.cpp")
    target_link_libraries(stems_test PRIVATE flayr_chop_stems)
    add_test(NAME stems_test COMMAND stems_test)
endif()
