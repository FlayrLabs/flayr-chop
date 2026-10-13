// Engine implementation shared by every instruction-set build (no include
// guard on purpose). The including file defines FLAYR_STEMS_ENGINE_NS and has
// already included the demucs.cpp sources or headers, possibly with the
// Eigen / demucscpp namespaces renamed by macros (see EngineAvx2.cpp).

#include "stems/Engine.h"

namespace chop::stems::detail::FLAYR_STEMS_ENGINE_NS {

struct Worker {
    demucscpp::demucs_segment_buffers buffers;
    demucscpp::stft_buffers stft;
    Worker(int segmentSamples, int nbSources)
        : buffers(2, segmentSamples, nbSources), stft(buffers.padded_segment_samples)
    {
    }
};

inline std::size_t mixHash(std::size_t h, std::size_t v)
{
    return (h ^ v) * (std::size_t)1099511628211ULL;
}

std::size_t layoutSignature()
{
    std::size_t h = 1469598103934665603ULL & ~(std::size_t)0;
    h = mixHash(h, sizeof(demucscpp::demucs_model));
    h = mixHash(h, sizeof(demucscpp::crosstransformer_base));
    h = mixHash(h, sizeof(demucscpp::demucs_crosstransformer_4s));
    h = mixHash(h, alignof(demucscpp::demucs_model));
    h = mixHash(h, sizeof(Eigen::MatrixXf));
    h = mixHash(h, sizeof(Eigen::VectorXf));
    h = mixHash(h, sizeof(Eigen::Tensor1dXf));
    h = mixHash(h, sizeof(Eigen::Tensor3dXf));
    h = mixHash(h, sizeof(Eigen::Tensor4dXf));
    h = mixHash(h, (std::size_t)EIGEN_MAX_ALIGN_BYTES);
    h = mixHash(h, (std::size_t)EIGEN_DEFAULT_ALIGN_BYTES);
    h = mixHash(h, (std::size_t)EIGEN_MAX_STATIC_ALIGN_BYTES);
    h = mixHash(h, (std::size_t)EIGEN_MALLOC_ALREADY_ALIGNED);
    return h;
}

void* createWorker(int segmentSamples, int nbSources)
{
    return new Worker(segmentSamples, nbSources);
}

void destroyWorker(void* w)
{
    delete static_cast<Worker*>(w);
}

float* workerMix(void* w)
{
    return static_cast<Worker*>(w)->buffers.mix.data();
}

const float* workerTargets(void* w)
{
    return static_cast<Worker*>(w)->buffers.targets_out.data();
}

void run(void* w, const void* model, const std::function<void(float)>& progress)
{
    auto* worker = static_cast<Worker*>(w);
    demucscpp::ProgressCallback cb = [&progress](float p, const std::string&) {
        if (progress)
            progress(p);
    };
    demucscpp::model_inference(*static_cast<const demucscpp::demucs_model*>(model), worker->buffers,
                               worker->stft, cb, 0.0f, 1.0f);
}

const EngineOps& ops()
{
    static const EngineOps o{FLAYR_STEMS_ENGINE_NAME, &layoutSignature, &createWorker, &destroyWorker,
                             &workerMix, &workerTargets, &run};
    return o;
}

} // namespace chop::stems::detail::FLAYR_STEMS_ENGINE_NS
