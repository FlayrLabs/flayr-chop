#include "stems/Stems.h"
#include "stems/Resampler.h"

#include "dsp.hpp"
#include "model.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#if defined(EIGEN_USE_BLAS) && __has_include(<vecLib/thread_api.h>)
#include <vecLib/thread_api.h>
#define FLAYR_STEMS_HAS_BLAS_THREADING 1
#endif
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace chop::stems {

namespace {

constexpr int kModelRate = demucscpp::SUPPORTED_SAMPLE_RATE; // 44100

// Where the default model is downloaded from. Change this one constant to
// point at a mirror (for example a GitHub release asset); the file must keep
// the name and SHA-256 listed in kKnownModels[0].
constexpr const char* kDefaultModelUrl =
    "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-4s-f16.bin";

// Known model releases (ggml f16 repacks from demucs.cpp).
struct KnownModel {
    const char* file;
    long long size;
    const char* sha256;
};
constexpr KnownModel kKnownModels[] = {
    {"ggml-model-htdemucs-4s-f16.bin", 83994361LL,
     "72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064"},
    {"ggml-model-htdemucs-6s-f16.bin", 54855129LL,
     "09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856"},
    {"ggml-model-htdemucs_ft_drums-4s-f16.bin", 83994361LL,
     "cda268755cc80deb5f4616a99f9d09f26bc5ce3882aa35e0bc504708a12604bb"},
    {"ggml-model-htdemucs_ft_bass-4s-f16.bin", 83994361LL,
     "e6ded215aa15fb4eff7a37f3a51fd596ee17c94ae0ddb7ef3f2f6ecf8b86cdb5"},
    {"ggml-model-htdemucs_ft_other-4s-f16.bin", 83994361LL,
     "cbc315571584d512e7853c9d40a499182393a7f4155cb8723a9040a685c2435b"},
    {"ggml-model-htdemucs_ft_vocals-4s-f16.bin", 83994361LL,
     "19186500a45a551a034d96e9500415ebe73c8bd570bf55337ddc8cc8f53a9120"},
};
constexpr const char* kFtStemOrder[4] = {"drums", "bass", "other", "vocals"};

struct Cancelled {};

void setError(std::string* error, const std::string& msg)
{
    if (error)
        *error = msg;
}

std::filesystem::path toPath(const std::string& utf8)
{
#if defined(_WIN32)
    return std::filesystem::u8path(utf8);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string fromPath(const std::filesystem::path& p)
{
#if defined(_WIN32)
    return p.u8string();
#else
    return p.string();
#endif
}

// We parallelize across segments, so each worker asks Accelerate for
// single-threaded GEMMs (per-thread setting, macOS 15+; older systems keep
// Accelerate's default and simply oversubscribe a little).
void configureBlasForWorker()
{
#if defined(FLAYR_STEMS_HAS_BLAS_THREADING)
    if (__builtin_available(macOS 15.0, *))
        BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
#endif
}

// Flush denormals on our worker threads (audio-plugin hosts do this on the
// audio thread, not on threads we create).
void enableFlushToZero()
{
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC arm64: leave defaults
#else
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24); // FZ
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_setcsr(_mm_getcsr() | 0x8040); // FTZ | DAZ
#endif
}

// ---------------------------------------------------------------- models

struct LoadedModel {
    std::unique_ptr<demucscpp::demucs_model> model;
};

std::mutex gCacheMutex;
std::map<std::string, std::shared_ptr<const LoadedModel>> gCache;

const KnownModel* findKnown(const std::string& path)
{
    const std::string name = fromPath(toPath(path).filename());
    for (const auto& k : kKnownModels)
        if (name == k.file)
            return &k;
    return nullptr;
}

bool checkModelFile(const std::string& path, std::string* why)
{
    std::error_code ec;
    const auto p = toPath(path);
    if (!std::filesystem::is_regular_file(p, ec)) {
        if (why) *why = "model file not found: " + path;
        return false;
    }
    const auto size = (long long)std::filesystem::file_size(p, ec);
    if (ec) {
        if (why) *why = "cannot stat model file: " + path;
        return false;
    }
    bool sizeOk = false;
    if (const KnownModel* k = findKnown(path))
        sizeOk = (size == k->size);
    else
        sizeOk = (size == kKnownModels[0].size || size == kKnownModels[1].size);
    if (!sizeOk) {
        if (why) *why = "model file is incomplete or not a supported Demucs model (size " + std::to_string(size) + "): " + path;
        return false;
    }
#if defined(_WIN32)
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) {
        if (why) *why = "cannot open model file: " + path;
        return false;
    }
    uint32_t magic = 0;
    const size_t got = std::fread(&magic, sizeof(magic), 1, f);
    std::fclose(f);
    if (got != 1 || (magic != 0x646d6334 && magic != 0x646d6336)) { // dmc4 / dmc6
        if (why) *why = "not a Demucs v4 model (bad header): " + path;
        return false;
    }
    return true;
}

std::shared_ptr<const LoadedModel> getModel(const std::string& path, std::string* why)
{
    std::lock_guard<std::mutex> lock(gCacheMutex);
    auto it = gCache.find(path);
    if (it != gCache.end())
        return it->second;
    if (!checkModelFile(path, why))
        return nullptr;
    auto lm = std::make_shared<LoadedModel>();
    lm->model = std::make_unique<demucscpp::demucs_model>();
    // demucs.cpp opens files with fopen(); on Windows that is the ANSI code
    // page, so non-ASCII model paths may fail there (copy to an ASCII path).
    bool ok = false;
    try {
        ok = demucscpp::load_demucs_model(path, lm->model.get());
    } catch (const std::exception& e) {
        if (why) *why = std::string("model load failed: ") + e.what();
        return nullptr;
    }
    if (!ok) {
        if (why) *why = "model load failed: " + path;
        return nullptr;
    }
    gCache[path] = lm;
    return lm;
}

// The fine-tuned bag: four models, model i contributes stem i.
bool ftSiblings(const std::string& path, std::vector<std::string>& paths)
{
    const auto p = toPath(path);
    const std::string name = fromPath(p.filename());
    if (name.rfind("ggml-model-htdemucs_ft_", 0) != 0)
        return false;
    paths.clear();
    for (const char* stem : kFtStemOrder)
        paths.push_back(fromPath(p.parent_path() / ("ggml-model-htdemucs_ft_" + std::string(stem) + "-4s-f16.bin")));
    return true;
}

// ---------------------------------------------------------------- inference

// Demucs "apply_model" with shifts=1 (fixed zero offset, deterministic) and
// split=True: 7.8 s segments, 25 % overlap, triangular cross-fade. Segments
// run in parallel, one set of buffers per worker. Each output sample is
// covered by at most two segments, so the overlap-add is order independent;
// results match across thread counts to within GEMM rounding (~1e-7).
class SegmentRunner {
public:
    SegmentRunner(const demucscpp::demucs_model& model, const Eigen::MatrixXf& audio, int threads,
                  const std::function<bool(float)>& progress, float progLo, float progHi)
        : model_(model), audio_(audio), threads_(std::max(1, threads)), progress_(progress),
          progLo_(progLo), progHi_(progHi)
    {
        nbSources_ = model.is_4sources ? 4 : 6;
        segment_ = (int)(demucscpp::SEGMENT_LEN_SECS * kModelRate);
        stride_ = (int)((1.0f - demucscpp::OVERLAP) * segment_);
        length_ = (int)audio.cols();
        weight_.resize(segment_);
        weight_.setZero();
        weight_.head(segment_ / 2) = Eigen::VectorXf::LinSpaced(segment_ / 2, 1, (float)(segment_ / 2));
        weight_.tail(segment_ / 2) = weight_.head(segment_ / 2).reverse();
        weight_ /= weight_.maxCoeff();
        weight_ = weight_.array().pow(demucscpp::TRANSITION_POWER);
        for (int off = 0; off < length_; off += stride_)
            offsets_.push_back(off);
    }

    // returns false on cancel or error (error string set)
    bool run(Eigen::Tensor3dXf& out, std::string* error)
    {
        out = Eigen::Tensor3dXf(nbSources_, 2, length_);
        out.setZero();
        sumWeight_ = Eigen::VectorXf::Zero(length_);
        const int nChunks = (int)offsets_.size();
        partial_.reset(new std::atomic<float>[(size_t)threads_]);
        for (int i = 0; i < threads_; ++i)
            partial_[i].store(0.0f);

        const int nWorkers = std::min(threads_, nChunks);
        std::vector<std::thread> workers;
        workers.reserve((size_t)nWorkers);
        for (int w = 0; w < nWorkers; ++w)
            workers.emplace_back([this, w, &out] { worker(w, out); });

        // Coordinator: the calling thread reports progress and polls cancel.
        bool cancelledByUser = false;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            while (finishedWorkers_ < nWorkers) {
                stateCv_.wait_for(lock, std::chrono::milliseconds(50));
                if (progress_ && !cancel_.load()) {
                    float done = (float)completed_;
                    for (int i = 0; i < threads_; ++i)
                        done += partial_[i].load(std::memory_order_relaxed);
                    const float frac = std::clamp(done / (float)nChunks, 0.0f, 1.0f);
                    reported_ = std::max(reported_, progLo_ + (progHi_ - progLo_) * frac);
                    const float value = reported_;
                    lock.unlock();
                    const bool keepGoing = progress_(value);
                    lock.lock();
                    if (!keepGoing && !cancel_.load()) {
                        cancelledByUser = true;
                        cancel_.store(true);
                    }
                }
            }
        }
        for (auto& t : workers)
            t.join();

        if (cancelledByUser) {
            setError(error, "cancelled");
            return false;
        }
        if (!workerError_.empty()) {
            setError(error, workerError_);
            return false;
        }
        for (int s = 0; s < nbSources_; ++s)
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < length_; ++k)
                    out(s, c, k) /= sumWeight_(k);
        return true;
    }

    void cancel() { cancel_.store(true); }

private:
    void worker(int index, Eigen::Tensor3dXf& out)
    {
        enableFlushToZero();
        configureBlasForWorker();
        try {
            demucscpp::demucs_segment_buffers buffers(2, segment_, nbSources_);
            demucscpp::stft_buffers stftBuf(buffers.padded_segment_samples);
            demucscpp::ProgressCallback cb = [this, index](float p, const std::string&) {
                if (cancel_.load(std::memory_order_relaxed))
                    throw Cancelled{};
                partial_[index].store(std::clamp(p, 0.0f, 1.0f), std::memory_order_relaxed);
            };
            for (;;) {
                const int i = next_.fetch_add(1);
                if (i >= (int)offsets_.size() || cancel_.load())
                    break;
                const int offset = offsets_[(size_t)i];
                const int chunkLen = std::min(segment_, length_ - offset);

                // symmetric zero padding into buffers.mix
                const int total = segment_ - chunkLen;
                const int left = total / 2;
                buffers.mix.setZero();
                buffers.mix.block(0, left, 2, chunkLen) = audio_.block(0, offset, 2, chunkLen);

                partial_[index].store(0.0f, std::memory_order_relaxed);
                demucscpp::model_inference(model_, buffers, stftBuf, cb, 0.0f, 1.0f);

                {
                    std::lock_guard<std::mutex> lock(accumMutex_);
                    for (int s = 0; s < nbSources_; ++s)
                        for (int c = 0; c < 2; ++c)
                            for (int k = 0; k < chunkLen; ++k)
                                out(s, c, offset + k) += weight_(k) * buffers.targets_out(s, c, k + left);
                    for (int k = 0; k < chunkLen; ++k)
                        sumWeight_(offset + k) += weight_(k);
                }
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    partial_[index].store(0.0f, std::memory_order_relaxed);
                    ++completed_;
                }
                stateCv_.notify_all();
            }
        } catch (const Cancelled&) {
        } catch (const std::bad_alloc&) {
            fail("out of memory during stem separation");
        } catch (const std::exception& e) {
            fail(std::string("stem separation failed: ") + e.what());
        } catch (...) {
            fail("stem separation failed");
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++finishedWorkers_;
        }
        stateCv_.notify_all();
    }

    void fail(const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (workerError_.empty())
            workerError_ = msg;
        cancel_.store(true);
    }

    const demucscpp::demucs_model& model_;
    const Eigen::MatrixXf& audio_;
    int threads_;
    const std::function<bool(float)>& progress_;
    float progLo_, progHi_;
    int nbSources_ = 4, segment_ = 0, stride_ = 0, length_ = 0;
    Eigen::VectorXf weight_, sumWeight_;
    std::vector<int> offsets_;
    std::atomic<int> next_{0};
    std::atomic<bool> cancel_{false};
    std::mutex accumMutex_, stateMutex_;
    std::condition_variable stateCv_;
    int completed_ = 0, finishedWorkers_ = 0;
    std::unique_ptr<std::atomic<float>[]> partial_;
    float reported_ = 0.0f;
    std::string workerError_;
};

// Runs one model over the normalized, shifted mix. Returns [src][ch][n].
bool runModel(const demucscpp::demucs_model& model, const Eigen::MatrixXf& mix, int threads,
              const std::function<bool(float)>& progress, float lo, float hi, Eigen::Tensor3dXf& out,
              std::string* error)
{
    SegmentRunner runner(model, mix, threads, progress, lo, hi);
    return runner.run(out, error);
}

// Runs fn(i) for i in [0, n) on up to `threads` threads.
template <typename Fn>
void parallelFor(int n, int threads, Fn&& fn)
{
    threads = std::max(1, std::min(threads, n));
    if (threads == 1) {
        for (int i = 0; i < n; ++i)
            fn(i);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = next.fetch_add(1); i < n; i = next.fetch_add(1))
                fn(i);
        });
    for (auto& t : pool)
        t.join();
}

} // namespace

// ---------------------------------------------------------------- public API

const char* defaultModelFileName() { return kKnownModels[0].file; }

const char* defaultModelUrl() { return kDefaultModelUrl; }

const char* defaultModelSha256() { return kKnownModels[0].sha256; }

std::string defaultModelPath()
{
    std::filesystem::path base;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"))
        base = std::filesystem::u8path(appdata);
    else if (const char* profile = std::getenv("USERPROFILE"))
        base = std::filesystem::u8path(profile) / "AppData" / "Roaming";
    base = base / "Flayr Chop" / "models";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        base = home;
    base = base / "Library" / "Application Support" / "Flayr Chop" / "models";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"))
        base = std::filesystem::path(home) / ".local" / "share";
    base = base / "Flayr Chop" / "models";
#endif
    return fromPath(base / defaultModelFileName());
}

bool modelAvailable(const std::string& path)
{
    std::vector<std::string> ft;
    if (ftSiblings(path, ft)) {
        for (const auto& p : ft)
            if (!checkModelFile(p, nullptr))
                return false;
        return true;
    }
    return checkModelFile(path, nullptr);
}

int defaultThreadCount()
{
    // Each worker holds about 0.5-0.8 GB at peak; keep all workers within a
    // quarter of physical RAM.
    unsigned long long ram = 0;
    int cores = 0;
#if defined(__APPLE__)
    size_t len = sizeof(ram);
    sysctlbyname("hw.memsize", &ram, &len, nullptr, 0);
    // On Apple silicon the GEMMs run on the per-cluster matrix units, so
    // throughput stops improving after about 3 workers while memory keeps
    // growing (measured on an M3 Pro).
    int perf = 0;
    len = sizeof(perf);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &perf, &len, nullptr, 0) == 0 && perf > 0)
        cores = std::min(perf, 3);
#endif
    if (cores <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        cores = hc == 0 ? 4 : (int)(hc > 4 ? hc / 2 : hc); // assume SMT
    }
    if (ram > 0) {
        const int byRam = (int)((ram / 4) / (800ULL << 20));
        cores = std::min(cores, std::max(1, byRam));
    }
    return std::max(1, cores);
}

void releaseCachedModels()
{
    std::lock_guard<std::mutex> lock(gCacheMutex);
    gCache.clear(); // in-flight separations keep their shared_ptr alive
}

std::vector<float> resample(const float* in, int inLength, double inRate, double outRate, int outLength)
{
    std::vector<float> out((size_t)std::max(0, outLength), 0.0f);
    if (outLength <= 0)
        return out;
    detail::SincResampler rs(inRate, outRate);
    rs.process(in, inLength, out.data(), outLength);
    return out;
}

bool separate(const float* left, const float* right, int numSamples, double sampleRate,
              const std::string& modelPath, Separated& out,
              const std::function<bool(float)>& progress, int numThreads, std::string* error)
{
    out = Separated{};
    if (error)
        error->clear();
    if (!left || !right || numSamples < 0) {
        setError(error, "invalid input buffers");
        return false;
    }
    if (!(sampleRate >= 1000.0 && sampleRate <= 1.0e6)) {
        setError(error, "unsupported sample rate");
        return false;
    }
    const int threads = numThreads > 0 ? numThreads : defaultThreadCount();
    auto report = [&](float p) { return progress ? progress(std::clamp(p, 0.0f, 1.0f)) : true; };

    // ---- models
    std::vector<std::string> paths;
    const bool fineTuned = ftSiblings(modelPath, paths);
    if (!fineTuned)
        paths = {modelPath};
    std::vector<std::shared_ptr<const LoadedModel>> models;
    for (const auto& p : paths) {
        std::string why;
        auto m = getModel(p, &why);
        if (!m) {
            setError(error, why);
            return false;
        }
        models.push_back(std::move(m));
    }
    const bool six = !models[0]->model->is_4sources;
    out.names = six ? std::vector<std::string>{"Drums", "Bass", "Other", "Vocals", "Guitar", "Piano"}
                    : std::vector<std::string>{"Drums", "Bass", "Other", "Vocals"};
    const int nbSources = (int)out.names.size();

    if (numSamples == 0) {
        out.audio.resize((size_t)nbSources);
        return true;
    }
    if (!report(0.0f)) {
        out = Separated{};
        setError(error, "cancelled");
        return false;
    }

    // ---- resample to 44.1 kHz
    const bool needResample = std::abs(sampleRate - kModelRate) > 1e-6;
    const int modelLen = needResample ? (int)std::llround((double)numSamples * kModelRate / sampleRate) : numSamples;
    if (modelLen <= 0) {
        out.audio.assign((size_t)nbSources, {std::vector<float>((size_t)numSamples, 0.0f),
                                             std::vector<float>((size_t)numSamples, 0.0f)});
        return true;
    }
    std::array<std::vector<float>, 2> modelIn;
    {
        const float* src[2] = {left, right};
        const bool mono = (left == right);
        parallelFor(mono ? 1 : 2, threads, [&](int c) {
            modelIn[(size_t)c] = needResample ? resample(src[c], numSamples, sampleRate, kModelRate, modelLen)
                                              : std::vector<float>(src[c], src[c] + numSamples);
        });
        if (mono)
            modelIn[1] = modelIn[0];
    }

    // ---- normalize like demucs: (wav - mean(ref)) / std(ref), ref = mono mix
    double sum = 0.0;
    for (int i = 0; i < modelLen; ++i)
        sum += 0.5 * ((double)modelIn[0][(size_t)i] + modelIn[1][(size_t)i]);
    const double mean = sum / modelLen;
    double ss = 0.0;
    for (int i = 0; i < modelLen; ++i) {
        const double d = 0.5 * ((double)modelIn[0][(size_t)i] + modelIn[1][(size_t)i]) - mean;
        ss += d * d;
    }
    const double stdev = modelLen > 1 ? std::sqrt(ss / (modelLen - 1)) : 0.0;
    if (!(stdev > 1e-7)) {
        // digital silence (or a constant): every stem is silent
        out.audio.assign((size_t)nbSources, {std::vector<float>((size_t)numSamples, 0.0f),
                                             std::vector<float>((size_t)numSamples, 0.0f)});
        report(1.0f);
        return true;
    }

    // ---- shift padding (demucs shifts=1 with a fixed offset of 0): the mix
    // is preceded by max_shift samples of silence so the first segment edge
    // does not sit on the start of the song.
    const int maxShift = (int)(demucscpp::MAX_SHIFT_SECS * kModelRate);
    const int shiftedLen = modelLen + maxShift;
    Eigen::MatrixXf mix = Eigen::MatrixXf::Zero(2, shiftedLen);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < modelLen; ++i)
            mix(c, maxShift + i) = (float)((modelIn[(size_t)c][(size_t)i] - mean) / stdev);
    modelIn = {};

    // ---- network
    const float netLo = 0.02f, netHi = 0.95f;
    Eigen::Tensor3dXf result;
    if (!fineTuned) {
        if (!runModel(*models[0]->model, mix, threads, progress, netLo, netHi, result, error)) {
            out = Separated{};
            return false;
        }
    } else {
        result = Eigen::Tensor3dXf(4, 2, shiftedLen);
        const float span = (netHi - netLo) / 4.0f;
        for (int m = 0; m < 4; ++m) {
            Eigen::Tensor3dXf r;
            if (!runModel(*models[(size_t)m]->model, mix, threads, progress, netLo + span * m,
                          netLo + span * (m + 1), r, error)) {
                out = Separated{};
                return false;
            }
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < shiftedLen; ++k)
                    result(m, c, k) = r(m, c, k);
        }
    }
    mix.resize(0, 0);

    // ---- denormalize, drop the shift, resample back to the input rate
    out.audio.resize((size_t)nbSources);
    parallelFor(nbSources * 2, threads, [&](int job) {
        const int s = job / 2, c = job % 2;
        std::vector<float> stem((size_t)modelLen);
        for (int i = 0; i < modelLen; ++i)
            stem[(size_t)i] = (float)(result(s, c, maxShift + i) * stdev + mean);
        out.audio[(size_t)s][(size_t)c] =
            needResample ? resample(stem.data(), modelLen, kModelRate, sampleRate, numSamples) : std::move(stem);
    });
    if (!report(1.0f)) {
        out = Separated{};
        setError(error, "cancelled");
        return false;
    }
    return true;
}

} // namespace chop::stems
