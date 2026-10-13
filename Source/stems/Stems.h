#pragma once
// Flayr Chop on-device stem separation (framework-free C++17, no JUCE).
//
// Engine: Demucs v4 "Hybrid Transformer" (htdemucs) by Meta AI Research,
// run through a vendored and speed-optimized copy of sevagh/demucs.cpp
// (Source/stems/third_party/demucs.cpp, MIT) on Eigen (MPL2). Fully local,
// no Python, no dylibs. macOS uses the system Accelerate framework for GEMM.
// Local changes to the vendored code (marked "Flayr Chop:"): GEMM-friendly
// conv/norm/softmax kernels, shuffle-free channel-first layouts, cache-blocked
// attention / feed-forward / DConv, vDSP FFTs on Apple (each change kept
// the output within 60+ dB SNR, measured 85-130 dB per stem against v1.0.0),
// lower peak memory, no stdout logging, no exit().
// On x86-64 the network is built twice (portable and AVX2+FMA) and the
// engine is chosen at run time; see activeEngine().
//
// Model weights license: the Demucs repository (github.com/facebookresearch/
// demucs), including its released pretrained weights (htdemucs, htdemucs_6s,
// htdemucs_ft), is published by Meta under the MIT license; no separate
// weights license exists. The ggml .bin files
// fetched by scripts/fetch-model.sh are a lossless float16 repack of those
// weights made with demucs.cpp's convert script (hosted on Hugging Face at
// datasets/Retrobear/demucs.cpp). MIT is compatible with Flayr Chop's
// AGPLv3; keep the MIT notice when redistributing the weights. The weights
// are NOT committed to git; they are downloaded on first use.
//
// Supported model files (any of them can be passed as modelPath):
//   ggml-model-htdemucs-4s-f16.bin       84 MB  Drums, Bass, Other, Vocals (default)
//   ggml-model-htdemucs-6s-f16.bin       55 MB  + Guitar, Piano
//   ggml-model-htdemucs_ft_<stem>-4s-f16.bin x4  "fine-tuned" bag of 4 models:
//        pass any one of the four; all four must sit in the same folder.
//        About 4x slower than htdemucs for roughly +0.2 dB SDR.
//
// Threading: separate() is blocking and is meant to be called from a
// background thread. It is safe to run several separate() calls at once
// (each has its own buffers; loaded models are shared read-only and cached).
// Never call it from the audio thread.

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace chop::stems {

struct Separated {
    std::vector<std::string> names;                       // e.g. {"Drums","Bass","Other","Vocals"}
    std::vector<std::array<std::vector<float>, 2>> audio; // [stem][channel][sample], same length + sample rate as the input
};

// Input: stereo (pass the same pointer twice for mono) at any sample rate;
// resampled internally to the model rate (44.1 kHz) and back with a
// Kaiser-windowed sinc. Output length always equals numSamples exactly.
// progress(0..1) is called from the calling thread (the plug-in's worker
// thread), never from internal threads; return false to cancel, in which
// case separate() returns false (error = "cancelled") within a fraction of a
// second. numThreads <= 0 picks a default for this machine.
bool separate(const float* left, const float* right, int numSamples, double sampleRate,
              const std::string& modelPath, Separated& out,
              const std::function<bool(float)>& progress, int numThreads, std::string* error = nullptr);

// Same, with progressive results. `out` is sized (names, and every stem at
// numSamples, zero-filled) before the first call, and onSegmentReady(start,
// end) reports that samples [start, end) of every stem are final: the calls
// come in time order, cover [0, numSamples) without gaps or overlap, and
// arrive on the calling thread (like progress), roughly every 6 s of audio as
// the segments of the song finish, so the plug-in can use the start of the
// song long before the end is separated. Final samples are never written
// again; only the calling thread writes into out.audio. Another thread that
// reads them needs its own synchronization with the callback. On failure or
// cancel `out` is cleared as usual (data already handed out stays valid only
// in the caller's own copies). With the fine-tuned bag (four models run one
// after another) there is a single call at the end.
bool separate(const float* left, const float* right, int numSamples, double sampleRate,
              const std::string& modelPath, Separated& out,
              const std::function<bool(float)>& progress, int numThreads, std::string* error,
              const std::function<void(int startSample, int endSample)>& onSegmentReady);

// macOS:   ~/Library/Application Support/Flayr Chop/models/ggml-model-htdemucs-4s-f16.bin
// Windows: %APPDATA%\Flayr Chop\models\ggml-model-htdemucs-4s-f16.bin
// Linux:   $XDG_DATA_HOME or ~/.local/share/Flayr Chop/models/ggml-model-htdemucs-4s-f16.bin
std::string defaultModelPath();

// True when the file exists and looks like a complete, supported model
// (header magic and exact size of a known release). Cheap; no full load.
bool modelAvailable(const std::string& path);

// ---- extras (optional for the plug-in) ----

// File name and download URL of the default model, for an in-app downloader.
const char* defaultModelFileName();
const char* defaultModelUrl();
const char* defaultModelSha256();

// Default worker count used when numThreads <= 0.
int defaultThreadCount();

// Network code path. x86-64 builds carry a portable engine ("generic") and
// an AVX2 + FMA one ("avx2"); the fastest one this CPU supports is picked at
// run time. Other targets have only "generic".
const char* activeEngine();

// Engines this machine can run, fastest first.
std::vector<std::string> availableEngines();

// For tests and benchmarks: force an engine for later separate() calls, or
// pass "" to go back to automatic. Returns false (and changes nothing) if the
// name is unknown or this CPU cannot run it. Do not call it while a
// separation is running.
bool forceEngine(const std::string& name);

// Loaded models are cached (about 170 MB of RAM for htdemucs) so repeated
// separations skip the load. Call this to free them, e.g. when the editor
// closes or the plug-in is unloaded. Safe to call at any time.
void releaseCachedModels();

// Resampling helper used internally, exposed for tests and the plug-in.
// Resamples `in` from inRate to outRate producing exactly outLength samples
// (sample n of the output sits at time n / outRate).
std::vector<float> resample(const float* in, int inLength, double inRate, double outRate, int outLength);

} // namespace chop::stems
