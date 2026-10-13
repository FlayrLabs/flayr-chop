// stems_test: invariants of the Flayr Chop stem separator.
//
// Always runs: resampler quality and length, model path helpers, error paths.
// With a model (FLAYR_CHOP_STEMS_MODEL env var, else defaultModelPath()):
// output length and stem count at several rates, mono input, empty and
// silent input, determinism across thread counts, cancellation. Without a
// model those checks are skipped (reported, exit code 0).

#include "stems/Stems.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double rate, int n, float amp = 0.5f)
{
    std::vector<float> v((size_t)n);
    for (int i = 0; i < n; ++i)
        v[(size_t)i] = amp * (float)std::sin(2.0 * kPi * freq * i / rate);
    return v;
}

double rmsDb(const std::vector<float>& v, size_t a, size_t b)
{
    double e = 0;
    for (size_t i = a; i < b; ++i)
        e += (double)v[i] * v[i];
    return 10.0 * std::log10(e / double(b - a) + 1e-30);
}

void testResampler()
{
    using chop::stems::resample;
    // exact output lengths
    const int n = 48001;
    auto x = sine(1000.0, 48000.0, n);
    for (double out : {44100.0, 22050.0, 96000.0, 44100.0 * 1.0001}) {
        const int m = (int)std::llround(n * out / 48000.0);
        check((int)resample(x.data(), n, 48000.0, out, m).size() == m,
              "resample length 48000 -> " + std::to_string(out));
    }

    // round trip 48k -> 44.1k -> 48k of a 1 kHz tone (interior, edges excluded)
    for (double mid : {44100.0, 44100.0 * 1.0001}) {
        const int m = (int)std::llround(n * mid / 48000.0);
        auto y = resample(x.data(), n, 48000.0, mid, m);
        auto z = resample(y.data(), m, mid, 48000.0, n);
        double e = 0, r = 0;
        for (int i = 4800; i < n - 4800; ++i) {
            e += double(z[(size_t)i] - x[(size_t)i]) * (z[(size_t)i] - x[(size_t)i]);
            r += double(x[(size_t)i]) * x[(size_t)i];
        }
        const double snr = 10.0 * std::log10(r / (e + 1e-30));
        char buf[128];
        std::snprintf(buf, sizeof buf, "round trip 48k -> %.1f -> 48k SNR %.1f dB (> 90)", mid, snr);
        check(snr > 90.0, buf);
    }

    // anti-aliasing: 30 kHz at 96 kHz must vanish at 44.1 kHz
    {
        auto hi = sine(30000.0, 96000.0, 96000);
        auto d = resample(hi.data(), 96000, 96000.0, 44100.0, 44100);
        const double db = rmsDb(d, 2000, 42000) - rmsDb(hi, 2000, 94000);
        char buf[128];
        std::snprintf(buf, sizeof buf, "30 kHz tone rejected by %.1f dB going to 44.1k (> 90)", -db);
        check(db < -90.0, buf);
    }
    // passband: 18 kHz at 48 kHz keeps its level at 44.1 kHz
    {
        auto t = sine(18000.0, 48000.0, 48000);
        auto d = resample(t.data(), 48000, 48000.0, 44100.0, 44100);
        const double db = rmsDb(d, 2000, 42000) - rmsDb(t, 2000, 46000);
        check(std::fabs(db) < 0.05, "18 kHz passband gain within 0.05 dB");
    }
}

void testHelpers(const std::filesystem::path& tmp)
{
    const std::string p = chop::stems::defaultModelPath();
    check(p.find("Flayr Chop") != std::string::npos &&
              p.size() > std::string(chop::stems::defaultModelFileName()).size() &&
              p.compare(p.size() - std::string(chop::stems::defaultModelFileName()).size(), std::string::npos,
                        chop::stems::defaultModelFileName()) == 0,
          "defaultModelPath(): " + p);
    check(!chop::stems::modelAvailable((tmp / "does-not-exist.bin").string()), "modelAvailable(missing) is false");

    const auto junk = tmp / "ggml-model-htdemucs-4s-f16.bin";
    {
        std::ofstream f(junk, std::ios::binary);
        f << "definitely not a model";
    }
    check(!chop::stems::modelAvailable(junk.string()), "modelAvailable(truncated file) is false");

    std::vector<float> a(44100, 0.1f);
    chop::stems::Separated out;
    std::string err;
    const bool ok = chop::stems::separate(a.data(), a.data(), (int)a.size(), 44100.0, junk.string(), out,
                                          nullptr, 2, &err);
    check(!ok && !err.empty(), "separate() with a bad model fails cleanly: " + err);
    check(!chop::stems::separate(nullptr, a.data(), 10, 44100.0, junk.string(), out, nullptr, 1, &err),
          "separate() rejects null input");
    std::filesystem::remove(junk);
}

void testWithModel(const std::string& model)
{
    std::mt19937 rng(1);
    std::normal_distribution<float> g(0.0f, 0.1f);
    auto makeMix = [&](double rate, int n) {
        std::array<std::vector<float>, 2> s;
        for (int c = 0; c < 2; ++c) {
            s[(size_t)c].resize((size_t)n);
            for (int i = 0; i < n; ++i) {
                const double t = i / rate;
                const float kick = (float)(std::sin(2 * kPi * 55.0 * t) * std::exp(-std::fmod(t, 0.5) * 12.0));
                s[(size_t)c][(size_t)i] = 0.4f * kick + 0.2f * (float)std::sin(2 * kPi * (330.0 + 2 * c) * t) +
                                          (std::fmod(t, 0.25) < 0.02 ? g(rng) : 0.0f);
            }
        }
        return s;
    };

    // length + stem count at awkward rates and lengths (two segments at 48k)
    for (auto [rate, n] : {std::pair<double, int>{48000.0, 48000 * 9 + 7}, {22050.0, 22050 * 3 + 1},
                           {44100.0, 12345}, {96000.0, 96000 * 2 + 3}}) {
        auto mix = makeMix(rate, n);
        chop::stems::Separated out;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = chop::stems::separate(mix[0].data(), mix[1].data(), n, rate, model, out, nullptr, 0, &err);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        bool lengths = ok && out.audio.size() == out.names.size() && (out.names.size() == 4 || out.names.size() == 6);
        bool finite = true;
        for (auto& st : out.audio)
            for (auto& ch : st) {
                lengths = lengths && (int)ch.size() == n;
                for (float v : ch)
                    finite = finite && std::isfinite(v);
            }
        char buf[160];
        std::snprintf(buf, sizeof buf, "separate %d samples @ %.0f Hz: %zu stems, exact length, finite (%.1f s)%s%s", n,
                      rate, out.names.size(), secs, ok ? "" : " error: ", err.c_str());
        check(ok && lengths && finite, buf);

        if (ok && rate == 48000.0) {
            // stems should add back up to (roughly) the mix
            double e = 0, r = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) {
                    double s = 0;
                    for (auto& st : out.audio)
                        s += st[(size_t)c][(size_t)i];
                    e += (s - mix[(size_t)c][(size_t)i]) * (s - mix[(size_t)c][(size_t)i]);
                    r += (double)mix[(size_t)c][(size_t)i] * mix[(size_t)c][(size_t)i];
                }
            std::snprintf(buf, sizeof buf, "sum of stems matches the mix: %.1f dB (> 15)", 10 * std::log10(r / e));
            check(10 * std::log10(r / e) > 15.0, buf);
        }
    }

    // mono (same pointer twice): both output channels identical length
    {
        auto mix = makeMix(44100.0, 44100 * 2);
        chop::stems::Separated out;
        std::string err;
        const bool ok = chop::stems::separate(mix[0].data(), mix[0].data(), 44100 * 2, 44100.0, model, out, nullptr, 2, &err);
        bool same = ok;
        for (auto& st : out.audio)
            same = same && st[0].size() == st[1].size() && st[0].size() == (size_t)(44100 * 2);
        check(same, "mono input via the same pointer twice");
    }

    // empty and silent input
    {
        chop::stems::Separated out;
        std::string err;
        float dummy = 0.0f;
        const bool ok = chop::stems::separate(&dummy, &dummy, 0, 48000.0, model, out, nullptr, 1, &err);
        bool empty = ok && !out.audio.empty();
        for (auto& st : out.audio)
            empty = empty && st[0].empty() && st[1].empty();
        check(empty, "zero-length input gives empty stems");

        std::vector<float> z(48000 * 2, 0.0f);
        const bool ok2 = chop::stems::separate(z.data(), z.data(), (int)z.size(), 48000.0, model, out, nullptr, 1, &err);
        bool silent = ok2;
        for (auto& st : out.audio)
            for (auto& ch : st) {
                silent = silent && ch.size() == z.size();
                for (float v : ch)
                    silent = silent && v == 0.0f;
            }
        check(silent, "digital silence gives silent stems of the right length");
    }

    // determinism across thread counts (three segments)
    {
        const int n = 44100 * 14;
        auto mix = makeMix(44100.0, n);
        chop::stems::Separated a, b;
        std::string err;
        const bool okA = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, a, nullptr, 1, &err);
        const bool okB = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, b, nullptr, 3, &err);
        // Same segmentation and overlap-add for any thread count; GEMM
        // kernels may round differently with buffer alignment, so allow a
        // few float ulps.
        bool same = okA && okB && a.audio.size() == b.audio.size();
        double maxDiff = 0.0;
        for (size_t s = 0; same && s < a.audio.size(); ++s)
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < a.audio[s][(size_t)c].size(); ++i)
                    maxDiff = std::max(maxDiff, (double)std::fabs(a.audio[s][(size_t)c][i] - b.audio[s][(size_t)c][i]));
        char buf[128];
        std::snprintf(buf, sizeof buf, "same output with 1 and 3 threads (max diff %.2g < 1e-5)", maxDiff);
        check(same && maxDiff < 1e-5, buf);
    }

    // cancellation: immediately, and part way through
    {
        const int n = 44100 * 30;
        auto mix = makeMix(44100.0, n);
        chop::stems::Separated out;
        std::string err;
        bool ok = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, out,
                                        [](float) { return false; }, 2, &err);
        check(!ok && err == "cancelled" && out.audio.empty(), "cancel on the first progress call");

        std::atomic<float> last{0.0f};
        int calls = 0;
        bool monotonic = true;
        float prev = -1.0f;
        const auto t0 = std::chrono::steady_clock::now();
        double cancelAt = 0;
        ok = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, out,
                                   [&](float p) {
                                       ++calls;
                                       monotonic = monotonic && p >= prev && p >= 0.0f && p <= 1.0f;
                                       prev = p;
                                       last = p;
                                       if (p > 0.15f && cancelAt == 0)
                                           cancelAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                                       return p <= 0.15f;
                                   },
                                   2, &err);
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "cancel part way: returned false after %.2f s (asked at %.2f s), %d progress calls, monotonic", total,
                      cancelAt, calls);
        check(!ok && err == "cancelled" && out.audio.empty() && monotonic && calls > 2 && total - cancelAt < 2.0, buf);
    }
}

} // namespace

int main()
{
    const auto tmp = std::filesystem::temp_directory_path() / "flayr_chop_stems_test";
    std::filesystem::create_directories(tmp);

    testResampler();
    testHelpers(tmp);

    std::string model = chop::stems::defaultModelPath();
    if (const char* env = std::getenv("FLAYR_CHOP_STEMS_MODEL"); env && *env)
        model = env;
    if (chop::stems::modelAvailable(model)) {
        std::printf("model: %s\n", model.c_str());
        testWithModel(model);
    } else {
        std::printf("SKIP  model-dependent checks (no model at %s; run scripts/fetch-model.sh)\n", model.c_str());
    }
    chop::stems::releaseCachedModels();
    std::filesystem::remove_all(tmp);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
