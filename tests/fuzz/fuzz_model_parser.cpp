// Fuzz target: the stem model's header and tensor parser, from memory. The plug-in only ever parses a
// file whose SHA-256 matches a known release; this harness calls the parser directly (the same
// function separate() calls after the checksum) so the parser itself is what gets fuzzed.
//
// Input: a model image (magic, then tensor headers and float16 payloads).
//
// Speed: building a demucs_model allocates ~170 MB of weights, which under ASan costs more than the
// parse itself, so by default each run parses into one of two preallocated models (one per magic:
// the 6-source magic reshapes a few tensors, the same way every time, so shapes, and so the parse,
// don't depend on earlier inputs). FUZZ_FRESH_MODEL=1 goes through parseModelImage() instead, a new
// model per input, exactly as the plug-in does (use it to confirm a finding).

#include "stems/Stems.h"
#include "model.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;

struct Image
{
    Bytes b;
    Image& i32 (int32_t v)
    {
        const auto* p = reinterpret_cast<const uint8_t*> (&v);
        b.insert (b.end(), p, p + 4);
        return *this;
    }
    Image& tensor (const std::string& name, std::vector<int32_t> dims, uint16_t fill = 0x3c00) // 1.0 in float16
    {
        i32 ((int32_t) dims.size()).i32 ((int32_t) name.size());
        size_t n = 1;
        for (auto d : dims)
        {
            i32 (d);
            n *= (size_t) d;
        }
        b.insert (b.end(), name.begin(), name.end());
        for (size_t i = 0; i < n; ++i)
        {
            b.push_back ((uint8_t) fill);
            b.push_back ((uint8_t) (fill >> 8));
        }
        return *this;
    }
};

// Real tensor names and shapes from the htdemucs layout (small ones, so seeds stay tiny).
std::vector<Bytes> makeSeeds()
{
    std::vector<Bytes> seeds;
    for (const uint32_t magic : { 0x646d6334u, 0x646d6336u }) // dmc4, dmc6
    {
        Image m;
        m.i32 ((int32_t) magic);
        seeds.push_back (m.b);
        m.tensor ("encoder.0.conv.bias", { 48 });
        seeds.push_back (m.b);
        m.tensor ("encoder.1.conv.bias", { 96 }).tensor ("tencoder.0.conv.bias", { 48 });
        seeds.push_back (m.b);
        Image t;
        t.i32 ((int32_t) magic);
        t.tensor ("crosstransformer.norm_in.weight", { 512 }).tensor ("encoder.0.dconv.layers.0.1.weight", { 12 });
        seeds.push_back (t.b);
        Image bad; // a wrong shape for a real name, and an unknown name
        bad.i32 ((int32_t) magic).tensor ("encoder.0.conv.bias", { 2, 24 }).tensor ("nope", { 4 });
        seeds.push_back (bad.b);
    }

    // The start of the real model, when it is on this machine (read only; never committed). Cut at
    // several sizes, so whole tensors and truncated ones are both in the corpus.
    const auto model = chop::stems::defaultModelPath();
    std::error_code ec;
    if (! model.empty() && std::filesystem::exists (model, ec))
    {
        std::ifstream in (model, std::ios::binary);
        Bytes head (256 * 1024);
        in.read (reinterpret_cast<char*> (head.data()), (std::streamsize) head.size());
        head.resize ((size_t) in.gcount());
        for (size_t cut : { (size_t) 4096, (size_t) 16384, (size_t) 65536, head.size() })
            seeds.push_back (Bytes (head.begin(), head.begin() + (long) std::min (cut, head.size())));
    }
    return seeds;
}
} // namespace

extern "C" int LLVMFuzzerInitialize (int* argc, char*** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::setvbuf (stderr, nullptr, _IONBF, 0);
    for (int i = 1; i < *argc; ++i)
        if (std::strncmp ((*argv)[i], "-write_seeds=", 13) == 0)
        {
            const std::filesystem::path dir ((*argv)[i] + 13);
            std::filesystem::create_directories (dir);
            int n = 0;
            for (const auto& s : makeSeeds())
            {
                char name[32];
                std::snprintf (name, sizeof (name), "seed-%03d", n++);
                std::ofstream (dir / name, std::ios::binary).write (reinterpret_cast<const char*> (s.data()), (std::streamsize) s.size());
            }
            std::printf ("wrote %d seeds to %s\n", n, dir.string().c_str());
            std::exit (0);
        }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    static const bool fresh = std::getenv ("FUZZ_FRESH_MODEL") != nullptr;
    if (fresh)
    {
        std::string error;
        const bool ok = chop::stems::parseModelImage (data, size, &error);
        if (! ok && error.empty())
        {
            std::fprintf (stderr, "refused without an error message\n");
            std::abort();
        }
        return 0;
    }
    static auto* model4 = new demucscpp::demucs_model();
    static auto* model6 = new demucscpp::demucs_model();
    uint32_t magic = 0;
    if (size >= 4)
        std::memcpy (&magic, data, 4);
    try
    {
        demucscpp::load_demucs_model_from_memory (data, size, magic == 0x646d6336u ? model6 : model4);
    }
    catch (const std::exception&) // parseModelImage turns these into an error message
    {
    }
    return 0;
}
