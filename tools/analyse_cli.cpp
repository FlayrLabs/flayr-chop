// analyse_cli: run the Flayr Chop analysis engine on a WAV file and print the results.
//
// Build: clang++ -O2 -std=c++17 tools/analyse_cli.cpp -o analyse_cli
// Usage: analyse_cli file.wav [cues=16] [--onsets] [--beats N]
//
// The WAV reader handles RIFF/WAVE PCM 8/16/24/32-bit, IEEE float 32/64, and WAVE_FORMAT_EXTENSIBLE,
// any channel count (downmixed to mono).

#include "../Source/Analysis.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool readWav(const char* path, std::vector<float>& mono, double& sr, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open"; return false; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    int fmt = 0, ch = 0, bits = 0;
    size_t dataOff = 0, dataLen = 0;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        const uint32_t len = rd32(&d[p + 4]);
        const unsigned char* c = &d[p + 8];
        if (std::memcmp(&d[p], "fmt ", 4) == 0 && p + 8 + 16 <= d.size()) {
            fmt = rd16(c);
            ch = rd16(c + 2);
            sr = rd32(c + 4);
            bits = rd16(c + 14);
            if (fmt == 0xFFFE && len >= 40 && p + 8 + 26 <= d.size()) fmt = rd16(c + 24);  // sub-format GUID head
        } else if (std::memcmp(&d[p], "data", 4) == 0) {
            dataOff = p + 8;
            dataLen = std::min<size_t>(len, d.size() - dataOff);
            if (len == 0xFFFFFFFFu || len == 0) dataLen = d.size() - dataOff;  // streamed writers
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!dataOff || ch <= 0 || sr <= 0) { err = "missing fmt/data chunk"; return false; }
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64)))) {
        err = "unsupported format " + std::to_string(fmt) + "/" + std::to_string(bits) + " bit";
        return false;
    }
    const int bps = bits / 8;
    const size_t frames = dataLen / (size_t)(bps * ch);
    mono.assign(frames, 0.f);
    const unsigned char* s = d.data() + dataOff;
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0;
        for (int c = 0; c < ch; ++c) {
            const unsigned char* q = s + (i * ch + c) * bps;
            double v = 0;
            if (fmt == 3 && bits == 32) { float x; std::memcpy(&x, q, 4); v = x; }
            else if (fmt == 3) { double x; std::memcpy(&x, q, 8); v = x; }
            else if (bits == 8) v = (q[0] - 128) / 128.0;
            else if (bits == 16) v = (int16_t)rd16(q) / 32768.0;
            else if (bits == 24) { int32_t x = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8; v = x / 8388608.0; }
            else v = (int32_t)rd32(q) / 2147483648.0;
            acc += v;
        }
        mono[i] = (float)(acc / ch);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file.wav [cues=16] [--onsets] [--beats N]\n", argv[0]);
        return 2;
    }
    int cues = 16;
    bool printOnsets = false;
    double beatsPerCue = 1.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--onsets")) printOnsets = true;
        else if (!std::strcmp(argv[i], "--beats") && i + 1 < argc) beatsPerCue = std::atof(argv[++i]);
        else cues = std::atoi(argv[i]);
    }
    std::vector<float> x;
    double sr = 0;
    std::string err;
    if (!readWav(argv[1], x, sr, err)) { std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str()); return 1; }

    using clk = std::chrono::steady_clock;
    namespace A = chop::analysis;
    const auto t0 = clk::now();
    const A::Result r = A::analyse(x.data(), (int)x.size(), sr);
    const auto t1 = clk::now();
    const auto autoCues = A::chop(r, x.data(), (int)x.size(), sr, cues, A::ChopMode::Auto);
    const auto beatCues = A::chop(r, x.data(), (int)x.size(), sr, cues, A::ChopMode::Beats, beatsPerCue);
    const auto t2 = clk::now();
    const A::Overview ov = A::overview(x.data(), (int)x.size(), sr, 2000);
    const auto t3 = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    std::printf("file      %s\n", argv[1]);
    std::printf("length    %.2f s @ %.0f Hz\n", x.size() / sr, sr);
    std::printf("bpm       %.2f  (conf %.2f)  first beat %.3f s\n", r.bpm, r.bpmConfidence, r.firstBeatSec);
    std::printf("key       %s  (%s, conf %.2f)\n", A::keyName(r.key, r.minor), A::wheelCode(r.key, r.minor), r.keyConfidence);
    std::printf("onsets    %zu\n", r.onsetsSec.size());
    if (printOnsets)
        for (size_t i = 0; i < r.onsetsSec.size(); ++i) std::printf("  %9.4f  %.2f\n", r.onsetsSec[i], r.onsetStrength[i]);
    std::printf("auto      ");
    for (double c : autoCues) std::printf("%.3f ", c);
    std::printf("\nbeats     ");
    for (double c : beatCues) std::printf("%.3f ", c);
    std::printf("\ntiming    analyse %.1f ms, chop x2 %.1f ms, overview(2000) %.1f ms  (%.0fx realtime)\n",
                ms(t0, t1), ms(t1, t2), ms(t2, t3), (x.size() / sr) * 1000.0 / std::max(1e-3, ms(t0, t1)));
    (void)ov;
    return 0;
}
