// stems_test: invariants of the Flayr Chop stem separator.
//
// Always runs: resampler quality and length (and range processing), model
// path helpers, error paths, engine selection API.
// With a model (FLAYR_CHOP_STEMS_MODEL env var, else defaultModelPath()):
// output length and stem count at several rates, mono input, empty and
// silent input, determinism across thread counts, cancellation, every
// network engine this CPU can run agreeing with the generic one, and the
// progressive-results overload. Without a model those checks are skipped
// (reported, exit code 0).

#include "stems/Stems.h"
#include "stems/Resampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // processRange() gives exactly the samples process() gives, for any
    // sub-range (the progressive output path resamples range by range)
    for (auto [from, to] : {std::pair<double, double>{44100.0, 48000.0}, {44100.0, 96000.0},
                            {44100.0, 44100.0 * 1.0001}, {44100.0, 44100.0}}) {
        const int inLen = 44100;
        auto src = sine(440.0, from, inLen);
        const int outLen = (int)std::llround(inLen * to / from);
        chop::stems::detail::SincResampler rs(from, to);
        std::vector<float> whole((size_t)outLen), parts((size_t)outLen);
        rs.process(src.data(), inLen, whole.data(), outLen);
        for (int a = 0; a < outLen;) {
            const int b = std::min(outLen, a + 997 + (a % 13) * 101);
            rs.processRange(src.data(), inLen, parts.data() + a, a, b);
            a = b;
        }
        check(whole == parts, "resampler processRange matches process for " + std::to_string((int)from) + " -> " +
                                  std::to_string(to));
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

// Model files are untrusted: the checksum is checked on every load and the
// parser survives malformed headers.
void testModelIntegrity(const std::filesystem::path& tmp)
{
    check(chop::stems::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256(\"abc\")");
    check(chop::stems::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256(\"\")");
    {
        std::string million(1000000, 'a');
        check(chop::stems::sha256Hex(million.data(), million.size()) ==
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "sha256(1M x 'a')");
    }

    // Right size, right magic, wrong bytes: passes the cheap check, refused before parsing.
    const auto fake = tmp / "ggml-model-htdemucs-4s-f16.bin";
    {
        std::ofstream f(fake, std::ios::binary);
        const uint32_t magic = 0x646d6334;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        std::vector<char> zeros(1 << 20, 0);
        long long left = 83994361LL - 4;
        while (left > 0) {
            const auto n = (std::streamsize)std::min<long long>(left, (long long)zeros.size());
            f.write(zeros.data(), n);
            left -= n;
        }
    }
    check(chop::stems::modelAvailable(fake.string()), "right-size fake model passes the cheap check");
    {
        std::vector<float> a(44100, 0.1f);
        chop::stems::Separated out;
        std::string err;
        const bool ok = chop::stems::separate(a.data(), a.data(), (int)a.size(), 44100.0, fake.string(), out, nullptr, 1, &err);
        check(!ok && err.find("checksum") != std::string::npos, "tampered right-size model refused: " + err);
    }
    std::filesystem::remove(fake);

    // Hand-made images for the parser (no checksum on this path).
    struct Img {
        std::vector<unsigned char> b;
        Img& i32(int32_t v) { const auto* p = reinterpret_cast<const unsigned char*>(&v); b.insert(b.end(), p, p + 4); return *this; }
        Img& str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return *this; }
        Img& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
    };
    auto image = [] { Img m; m.i32(0x646d6334); return m; };
    auto refused = [](const Img& m) { std::string e; return !chop::stems::parseModelImage(m.b.data(), m.b.size(), &e); };
    check(refused(image()), "parser: no tensors");
    check(refused(image().i32(100).i32(4).str("abcd")), "parser: n_dims 100 (was a stack overflow)");
    check(refused(image().i32(5).i32(4).i32(1).i32(1).i32(1).i32(1).i32(1).str("abcd")), "parser: n_dims 5");
    check(refused(image().i32(0).i32(4).str("abcd")), "parser: n_dims 0");
    check(refused(image().i32(1).i32(1 << 30).i32(4)), "parser: name length 2^30");
    check(refused(image().i32(1).i32(-4).i32(4)), "parser: negative name length");
    check(refused(image().i32(2).i32(4).i32(-3).i32(5).str("abcd")), "parser: negative dimension");
    check(refused(image().i32(4).i32(4).i32(65536).i32(65536).i32(65536).i32(65536).str("abcd")),
          "parser: element count overflow");
    check(refused(image().i32(1).i32(22).i32(48).str("encoder.0.conv.bias")), "parser: truncated header");
    check(refused(image().i32(1).i32(19).i32(48).str("encoder.0.conv.bias").zeros(10)), "parser: truncated payload");
    check(refused(image().i32(2).i32(19).i32(24).i32(2).str("encoder.0.conv.bias").zeros(96)),
          "parser: right element count, wrong shape");
    check(refused(image().i32(1).i32(4).i32(8).str("nope").zeros(16)), "parser: unknown tensor");
    // Random headers: nothing may crash (run under ASan to see out-of-bounds reads).
    uint32_t seed = 12345;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed; };
    int fuzzRefused = 0;
    for (int k = 0; k < 2000; ++k) {
        Img m = image();
        const int words = 1 + (int)(rnd() % 12);
        for (int w = 0; w < words; ++w)
            m.i32((rnd() & 1) ? (int32_t)(rnd() % 64) - 8 : (int32_t)rnd());
        fuzzRefused += refused(m) ? 1 : 0;
    }
    check(fuzzRefused == 2000, "parser: 2000 random headers refused (" + std::to_string(fuzzRefused) + ")");
}

// The real model: the hardened parser still accepts it, and a single flipped byte is refused.
void testRealModelIntegrity(const std::string& model, const std::filesystem::path& tmp)
{
    std::ifstream in(model, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    check(chop::stems::parseModelImage(bytes.data(), bytes.size(), &err), "real model parses: " + err);
    check(chop::stems::sha256Hex(bytes.data(), bytes.size()) == chop::stems::defaultModelSha256() ||
              std::filesystem::path(model).filename() != chop::stems::defaultModelFileName(),
          "real model matches its pinned SHA-256");

    bytes[bytes.size() / 2] ^= 0x01;
    const auto copy = tmp / std::filesystem::path(model).filename();
    {
        std::ofstream out(copy, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }
    std::vector<float> a(44100, 0.1f);
    chop::stems::Separated sep;
    const bool ok = chop::stems::separate(a.data(), a.data(), (int)a.size(), 44100.0, copy.string(), sep, nullptr, 1, &err);
    check(!ok && err.find("checksum") != std::string::npos, "one flipped byte in the real model is refused: " + err);
    std::filesystem::remove(copy);
}

void testEngineApi()
{
    const auto engines = chop::stems::availableEngines();
    std::string list;
    for (const auto& e : engines)
        list += (list.empty() ? "" : ", ") + e;
    check(!engines.empty() && engines.back() == "generic", "available engines: " + list);
    check(engines.front() == chop::stems::activeEngine(),
          std::string("automatic engine is the fastest available: ") + chop::stems::activeEngine());
    check(!chop::stems::forceEngine("no-such-engine"), "forceEngine rejects unknown names");
    for (const auto& e : engines) {
        const bool ok = chop::stems::forceEngine(e);
        check(ok && e == chop::stems::activeEngine(), "forceEngine(\"" + e + "\") takes effect");
    }
    check(chop::stems::forceEngine("") && engines.front() == chop::stems::activeEngine(),
          "forceEngine(\"\") restores automatic selection");
}

double snrDb(const chop::stems::Separated& ref, const chop::stems::Separated& test)
{
    double e = 0, r = 0;
    for (size_t s = 0; s < ref.audio.size() && s < test.audio.size(); ++s)
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < ref.audio[s][(size_t)c].size() && i < test.audio[s][(size_t)c].size(); ++i) {
                const double d = (double)ref.audio[s][(size_t)c][i] - test.audio[s][(size_t)c][i];
                e += d * d;
                r += (double)ref.audio[s][(size_t)c][i] * ref.audio[s][(size_t)c][i];
            }
    return 10.0 * std::log10((r + 1e-30) / (e + 1e-30));
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

    // every engine this CPU can run (e.g. "avx2" on x86-64) matches the
    // portable one on a short input
    {
        const int n = 44100 * 9;
        auto mix = makeMix(44100.0, n);
        const auto engines = chop::stems::availableEngines();
        chop::stems::Separated ref;
        std::string err;
        chop::stems::forceEngine("generic");
        const bool okRef = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, ref, nullptr, 2, &err);
        check(okRef, "generic engine separates");
        for (const auto& e : engines) {
            if (e == "generic")
                continue;
            chop::stems::forceEngine(e);
            chop::stems::Separated out;
            const bool ok = chop::stems::separate(mix[0].data(), mix[1].data(), n, 44100.0, model, out, nullptr, 2, &err);
            const double snr = ok && okRef ? snrDb(ref, out) : -1.0;
            char buf[160];
            std::snprintf(buf, sizeof buf, "engine \"%s\" matches \"generic\": %.1f dB SNR (> 60)", e.c_str(), snr);
            check(ok && snr > 60.0, buf);
        }
        chop::stems::forceEngine("");
        if (engines.size() == 1)
            std::printf("note  only the generic engine runs on this CPU\n");
    }

    // progressive results: ranges in order, gap-free, final when reported,
    // and the same output as the plain overload
    {
        const int n = 48000 * 20 + 11; // four segments, with resampling
        auto mix = makeMix(48000.0, n);
        chop::stems::Separated plain, prog;
        std::string err;
        const bool okPlain = chop::stems::separate(mix[0].data(), mix[1].data(), n, 48000.0, model, plain, nullptr, 3, &err);
        int expected = 0, calls = 0;
        bool ordered = true, sized = true;
        std::vector<std::vector<float>> snapshot((size_t)8);
        const bool okProg = chop::stems::separate(
            mix[0].data(), mix[1].data(), n, 48000.0, model, prog, nullptr, 3, &err, [&](int start, int end) {
                ordered = ordered && start == expected && end > start && end <= n;
                expected = end;
                ++calls;
                sized = sized && prog.audio.size() == prog.names.size();
                for (size_t s = 0; sized && s < prog.audio.size() && s < 4; ++s)
                    for (int c = 0; c < 2; ++c) {
                        auto& ch = prog.audio[s][(size_t)c];
                        sized = sized && (int)ch.size() == n;
                        if (sized)
                            snapshot[s * 2 + (size_t)c].insert(snapshot[s * 2 + (size_t)c].end(), ch.begin() + start,
                                                               ch.begin() + end);
                    }
            });
        bool unchanged = okProg && sized;
        for (size_t s = 0; unchanged && s < prog.audio.size() && s < 4; ++s)
            for (int c = 0; c < 2; ++c)
                unchanged = unchanged && snapshot[s * 2 + (size_t)c] == prog.audio[s][(size_t)c];
        char buf[200];
        std::snprintf(buf, sizeof buf, "progressive: %d ranges in order covering all %d samples, final when reported", calls,
                      n);
        check(okProg && ordered && expected == n && calls >= 2 && unchanged, buf);
        const double snr = okPlain && okProg ? snrDb(plain, prog) : -1.0;
        std::snprintf(buf, sizeof buf, "progressive output equals the plain overload (%.1f dB SNR > 120)", snr);
        check(snr > 120.0, buf);

        int silentCalls = 0;
        std::vector<float> z(48000, 0.0f);
        const bool okSilent = chop::stems::separate(z.data(), z.data(), (int)z.size(), 48000.0, model, prog, nullptr, 1,
                                                    &err, [&](int a, int b) { silentCalls += (a == 0 && b == 48000); });
        check(okSilent && silentCalls == 1, "progressive: silent input reported as one final range");
    }
}

} // namespace

int main()
{
    const auto tmp = std::filesystem::temp_directory_path() / "flayr_chop_stems_test";
    std::filesystem::create_directories(tmp);

    testResampler();
    testHelpers(tmp);
    testModelIntegrity(tmp);
    testEngineApi();

    std::string model = chop::stems::defaultModelPath();
    if (const char* env = std::getenv("FLAYR_CHOP_STEMS_MODEL"); env && *env)
        model = env;
    if (chop::stems::modelAvailable(model)) {
        std::printf("model: %s\n", model.c_str());
        testRealModelIntegrity(model, tmp);
        testWithModel(model);
    } else {
        std::printf("SKIP  model-dependent checks (no model at %s; run scripts/fetch-model.sh)\n", model.c_str());
    }
    chop::stems::releaseCachedModels();
    std::filesystem::remove_all(tmp);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
