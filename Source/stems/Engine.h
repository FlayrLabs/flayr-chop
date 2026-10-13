#pragma once
// Internal: the per-instruction-set network engines behind chop::stems.
//
// The Demucs network code (third_party/demucs.cpp + Eigen) is compiled more
// than once: a portable "generic" build (always present) and, on x86-64, an
// "avx2" build (AVX2 + FMA) whose code lives in separate namespaces so the
// two never share a symbol. The engine is picked at run time from CPUID, so
// one binary runs on any x86-64 CPU and is fast on AVX2 machines.
//
// The interface below is deliberately free of Eigen and demucs types: the
// two builds see different (renamed) copies of those types. Only the loaded
// model crosses the boundary, as an opaque pointer; both builds share one
// header for it, and layoutSignature() lets the caller refuse an engine
// whose view of that layout differs (it never should).

#include <cstddef>
#include <functional>

namespace chop::stems::detail {

struct EngineOps {
    const char* name;
    // Hash of the sizes and Eigen settings the model layout depends on.
    std::size_t (*layoutSignature)();
    // One worker = all buffers for one 7.8 s segment (about 0.4-0.7 GB peak).
    void* (*createWorker)(int segmentSamples, int nbSources);
    void (*destroyWorker)(void* worker);
    // Input: 2 x segmentSamples floats, column-major (index = channel + 2 * n).
    float* (*workerMix)(void* worker);
    // Output after run(): nbSources x 2 x segmentSamples, column-major
    // (index = source + nbSources * (channel + 2 * n)).
    const float* (*workerTargets)(void* worker);
    // Runs the network on workerMix() into workerTargets(). `model` is a
    // const demucscpp::demucs_model*. progress(0..1) is called from this
    // thread; it may throw to abort (the exception propagates).
    void (*run)(void* worker, const void* model, const std::function<void(float)>& progress);
};

const EngineOps& genericEngine();

// nullptr when this build has no AVX2 engine (non-x86 target, or
// FLAYR_CHOP_STEMS_DISPATCH=OFF).
const EngineOps* avx2EngineIfCompiled();

// True when the CPU and OS support AVX2 + FMA (YMM state enabled).
bool cpuSupportsAvx2Fma();

} // namespace chop::stems::detail
