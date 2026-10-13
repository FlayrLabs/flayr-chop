// eval_runner: score the analysis engine against tests/eval/labels.tsv (via a manifest from run_eval.sh).
//
// Manifest lines (tab separated): split root path bpm key kind source wav [stemsDir]
//   wav is one WAV path, or several joined by " + " for a mix (summed, each looped to the longest).
//   stemsDir (optional) holds Drums.wav, Bass.wav, Other.wav, Vocals.wav from tools/stems_cli.
//
// Modes:
//   eval_runner manifest                 analyse() on every row
//   eval_runner manifest --stems         analyseWithStems() on rows that have a stems dir (others skipped)
//   eval_runner manifest --write-mix DIR write each mix row to DIR/<index>.wav (stereo float) and exit
//   --rows FILE                          also write one line per file with truth, estimate and confidences
//   --excerpts                           tempo on an untrimmed excerpt of every tempo row instead: the audio is
//                                        tiled to >= 40 s and a 5..20 s window is cut at a pseudo-random offset
//                                        (hash of the path), so the file length carries no tempo information
//
// Build with -DEVAL_BASELINE to score an engine without the candidate / stem API (e.g. v1.0.0).

#include "Analysis.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace A = chop::analysis;

uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

struct Wav { double sr = 0; std::vector<float> l, r; };

bool readWav(const std::string& path, Wav& w) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    int fmt = 0, ch = 0, bits = 0;
    size_t dataOff = 0, dataLen = 0, p = 12;
    while (p + 8 <= d.size()) {
        const uint32_t len = rd32(&d[p + 4]);
        const unsigned char* c = &d[p + 8];
        if (std::memcmp(&d[p], "fmt ", 4) == 0 && p + 8 + 16 <= d.size()) {
            fmt = rd16(c); ch = rd16(c + 2); w.sr = rd32(c + 4); bits = rd16(c + 14);
            if (fmt == 0xFFFE && len >= 40 && p + 8 + 26 <= d.size()) fmt = rd16(c + 24);
        } else if (std::memcmp(&d[p], "data", 4) == 0) {
            dataOff = p + 8;
            dataLen = std::min<size_t>(len, d.size() - dataOff);
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!dataOff || ch <= 0 || w.sr <= 0) return false;
    if (!(fmt == 3 && bits == 32) && !(fmt == 1 && bits == 16) && !(fmt == 1 && bits == 24)) return false;
    const int bps = bits / 8;
    const size_t frames = dataLen / (size_t)(bps * ch);
    w.l.assign(frames, 0.f);
    w.r.assign(frames, 0.f);
    const unsigned char* s = d.data() + dataOff;
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < std::min(ch, 2); ++c) {
            const unsigned char* q = s + (i * ch + c) * bps;
            float v = 0;
            if (fmt == 3) std::memcpy(&v, q, 4);
            else if (bits == 16) v = (int16_t)rd16(q) / 32768.0f;
            else v = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.0f;
            (c == 0 ? w.l : w.r)[i] = v;
        }
        if (ch == 1) w.r[i] = w.l[i];
    }
    return true;
}

bool writeWav(const std::string& path, const Wav& w) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t frames = (uint32_t)w.l.size(), sr = (uint32_t)w.sr, dataLen = frames * 8;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16(2); w32(sr); w32(sr * 8); w16(8); w16(32);
    f.write("data", 4); w32(dataLen);
    for (uint32_t i = 0; i < frames; ++i) { f.write((const char*)&w.l[i], 4); f.write((const char*)&w.r[i], 4); }
    return (bool)f;
}

std::vector<std::string> splitStr(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + sep.size();
    }
    return out;
}

// Loads one file, or sums a " + " list (each part looped to the longest part, peak-normalised to 0.9).
bool loadAudio(const std::string& spec, Wav& out) {
    const auto parts = splitStr(spec, " + ");
    if (parts.size() == 1) return readWav(parts[0], out);
    std::vector<Wav> ws(parts.size());
    size_t n = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!readWav(parts[i], ws[i]) || ws[i].l.empty()) return false;
        n = std::max(n, ws[i].l.size());
        out.sr = ws[i].sr;
    }
    out.l.assign(n, 0.f);
    out.r.assign(n, 0.f);
    for (const Wav& w : ws) {
        double e = 0;
        for (size_t i = 0; i < w.l.size(); ++i) e += 0.5 * ((double)w.l[i] * w.l[i] + (double)w.r[i] * w.r[i]);
        const float g = (float)(0.1 / std::sqrt(e / w.l.size() + 1e-12));  // equal loudness per part
        for (size_t i = 0; i < n; ++i) { out.l[i] += g * w.l[i % w.l.size()]; out.r[i] += g * w.r[i % w.l.size()]; }
    }
    float pk = 0;
    for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(out.l[i]), std::fabs(out.r[i])));
    if (pk > 0) for (size_t i = 0; i < n; ++i) { out.l[i] *= 0.9f / pk; out.r[i] *= 0.9f / pk; }
    return true;
}

std::vector<float> mono(const Wav& w) {
    std::vector<float> m(w.l.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (w.l[i] + w.r[i]);
    return m;
}

bool parseKey(const std::string& s, int& pc, bool& minor) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (s.empty() || s == "-") return false;
    minor = s.back() == 'm';
    const std::string root = minor ? s.substr(0, s.size() - 1) : s;
    for (int i = 0; i < 12; ++i) if (root == names[i]) { pc = i; return true; }
    return false;
}

enum TempoClass { kTExact, kTOctave, kTTriple, kTOther, kTNone };
enum KeyClass { kKExact, kKRelative, kKFifth, kKParallel, kKOther };

TempoClass tempoClass(double est, double truth) {
    if (!(est > 0)) return kTNone;
    const double r = est / truth;
    auto near = [&](double x) { return std::fabs(r / x - 1.0) <= 0.02; };
    if (near(1.0)) return kTExact;
    for (double x : {2.0, 0.5, 4.0, 0.25}) if (near(x)) return kTOctave;
    for (double x : {1.5, 2.0 / 3.0, 0.75, 4.0 / 3.0, 3.0, 1.0 / 3.0, 3.0 / 8.0, 8.0 / 3.0}) if (near(x)) return kTTriple;
    return kTOther;
}

KeyClass keyClass(int k, bool m, int tk, bool tm) {
    if (k == tk && m == tm) return kKExact;
    if (m != tm && ((tm && k == (tk + 3) % 12) || (!tm && k == (tk + 9) % 12))) return kKRelative;
    if (m == tm && (k == (tk + 7) % 12 || k == (tk + 5) % 12)) return kKFifth;
    if (k == tk) return kKParallel;
    return kKOther;
}

struct Tally {
    int tN = 0, tc[5] = {0, 0, 0, 0, 0};
    int kN = 0, kc[5] = {0, 0, 0, 0, 0};
    int kMajN = 0, kMajOk = 0, kMinN = 0, kMinOk = 0;
    int candT = 0, candK = 0;  // truth (octave-equivalent / exact key) somewhere in the candidate list
    double confRight = 0, confWrong = 0; int nRight = 0, nWrong = 0;
    double kconfRight = 0, kconfWrong = 0; int nkRight = 0, nkWrong = 0;
    int acaN = 0; double acaConf = 0; int acaHigh = 0;  // rhythmless material: confidence must stay low
};

void printTally(const char* name, const Tally& t, bool cands) {
    auto pct = [](int a, int b) { return b > 0 ? 100.0 * a / b : 0.0; };
    std::printf("  %-14s tempo n=%-4d exact %5.1f%%  oct-eq %5.1f%%  3:2/4:3 %5.1f%%  other %5.1f%%  none %4.1f%%",
                name, t.tN, pct(t.tc[kTExact], t.tN), pct(t.tc[kTExact] + t.tc[kTOctave], t.tN), pct(t.tc[kTTriple], t.tN),
                pct(t.tc[kTOther], t.tN), pct(t.tc[kTNone], t.tN));
    std::printf("  conf right %.2f wrong %.2f", t.nRight ? t.confRight / t.nRight : 0.0, t.nWrong ? t.confWrong / t.nWrong : 0.0);
    if (cands) std::printf("  in-cands %5.1f%%", pct(t.candT, t.tN));
    std::printf("\n");
    if (t.kN > 0) {
        std::printf("  %-14s key   n=%-4d exact %5.1f%%  relative %5.1f%%  fifth %5.1f%%  parallel %5.1f%%  other %5.1f%%  (maj %d/%d, min %d/%d)",
                    "", t.kN, pct(t.kc[kKExact], t.kN), pct(t.kc[kKRelative], t.kN), pct(t.kc[kKFifth], t.kN), pct(t.kc[kKParallel], t.kN),
                    pct(t.kc[kKOther], t.kN), t.kMajOk, t.kMajN, t.kMinOk, t.kMinN);
        std::printf("  conf right %.2f wrong %.2f", t.nkRight ? t.kconfRight / t.nkRight : 0.0, t.nkWrong ? t.kconfWrong / t.nkWrong : 0.0);
        if (cands) std::printf("  top-3 %5.1f%%", pct(t.candK, t.kN));
        std::printf("\n");
    }
    if (t.acaN > 0)
        std::printf("  %-14s rhythmless n=%d  mean tempo conf %.2f  conf>0.5: %d\n", "", t.acaN, t.acaConf / t.acaN, t.acaHigh);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s manifest [--stems] [--write-mix DIR] [--rows FILE]\n", argv[0]);
        return 2;
    }
    bool stems = false, excerpts = false;
    std::string mixDir, rowsPath;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stems")) stems = true;
        else if (!std::strcmp(argv[i], "--excerpts")) excerpts = true;
        else if (!std::strcmp(argv[i], "--write-mix") && i + 1 < argc) mixDir = argv[++i];
        else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) rowsPath = argv[++i];
    }
#ifdef EVAL_BASELINE
    if (stems) { std::fprintf(stderr, "baseline build has no analyseWithStems\n"); return 2; }
    const bool cands = false;
#else
    const bool cands = true;
#endif
    std::ifstream mf(argv[1]);
    if (!mf) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    FILE* rows = rowsPath.empty() ? nullptr : std::fopen(rowsPath.c_str(), "w");
    std::map<std::string, Tally> tallies;
    std::string line;
    int idx = -1;
    double totalMs = 0, totalSec = 0;
    while (std::getline(mf, line)) {
        if (line.empty() || line[0] == '#') continue;
        ++idx;
        const auto f = splitStr(line, "\t");
        if (f.size() < 8) continue;
        const std::string& split = f[0];
        const std::string& root = f[1];
        const std::string& kind = f[5];
        const std::string stemDir = f.size() > 8 ? f[8] : "";
        if (!mixDir.empty()) {
            if (root != "mix") continue;
            Wav w;
            if (!loadAudio(f[7], w)) { std::fprintf(stderr, "load failed: %s\n", f[7].c_str()); continue; }
            writeWav(mixDir + "/" + std::to_string(idx) + ".wav", w);
            continue;
        }
        if (stems && stemDir.empty()) continue;
        Wav w;
        if (!loadAudio(f[7], w)) { std::fprintf(stderr, "load failed: %s\n", f[7].c_str()); continue; }
        std::vector<float> x = mono(w);
        if (excerpts) {
            if (f[3] == "-" || kind == "acapella" || stems) continue;
            const size_t n0 = x.size();
            if (n0 == 0) continue;
            while (x.size() < (size_t)(40 * w.sr)) x.insert(x.end(), x.begin(), x.begin() + (long)n0);
            uint32_t h = 2166136261u;
            for (char ch : f[2]) h = (h ^ (uint8_t)ch) * 16777619u;
            const size_t len = (size_t)((5.0 + 15.0 * ((h & 0xFFFF) / 65535.0)) * w.sr);
            const size_t off = (size_t)(((h >> 16) / 65535.0) * (double)(x.size() - len));
            x = std::vector<float>(x.begin() + (long)off, x.begin() + (long)(off + len));
        }
        const auto t0 = std::chrono::steady_clock::now();
        A::Result r;
#ifndef EVAL_BASELINE
        if (stems) {
            Wav dr, ba, ot, vo;
            const bool okD = readWav(stemDir + "/Drums.wav", dr), okB = readWav(stemDir + "/Bass.wav", ba);
            const bool okO = readWav(stemDir + "/Other.wav", ot), okV = readWav(stemDir + "/Vocals.wav", vo);
            std::vector<float> md = okD ? mono(dr) : std::vector<float>(), mb = okB ? mono(ba) : std::vector<float>();
            std::vector<float> mh;
            if (okO) {
                mh = mono(ot);
                if (okV) { const auto mv = mono(vo); for (size_t i = 0; i < mh.size() && i < mv.size(); ++i) mh[i] += mv[i]; }
            }
            const int n = (int)x.size();
            auto fit = [&](std::vector<float>& v) { if (!v.empty()) v.resize((size_t)n, 0.f); };
            fit(md); fit(mb); fit(mh);
            r = A::analyseWithStems(x.data(), md.empty() ? nullptr : md.data(), mb.empty() ? nullptr : mb.data(),
                                    mh.empty() ? nullptr : mh.data(), n, w.sr);
        } else
#endif
        {
            r = A::analyse(x.data(), (int)x.size(), w.sr);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        totalMs += ms;
        totalSec += x.size() / w.sr;

        std::string group = root;
        if (kind == "acapella") group = "acapella";
        else if (kind == "perc") group = "music-perc";
        else if (root == "music") group = "music";
        std::vector<std::string> keys = {split + "/all", split + "/" + group};
        const double tb = f[3] == "-" ? 0.0 : std::atof(f[3].c_str());
        int tk = 0;
        bool tm = false;
        const bool hasKey = parseKey(f[4], tk, tm);
        TempoClass tcl = kTNone;
        KeyClass kcl = kKOther;
        bool inCandT = false, inCandK = false;
#ifndef EVAL_BASELINE
        for (const auto& c : r.tempoCandidates) {
            const TempoClass q = tempoClass(c.bpm, tb);
            if (q == kTExact || q == kTOctave) inCandT = true;
        }
        for (const auto& c : r.keyCandidates) if (c.key == tk && c.minor == tm) inCandK = true;
#endif
        for (const auto& k : keys) {
            Tally& t = tallies[k];
            if (kind == "acapella") {
                ++t.acaN;
                t.acaConf += r.bpmConfidence;
                if (r.bpmConfidence > 0.5f) ++t.acaHigh;
            } else if (tb > 0) {
                ++t.tN;
                tcl = tempoClass(r.bpm, tb);
                ++t.tc[tcl];
                if (inCandT) ++t.candT;
                if (tcl == kTExact || tcl == kTOctave) { t.confRight += r.bpmConfidence; ++t.nRight; }
                else { t.confWrong += r.bpmConfidence; ++t.nWrong; }
            }
            if (hasKey && kind != "acapella") {
                ++t.kN;
                kcl = keyClass(r.key, r.minor, tk, tm);
                ++t.kc[kcl];
                if (tm) { ++t.kMinN; if (kcl == kKExact) ++t.kMinOk; }
                else { ++t.kMajN; if (kcl == kKExact) ++t.kMajOk; }
                if (inCandK) ++t.candK;
                if (kcl == kKExact) { t.kconfRight += r.keyConfidence; ++t.nkRight; }
                else { t.kconfWrong += r.keyConfidence; ++t.nkWrong; }
            }
        }
        if (rows) {
            std::fprintf(rows, "%s\t%s\t%s\t%s\t%.2f\t%.2f\t%s\t%s\t%.2f", split.c_str(), group.c_str(), f[3].c_str(), f[4].c_str(), r.bpm,
                         r.bpmConfidence, A::keyName(r.key, r.minor), hasKey ? (kcl == kKExact ? "ok" : "X") : "-", r.keyConfidence);
#ifndef EVAL_BASELINE
            std::fprintf(rows, "\t");
            for (const auto& c : r.tempoCandidates) std::fprintf(rows, "%.1f:%.2f ", c.bpm, c.score);
            std::fprintf(rows, "\t");
            for (const auto& c : r.keyCandidates) std::fprintf(rows, "%s:%.2f ", A::keyName(c.key, c.minor), c.score);
#endif
            std::fprintf(rows, "\t%s\n", f[2].c_str());
        }
    }
    if (rows) std::fclose(rows);
    if (!mixDir.empty()) return 0;
    for (const char* sp : {"dev", "held"}) {
        std::printf("%s\n", sp);
        for (const auto& kv : tallies)
            if (kv.first.compare(0, std::strlen(sp) + 1, std::string(sp) + "/") == 0)
                printTally(kv.first.c_str() + std::strlen(sp) + 1, kv.second, cands);
    }
    std::printf("analysis time %.1f s for %.1f min of audio (%.0fx realtime)\n", totalMs / 1000.0, totalSec / 60.0,
                totalSec * 1000.0 / std::max(1.0, totalMs));
    return 0;
}
