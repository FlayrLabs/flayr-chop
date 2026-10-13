// The AVX2 + FMA network engine (x86-64 only; empty elsewhere, so it is safe
// in a macOS universal build). Selected at run time by Stems.cpp when
// cpuSupportsAvx2Fma() is true; the generic engine covers every other CPU.
//
// How this file avoids illegal instructions on older CPUs:
//  - No compiler-wide flag (-mavx2, /arch:AVX2) is used. All standard and
//    system headers are parsed first (EnginePrelude.h), with baseline code
//    generation, so the library templates the linker shares between
//    translation units (std::vector, std::function, ...) are never AVX2 code.
//  - Only the code after that is AVX2: on GCC/Clang through a target region
//    pragma, on MSVC (which has no such pragma but accepts AVX2 intrinsics
//    without /arch) through Eigen's explicit AVX2/FMA intrinsics.
//  - Eigen and demucs.cpp are re-included under renamed namespaces
//    (flayr_avx2_Eigen, flayr_avx2_demucscpp), so none of their template
//    instantiations or functions can be merged with the generic copies.
// Only the loaded model is shared with the generic build (same header, same
// Eigen alignment settings via EIGEN_MAX_ALIGN_BYTES from cmake/Stems.cmake;
// checked at run time with layoutSignature()).

#include "stems/Engine.h"

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(FLAYR_STEMS_NO_AVX2_ENGINE)

#include "stems/EnginePrelude.h"

// ---- open the AVX2 + FMA region
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx,avx2,fma,sse3,ssse3,sse4.1,sse4.2,popcnt"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx,avx2,fma,sse3,ssse3,sse4.1,sse4.2,popcnt")
#endif

// Tell Eigen which packet math to use (it normally infers this from
// __AVX2__ etc., which only a global flag would define).
#ifndef EIGEN_VECTORIZE_SSE3
#define EIGEN_VECTORIZE_SSE3
#endif
#ifndef EIGEN_VECTORIZE_SSSE3
#define EIGEN_VECTORIZE_SSSE3
#endif
#ifndef EIGEN_VECTORIZE_SSE4_1
#define EIGEN_VECTORIZE_SSE4_1
#endif
#ifndef EIGEN_VECTORIZE_SSE4_2
#define EIGEN_VECTORIZE_SSE4_2
#endif
#ifndef EIGEN_VECTORIZE_AVX
#define EIGEN_VECTORIZE_AVX
#endif
#ifndef EIGEN_VECTORIZE_AVX2
#define EIGEN_VECTORIZE_AVX2
#endif
#ifndef EIGEN_VECTORIZE_FMA
#define EIGEN_VECTORIZE_FMA
#endif

// ---- separate namespaces for everything compiled in the region
#define Eigen flayr_avx2_Eigen
#define demucscpp flayr_avx2_demucscpp
#define demucscpp_v3 flayr_avx2_demucscpp_v3

#include "crosstransformer.cpp"
#include "dsp.cpp"
#include "encdec.cpp"
#include "layers.cpp"
#include "lstm.cpp"
#include "model_inference.cpp"

#define FLAYR_STEMS_ENGINE_NS avx2
#define FLAYR_STEMS_ENGINE_NAME "avx2"
#include "stems/EngineImpl.inc"

#undef demucscpp_v3
#undef demucscpp
#undef Eigen

#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

namespace chop::stems::detail {
const EngineOps* avx2EngineIfCompiled()
{
    return &avx2::ops();
}
} // namespace chop::stems::detail

#else

namespace chop::stems::detail {
const EngineOps* avx2EngineIfCompiled()
{
    return nullptr;
}
} // namespace chop::stems::detail

#endif
