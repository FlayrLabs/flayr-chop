// stems_cli: separate a WAV file into stems with the Flayr Chop engine.
//
//   stems_cli in.wav outdir [modelPath] [threads]
//
// Reads 16/24/32-bit PCM or 32/64-bit float WAV (mono or stereo, any rate),
// writes <outdir>/<Stem>.wav as 32-bit float at the input rate and length.
// Without modelPath it uses chop::stems::defaultModelPath().

#include "stems/Stems.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Wav {
    int channels = 0;
    double rate = 0;
    std::vector<float> left, right;
};

uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool readWav(const std::string& path, Wav& w, std::string& err)
{
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        err = "not a RIFF/WAVE file"; return false;
    }
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* data = nullptr;
    size_t dataLen = 0;
    for (size_t pos = 12; pos + 8 <= d.size();) {
        const uint32_t len = rd32(&d[pos + 4]);
        const unsigned char* body = &d[pos + 8];
        const size_t avail = d.size() - (pos + 8);
        if (!std::memcmp(&d[pos], "fmt ", 4) && len >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 40) fmt = rd16(body + 24); // extensible: subformat GUID
        } else if (!std::memcmp(&d[pos], "data", 4)) {
            data = body; dataLen = std::min<size_t>(len, avail);
        }
        pos += 8 + (size_t)len + (len & 1);
    }
    if (!data || ch < 1 || rate == 0) { err = "missing fmt/data chunk"; return false; }
    const int bps = bits / 8;
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64)))) {
        err = "unsupported WAV encoding (fmt " + std::to_string(fmt) + ", " + std::to_string(bits) + " bit)"; return false;
    }
    const size_t frames = dataLen / ((size_t)bps * ch);
    w.channels = ch; w.rate = rate;
    w.left.resize(frames); w.right.resize(frames);
    auto sample = [&](size_t i, int c) -> float {
        const unsigned char* p = data + (i * ch + c) * bps;
        if (fmt == 3) {
            if (bits == 32) { float v; std::memcpy(&v, p, 4); return v; }
            double v; std::memcpy(&v, p, 8); return (float)v;
        }
        if (bits == 16) return (int16_t)rd16(p) / 32768.0f;
        if (bits == 24) { int32_t v = (p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24); return (v >> 8) / 8388608.0f; }
        return (int32_t)rd32(p) / 2147483648.0f;
    };
    for (size_t i = 0; i < frames; ++i) {
        w.left[i] = sample(i, 0);
        w.right[i] = ch > 1 ? sample(i, 1) : w.left[i];
    }
    return true;
}

bool writeWav(const std::filesystem::path& path, const std::vector<float>& l, const std::vector<float>& r, double rate)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t frames = (uint32_t)l.size(), dataLen = frames * 8, sr = (uint32_t)(rate + 0.5);
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16(2); w32(sr); w32(sr * 8); w16(8); w16(32);
    f.write("data", 4); w32(dataLen);
    for (uint32_t i = 0; i < frames; ++i) { f.write((const char*)&l[i], 4); f.write((const char*)&r[i], 4); }
    return (bool)f;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.wav outdir [modelPath] [threads]\n", argv[0]);
        return 2;
    }
    const std::string model = argc > 3 ? argv[3] : chop::stems::defaultModelPath();
    const int threads = argc > 4 ? std::atoi(argv[4]) : 0;
    if (!chop::stems::modelAvailable(model)) {
        std::fprintf(stderr, "model not found or incomplete: %s\nrun scripts/fetch-model.sh first\n", model.c_str());
        return 3;
    }

    Wav w;
    std::string err;
    if (!readWav(argv[1], w, err)) {
        std::fprintf(stderr, "read failed: %s\n", err.c_str());
        return 1;
    }
    const int n = (int)w.left.size();
    std::printf("input: %d frames, %d ch, %.0f Hz (%.2f s)\n", n, w.channels, w.rate, n / w.rate);
    std::printf("model: %s\nthreads: %d\n", model.c_str(), threads > 0 ? threads : chop::stems::defaultThreadCount());

    const bool mono = w.channels == 1;
    chop::stems::Separated sep;
    int lastPct = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = chop::stems::separate(
        w.left.data(), mono ? w.left.data() : w.right.data(), n, w.rate, model, sep,
        [&](float p) {
            const int pct = (int)(p * 100.0f);
            if (pct / 10 != lastPct / 10) { std::printf("  %3d%%\n", pct); std::fflush(stdout); }
            lastPct = pct;
            return true;
        },
        threads, &err);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
        std::fprintf(stderr, "separation failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("separated in %.2f s (%.1fx realtime)\n", secs, (n / w.rate) / secs);

    std::filesystem::path outDir = std::filesystem::u8path(argv[2]);
    std::filesystem::create_directories(outDir);
    for (size_t s = 0; s < sep.names.size(); ++s) {
        const auto& a = sep.audio[s];
        if ((int)a[0].size() != n || (int)a[1].size() != n) {
            std::fprintf(stderr, "length mismatch on %s\n", sep.names[s].c_str());
            return 1;
        }
        double e = 0;
        for (int i = 0; i < n; ++i) e += 0.5 * ((double)a[0][i] * a[0][i] + (double)a[1][i] * a[1][i]);
        const double rms = std::sqrt(e / std::max(1, n));
        const auto p = outDir / (sep.names[s] + ".wav");
        if (!writeWav(p, a[0], a[1], w.rate)) {
            std::fprintf(stderr, "write failed: %s\n", p.string().c_str());
            return 1;
        }
        std::printf("  %-7s rms %6.1f dBFS -> %s\n", sep.names[s].c_str(), 20 * std::log10(rms + 1e-12), p.string().c_str());
    }
    return 0;
}
