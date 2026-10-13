// Tests for Source/Analysis.h using synthesized material with exactly known ground truth.
//
// Build and run:
//   clang++ -O2 -std=c++17 tests/analysis_test.cpp -o analysis_test && ./analysis_test
// Exit code is the number of failed checks (0 = all passed).

#include "../Source/Analysis.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace A = chop::analysis;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            std::printf("  FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

// ---------------------------------------------------------------------------------------------------------
// Synthesis helpers.

struct Rng {
    uint32_t s = 0x12345678u;
    float next() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f; }
};

constexpr double kPi = 3.14159265358979323846;

struct Buf {
    double sr;
    std::vector<float> x;
    Buf(double seconds, double sampleRate) : sr(sampleRate), x((size_t)(seconds * sampleRate), 0.f) {}
    double len() const { return x.size() / sr; }
    void add(long i, float v) { if (i >= 0 && i < (long)x.size()) x[(size_t)i] += v; }
};

void click(Buf& b, double t, double freq, float amp) {
    const long s0 = std::lround(t * b.sr);
    const int n = (int)(0.03 * b.sr);
    for (int i = 0; i < n; ++i) {
        const double tt = i / b.sr;
        b.add(s0 + i, (float)(amp * std::exp(-tt / 0.006) * std::sin(2 * kPi * freq * tt)));
    }
}

void kick(Buf& b, double t, float amp = 1.0f) {
    const long s0 = std::lround(t * b.sr);
    const int n = (int)(0.35 * b.sr);
    double ph = 0;
    for (int i = 0; i < n; ++i) {
        const double tt = i / b.sr;
        const double f = 50.0 + 110.0 * std::exp(-tt / 0.03);
        ph += 2 * kPi * f / b.sr;
        const double fade = std::min(1.0, (n - i) / (0.03 * b.sr));  // no click at the tail
        b.add(s0 + i, (float)(amp * fade * std::exp(-tt / 0.12) * std::sin(ph)));
    }
}

void snare(Buf& b, double t, Rng& r, float amp = 0.8f) {
    const long s0 = std::lround(t * b.sr);
    const int n = (int)(0.25 * b.sr);
    for (int i = 0; i < n; ++i) {
        const double tt = i / b.sr;
        const double v = 0.7 * r.next() * std::exp(-tt / 0.07) + 0.5 * std::sin(2 * kPi * 190 * tt) * std::exp(-tt / 0.05);
        const double fade = std::min(1.0, (n - i) / (0.03 * b.sr));
        b.add(s0 + i, (float)(amp * fade * v));
    }
}

void hat(Buf& b, double t, Rng& r, float amp = 0.25f, double decay = 0.025) {
    const long s0 = std::lround(t * b.sr);
    const int n = (int)(decay * 6 * b.sr);
    float prev = 0;
    for (int i = 0; i < n; ++i) {
        const double tt = i / b.sr;
        const float w = r.next();
        b.add(s0 + i, (float)(amp * (w - prev) * 0.5 * std::exp(-tt / decay)));  // first difference = bright noise
        prev = w;
    }
}

// Harmonic tone (harmonics 1..6, 1/h amplitudes) with a 5 ms attack, gentle decay and 30 ms release.
void tone(Buf& b, double t, double dur, double midi, float amp, double detuneCents = 0) {
    const double f0 = 440.0 * std::pow(2.0, (midi - 69.0 + detuneCents / 100.0) / 12.0);
    const long s0 = std::lround(t * b.sr);
    const int n = (int)((dur + 0.03) * b.sr);
    for (int i = 0; i < n; ++i) {
        const double tt = i / b.sr;
        double env = std::min(1.0, tt / 0.005) * (0.6 + 0.4 * std::exp(-tt / 0.3));
        if (tt > dur) env *= std::max(0.0, 1.0 - (tt - dur) / 0.03);
        double v = 0;
        for (int h = 1; h <= 6; ++h) {
            if (f0 * h > 0.45 * b.sr) break;
            v += std::sin(2 * kPi * f0 * h * tt) / h;
        }
        b.add(s0 + i, (float)(amp * env * v));
    }
}

// Chord: root-position triad around MIDI 60..71 plus the root as a bass note two octaves lower.
void chord(Buf& b, double t, double dur, int rootPc, bool minorChord, float amp, double detune = 0, bool seventh = false) {
    const int root = 60 + rootPc;
    const int third = root + (minorChord ? 3 : 4);
    tone(b, t, dur, root, amp, detune);
    tone(b, t, dur, third, amp, detune);
    tone(b, t, dur, root + 7, amp, detune);
    if (seventh) tone(b, t, dur, root + 10, amp * 0.7f, detune);
    tone(b, t, dur, root - 24, amp * 1.2f, detune);
}

void normalise(Buf& b, float peak = 0.9f) {
    float m = 0;
    for (float v : b.x) m = std::max(m, std::fabs(v));
    if (m > 0) for (float& v : b.x) v *= peak / m;
}

// ---------------------------------------------------------------------------------------------------------
// Checks shared by several tests.

// Returns how many truth events have a detected onset within tol, and reports the worst error.
int matchOnsets(const A::Result& r, const std::vector<double>& truth, double tol, double& worst) {
    int hit = 0;
    worst = 0;
    for (double t : truth) {
        double best = 1e9;
        for (double o : r.onsetsSec) best = std::min(best, std::fabs(o - t));
        if (best <= tol) { ++hit; worst = std::max(worst, best); }
    }
    return hit;
}

bool octaveCorrectWithin(double got, double want, double frac) { return std::fabs(got - want) <= want * frac; }

void checkChopInvariants(const A::Result& r, const Buf& b, const char* name) {
    const double len = b.len();
    for (A::ChopMode mode : {A::ChopMode::Auto, A::ChopMode::Beats, A::ChopMode::Even}) {
        for (int count : {1, 2, 4, 8, 16, 32, 64, 200}) {
            for (double bpc : {1.0, 0.5, 2.0}) {
                if (mode != A::ChopMode::Beats && bpc != 1.0) continue;
                const auto c = A::chop(r, b.x.data(), (int)b.x.size(), b.sr, count, mode, bpc);
                bool sorted = true, inRange = true;
                for (size_t i = 0; i < c.size(); ++i) {
                    if (c[i] < 0 || c[i] >= len) inRange = false;
                    if (i && !(c[i] > c[i - 1])) sorted = false;
                }
                CHECK((int)c.size() == count, "%s mode %d count %d bpc %.1f got %zu", name, (int)mode, count, bpc, c.size());
                CHECK(sorted, "%s mode %d count %d not strictly sorted", name, (int)mode, count);
                CHECK(inRange, "%s mode %d count %d out of range", name, (int)mode, count);
            }
        }
    }
    CHECK(A::chop(r, b.x.data(), (int)b.x.size(), b.sr, 0, A::ChopMode::Auto).empty(), "%s count 0", name);
}

// ---------------------------------------------------------------------------------------------------------
// Tests.

void testFFT() {
    std::printf("[fft] real FFT vs naive DFT\n");
    for (int n : {8, 64, 1024}) {
        std::vector<float> x((size_t)n), mag((size_t)n / 2 + 1);
        Rng r;
        for (auto& v : x) v = r.next();
        A::detail::RealFFT f(n);
        f.magnitudes(x.data(), mag.data());
        double worst = 0;
        for (int k = 0; k <= n / 2; ++k) {
            double re = 0, im = 0;
            for (int i = 0; i < n; ++i) { re += x[(size_t)i] * std::cos(2 * kPi * k * i / n); im -= x[(size_t)i] * std::sin(2 * kPi * k * i / n); }
            worst = std::max(worst, std::fabs(std::sqrt(re * re + im * im) - mag[(size_t)k]));
        }
        CHECK(worst < 1e-3 * std::sqrt((double)n), "n=%d worst err %g", n, worst);
    }
}

struct TempoCase { const char* name; double bpm; };

void testClickTracks() {
    std::printf("[clicks] metronome click tracks, accented downbeat, 0.5 s lead-in\n");
    for (double bpm : {87.0, 120.0, 140.0, 174.0}) {
        for (double sr : {44100.0, 48000.0}) {
            Buf b(30.0, sr);
            const double P = 60.0 / bpm;
            std::vector<double> truth;
            for (int i = 0;; ++i) {
                const double t = 0.5 + i * P;
                if (t > b.len() - 0.1) break;
                click(b, t, i % 4 == 0 ? 2000 : 1500, i % 4 == 0 ? 0.9f : 0.5f);
                truth.push_back(t);
            }
            const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), sr);
            double worst;
            const int hit = matchOnsets(r, truth, 0.010, worst);
            std::printf("  %5.1f BPM @%5.0f: got %.3f (conf %.2f) first beat %.4f, onsets %zu/%zu hit, worst %.1f ms\n",
                        bpm, sr, r.bpm, r.bpmConfidence, r.firstBeatSec, (size_t)hit, truth.size(), worst * 1000);
            CHECK(octaveCorrectWithin(r.bpm, bpm, 0.01), "click %.0f got %.3f", bpm, r.bpm);
            CHECK(hit == (int)truth.size(), "click %.0f recall %d/%zu", bpm, hit, truth.size());
            CHECK(r.onsetsSec.size() <= truth.size() + 2, "click %.0f false positives: %zu detected", bpm, r.onsetsSec.size());
            CHECK(std::fabs(r.firstBeatSec - 0.5) < 0.010, "click %.0f first downbeat %.4f want 0.5", bpm, r.firstBeatSec);
            CHECK(r.bpmConfidence > 0.7f, "click %.0f confidence %.2f", bpm, r.bpmConfidence);
            if (bpm == 120.0 && sr == 44100.0) checkChopInvariants(r, b, "click120");
        }
    }
    std::printf("[clicks] unaccented (every click identical)\n");
    for (double bpm : {87.0, 174.0, 100.0, 65.0}) {
        Buf b(20.0, 44100);
        for (int i = 0;; ++i) {
            const double t = 0.25 + i * 60.0 / bpm;
            if (t > b.len() - 0.1) break;
            click(b, t, 1800, 0.7f);
        }
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        // 65 BPM is below the preferred range, so the double (130) is the expected answer.
        const double want = bpm < 80 ? bpm * 2 : bpm;
        std::printf("  %5.1f BPM: got %.3f (conf %.2f)\n", bpm, r.bpm, r.bpmConfidence);
        CHECK(octaveCorrectWithin(r.bpm, want, 0.01), "flat click %.0f got %.3f want %.0f", bpm, r.bpm, want);
    }
}

void testSwing() {
    std::printf("[swing] beat clicks plus quieter swung 8ths (2/3 of the beat)\n");
    for (double bpm : {92.0, 120.0, 140.0}) {
        Buf b(30.0, 44100);
        Rng rng;
        const double P = 60.0 / bpm;
        std::vector<double> truth;
        for (int i = 0;; ++i) {
            const double t = 0.3 + i * P;
            if (t + P > b.len() - 0.1) break;
            if (i % 2 == 0) kick(b, t, 0.9f); else snare(b, t, rng, 0.7f);
            hat(b, t, rng, 0.25f);
            hat(b, t + P * 2.0 / 3.0, rng, 0.18f);
            truth.push_back(t);
            truth.push_back(t + P * 2.0 / 3.0);
        }
        normalise(b);
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        double worst;
        const int hit = matchOnsets(r, truth, 0.010, worst);
        std::printf("  swing %5.1f BPM: got %.3f (conf %.2f), onsets %d/%zu, worst %.1f ms\n", bpm, r.bpm, r.bpmConfidence, hit,
                    truth.size(), worst * 1000);
        CHECK(octaveCorrectWithin(r.bpm, bpm, 0.01), "swing %.0f got %.3f", bpm, r.bpm);
        CHECK(hit >= (int)(truth.size() * 0.97), "swing %.0f recall %d/%zu", bpm, hit, truth.size());
    }
}

// Drum pattern in 16th-note steps per bar: 'k' kick, 's' snare, 'h' hat, combos allowed per step string.
struct Pattern { const char* name; double bpm; const char* kickSteps; const char* snareSteps; const char* hatSteps; };

// `masked` (optional) receives the hat-only events that fall inside the decay of a snare hit less than
// 200 ms earlier: ghost notes about 20 dB under the snare tail, reported separately from the main recall.
Buf makeDrums(const Pattern& p, double seconds, double sr, std::vector<double>* truth, double lead = 0.0,
              std::vector<double>* masked = nullptr) {
    Buf b(seconds, sr);
    Rng rng;
    const double step = 60.0 / p.bpm / 4.0;
    double lastSnare = -1e9;
    for (int i = 0;; ++i) {
        const double t = lead + i * step;
        if (t > b.len() - 0.3) break;
        const int s = i % 16;
        bool any = false, hatOnly = true;
        if (p.kickSteps[s] == 'x') { kick(b, t); any = true; hatOnly = false; }
        if (p.snareSteps[s] == 'x') { snare(b, t, rng); any = true; hatOnly = false; }
        if (p.hatSteps[s] == 'x') { hat(b, t, rng, 0.22f); any = true; }
        if (p.hatSteps[s] == 'o') { hat(b, t, rng, 0.3f, 0.09); any = true; }
        if (any) {
            if (hatOnly && t - lastSnare < 0.2) { if (masked) masked->push_back(t); }
            else if (truth) truth->push_back(t);
        }
        if (p.snareSteps[s] == 'x') lastSnare = t;
    }
    normalise(b);
    return b;
}

const Pattern kPatterns[] = {
    //                         1...2...3...4...    1...2...3...4...    1...2...3...4...
    {"boom-bap 90", 90.0,    "x.....x...x.....", "....x.......x...", "x.x.x.x.x.x.x.x."},
    {"house 124", 124.0,     "x...x...x...x...", "....x.......x...", "..o...o...o...o."},
    {"trap 140", 140.0,      "x......x..x.....", "........x.......", "xxxxxxxxxxxxxxxx"},
    {"dnb 174", 174.0,       "x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x."},
    {"lofi 75", 75.0,        "x......x..x.....", "....x.......x...", "x.x.x.x.x.x.x.x."},
};

void testDrumLoops() {
    std::printf("[drums] synthesized kits\n");
    for (const Pattern& p : kPatterns) {
        std::vector<double> truth, masked;
        const Buf b = makeDrums(p, 24.0, 44100, &truth, 0.2, &masked);
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        double worst, worstM;
        const int hit = matchOnsets(r, truth, 0.010, worst);
        const int hitM = matchOnsets(r, masked, 0.010, worstM);
        // 75 BPM sits below the preferred 80..160 window: 150 is the musical answer.
        const double want = p.bpm < 80 ? p.bpm * 2 : p.bpm;
        std::printf("  %-12s want %.0f: got %.3f (conf %.2f) first beat %.3f, onsets %d/%zu, masked ghost hats %d/%zu, %zu detected, worst %.1f ms\n",
                    p.name, want, r.bpm, r.bpmConfidence, r.firstBeatSec, hit, truth.size(), hitM, masked.size(), r.onsetsSec.size(), worst * 1000);
        CHECK(octaveCorrectWithin(r.bpm, want, 0.01), "%s got %.3f", p.name, r.bpm);
        CHECK(hit >= (int)(truth.size() * 0.97), "%s recall %d/%zu", p.name, hit, truth.size());
        CHECK(r.onsetsSec.size() <= (truth.size() + masked.size()) * 1.05 + 2, "%s false positives %zu vs %zu", p.name, r.onsetsSec.size(), truth.size() + masked.size());
        // Downbeat: every pattern has its kick on step 0 of the bar, the bar starts at 0.2 s.
        const double barLen = 4 * 60.0 / p.bpm;
        const double ph = std::fmod(r.firstBeatSec - 0.2 + 100 * barLen, barLen);
        CHECK(std::min(ph, barLen - ph) < 0.012, "%s downbeat phase off by %.3f s", p.name, std::min(ph, barLen - ph));
        checkChopInvariants(r, b, p.name);

        // Beats mode lands on the grid; Auto mode starts at the first sound and keeps a 16th-note spacing.
        const double P = 60.0 / r.bpm;
        const auto beats = A::chop(r, b.x.data(), (int)b.x.size(), b.sr, 16, A::ChopMode::Beats, 1.0);
        double worstBeat = 0;
        for (size_t i = 0; i < beats.size(); ++i) worstBeat = std::max(worstBeat, std::fabs(beats[i] - (r.firstBeatSec + i * P)));
        CHECK(worstBeat < 0.031, "%s beats-mode cue off grid by %.3f", p.name, worstBeat);
        const auto au = A::chop(r, b.x.data(), (int)b.x.size(), b.sr, 16, A::ChopMode::Auto);
        CHECK(std::fabs(au[0] - 0.2) < 0.010, "%s auto first cue %.4f want 0.2", p.name, au[0]);
        double minGap = 1e9;
        int onGrid = 0;
        for (size_t i = 0; i < au.size(); ++i) {
            if (i) minGap = std::min(minGap, au[i] - au[i - 1]);
            const double x = (au[i] - r.firstBeatSec) / P;
            if (std::fabs(x - std::round(x)) * P < 0.02) ++onGrid;
        }
        CHECK(minGap >= P / 4 - 1e-6, "%s auto min gap %.4f < 16th %.4f", p.name, minGap, P / 4);
        CHECK(onGrid >= 12, "%s auto cues on beats %d/16", p.name, onGrid);
        std::printf("    auto 16: on-beat %d/16, min gap %.3f s (16th = %.3f s)\n", onGrid, minGap, P / 4);
    }
}

struct KeyCase { const char* name; int key; bool minor; std::vector<std::pair<int, bool>> chords; bool sevenths; double detune; double sr; };

void testKeys() {
    std::printf("[key] chord progressions with harmonic tones and bass\n");
    const std::vector<KeyCase> cases = {
        {"C  I-IV-V-I", 0, false, {{0, false}, {5, false}, {7, false}, {0, false}}, false, 0, 44100},
        {"G  I-vi-IV-V", 7, false, {{7, false}, {4, true}, {0, false}, {2, false}}, false, 0, 44100},
        {"Eb I-V-vi-IV", 3, false, {{3, false}, {10, false}, {0, true}, {8, false}}, false, 0, 48000},
        {"A  I-IV-I-V7", 9, false, {{9, false}, {2, false}, {9, false}, {4, false}}, true, 0, 44100},
        {"F  I-ii-V-I +35c", 5, false, {{5, false}, {7, true}, {0, false}, {5, false}}, false, 35, 44100},
        {"Am i-iv-V-i", 9, true, {{9, true}, {2, true}, {4, false}, {9, true}}, false, 0, 44100},
        {"F#m i-iv-v-i", 6, true, {{6, true}, {11, true}, {1, true}, {6, true}}, false, 0, 44100},
        {"Cm i-VI-iv-V", 0, true, {{0, true}, {8, false}, {5, true}, {7, false}}, false, 0, 48000},
        {"D#m i-VII-VI-V", 3, true, {{3, true}, {1, false}, {11, false}, {10, false}}, false, 0, 44100},
        {"Em i-iv-i-V -40c", 4, true, {{4, true}, {9, true}, {4, true}, {11, false}}, false, -40, 44100},
        {"Gm i-iv-VII-i", 7, true, {{7, true}, {0, true}, {5, false}, {7, true}}, false, 0, 44100},
    };
    for (const auto& kc : cases) {
        const double bpm = 100.0, bar = 4 * 60.0 / bpm;
        Buf b(bar * 4 * 3 + 0.5, kc.sr);
        std::vector<double> truth;
        for (int rep = 0; rep < 3; ++rep)
            for (size_t c = 0; c < kc.chords.size(); ++c) {
                const double t = 0.1 + (rep * 4 + c) * bar;
                chord(b, t, bar - 0.05, kc.chords[c].first, kc.chords[c].second, 0.15f, kc.detune, kc.sevenths && c == 3);
                truth.push_back(t);
            }
        normalise(b);
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        double worst;
        const int hit = matchOnsets(r, truth, 0.010, worst);
        std::printf("  %-18s want %-4s got %-4s (%s, conf %.2f), chord onsets %d/%zu worst %.1f ms\n", kc.name, A::keyName(kc.key, kc.minor),
                    A::keyName(r.key, r.minor), A::wheelCode(r.key, r.minor), r.keyConfidence, hit, truth.size(), worst * 1000);
        CHECK(r.key == kc.key && r.minor == kc.minor, "%s got %s", kc.name, A::keyName(r.key, r.minor));
        CHECK(hit == (int)truth.size(), "%s chord onset recall %d/%zu", kc.name, hit, truth.size());
    }

    std::printf("[key] chords over a drum loop (Am i-iv-V-i at 90 BPM, boom-bap)\n");
    {
        std::vector<double> truth;
        Buf b = makeDrums(kPatterns[0], 32.0, 44100, &truth, 0.0);
        const double bar = 4 * 60.0 / 90.0;
        Buf c(32.0, 44100);
        const std::pair<int, bool> prog[4] = {{9, true}, {2, true}, {4, false}, {9, true}};
        for (int i = 0; i * bar < 31.0; ++i) chord(c, i * bar, bar - 0.05, prog[i % 4].first, prog[i % 4].second, 0.15f);
        normalise(c, 0.6f);
        for (size_t i = 0; i < b.x.size(); ++i) b.x[i] = 0.6f * b.x[i] + c.x[i];
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        std::printf("  got %s (conf %.2f), %.3f BPM\n", A::keyName(r.key, r.minor), r.keyConfidence, r.bpm);
        CHECK(r.key == 9 && r.minor, "drums+Am got %s", A::keyName(r.key, r.minor));
        CHECK(octaveCorrectWithin(r.bpm, 90, 0.01), "drums+Am bpm %.3f", r.bpm);
    }
}

void testNames() {
    std::printf("[names] key names and key-wheel codes\n");
    CHECK(std::string(A::keyName(0, false)) == "C", "C");
    CHECK(std::string(A::keyName(6, true)) == "F#m", "F#m");
    CHECK(std::string(A::keyName(10, false)) == "A#", "A#");
    const char* majC[12] = {"8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B"};
    const char* minC[12] = {"5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"};
    for (int k = 0; k < 12; ++k) {
        CHECK(std::string(A::wheelCode(k, false)) == majC[k], "wheel code major %d got %s want %s", k, A::wheelCode(k, false), majC[k]);
        CHECK(std::string(A::wheelCode(k, true)) == minC[k], "wheel code minor %d got %s want %s", k, A::wheelCode(k, true), minC[k]);
    }
}

void testEdgeCases() {
    std::printf("[edge] silence, tiny buffers, no rhythm, snapping\n");
    {
        Buf b(5.0, 44100);
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        CHECK(r.bpm == 0 && r.onsetsSec.empty(), "silence bpm %.2f onsets %zu", r.bpm, r.onsetsSec.size());
        checkChopInvariants(r, b, "silence");
        const A::Overview ov = A::overview(b.x.data(), (int)b.x.size(), b.sr, 100);
        CHECK(ov.mn.size() == 100 && ov.high.size() == 100, "overview size on silence");
    }
    {
        Buf b(100.0 / 44100.0, 44100);
        for (auto& v : b.x) v = 0.5f;
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        checkChopInvariants(r, b, "100 samples");
        const A::Overview ov = A::overview(b.x.data(), (int)b.x.size(), b.sr, 1000);
        CHECK(ov.mx.size() == 1000, "overview more buckets than samples");
        bool ok = true;
        for (size_t i = 0; i < ov.mx.size(); ++i) ok = ok && ov.mn[i] <= ov.mx[i];
        CHECK(ok, "overview min<=max with more buckets than samples");
    }
    {
        A::Result none;
        Buf b(3.0, 44100);
        checkChopInvariants(none, b, "empty result");
        CHECK(A::analyse(nullptr, 0, 44100).bpm == 0, "null input");
    }
    {
        // A single sustained chord: one onset, no meaningful tempo, chop must still return `count` cues.
        Buf b(6.0, 44100);
        chord(b, 0.5, 5.0, 0, false, 0.2f);
        normalise(b);
        const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
        std::printf("  single chord: %zu onsets, bpm %.2f conf %.2f, key %s\n", r.onsetsSec.size(), r.bpm, r.bpmConfidence, A::keyName(r.key, r.minor));
        CHECK(!r.onsetsSec.empty() && std::fabs(r.onsetsSec[0] - 0.5) < 0.01, "single chord onset");
        CHECK(r.bpmConfidence < 0.3f, "single chord tempo confidence %.2f", r.bpmConfidence);
        checkChopInvariants(r, b, "single chord");
        const auto au = A::chop(r, b.x.data(), (int)b.x.size(), b.sr, 8, A::ChopMode::Auto);
        CHECK(std::fabs(au[0] - 0.5) < 0.01, "single chord first auto cue %.4f", au[0]);
    }
    {
        A::Result r;
        r.onsetsSec = {1.0, 2.0, 3.0};
        r.onsetStrength = {1, 1, 1};
        CHECK(A::snapToTransient(r, 1.02) == 1.0, "snap inside window");
        CHECK(A::snapToTransient(r, 1.05) == 1.05, "no snap outside window");
        CHECK(A::snapToTransient(r, 2.51, 0.6) == 3.0, "snap picks nearest");
        CHECK(A::snapToTransient(r, 0.98, 0.03) == 1.0, "snap from before");
    }
}

void testOverview() {
    std::printf("[overview] band colouring\n");
    Buf b(4.0, 44100);
    Rng rng;
    for (size_t i = 0; i < b.x.size() / 2; ++i) b.x[i] = 0.8f * (float)std::sin(2 * kPi * 60 * i / b.sr);
    float prev = 0;
    for (size_t i = b.x.size() / 2; i < b.x.size(); ++i) { const float w = rng.next(); b.x[i] = 0.4f * (w - prev); prev = w; }
    const A::Overview ov = A::overview(b.x.data(), (int)b.x.size(), b.sr, 200);
    CHECK(ov.mn.size() == 200 && ov.mx.size() == 200 && ov.low.size() == 200 && ov.mid.size() == 200 && ov.high.size() == 200, "sizes");
    bool range = true;
    for (size_t i = 0; i < 200; ++i)
        range = range && ov.mn[i] <= ov.mx[i] && ov.low[i] >= 0 && ov.low[i] <= 1 && ov.mid[i] >= 0 && ov.mid[i] <= 1 && ov.high[i] >= 0 && ov.high[i] <= 1;
    CHECK(range, "value ranges");
    CHECK(ov.low[50] > 0.5f && ov.high[50] < 0.1f && ov.low[50] > ov.mid[50], "bass bucket low %.2f mid %.2f high %.2f", ov.low[50], ov.mid[50], ov.high[50]);
    CHECK(ov.high[150] > ov.low[150] * 4 && ov.high[150] > 0.3f, "hiss bucket low %.2f mid %.2f high %.2f", ov.low[150], ov.mid[150], ov.high[150]);
    CHECK(std::fabs(ov.mx[50] - 0.8f) < 0.01f && std::fabs(ov.mn[50] + 0.8f) < 0.01f, "min/max %.3f %.3f", ov.mn[50], ov.mx[50]);
}

void testLongFileSpeed() {
    std::printf("[speed] 10 minute 44.1 kHz drums + chords (boom-bap 93 BPM, Cm i-VI-iv-V)\n");
    const Pattern p{"long", 93.0, "x.....x...x.....", "....x.......x...", "x.x.x.x.x.x.x.x."};
    Buf b = makeDrums(p, 600.0, 44100, nullptr, 1.0);
    Buf c(600.0, 44100);
    const double bar = 4 * 60.0 / 93.0;
    const std::pair<int, bool> prog[4] = {{0, true}, {8, false}, {5, true}, {7, false}};
    for (int i = 0; 1.0 + i * bar < 598.0; ++i) chord(c, 1.0 + i * bar, bar - 0.05, prog[i % 4].first, prog[i % 4].second, 0.15f);
    normalise(c, 0.6f);
    for (size_t i = 0; i < b.x.size(); ++i) b.x[i] = 0.6f * b.x[i] + c.x[i];
    const auto t0 = std::chrono::steady_clock::now();
    const A::Result r = A::analyse(b.x.data(), (int)b.x.size(), b.sr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto cues = A::chop(r, b.x.data(), (int)b.x.size(), b.sr, 64, A::ChopMode::Auto);
    const auto t2 = std::chrono::steady_clock::now();
    const A::Overview ov = A::overview(b.x.data(), (int)b.x.size(), b.sr, 4000);
    const auto t3 = std::chrono::steady_clock::now();
    auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point bb) {
        return std::chrono::duration<double, std::milli>(bb - a).count();
    };
    std::printf("  analyse %.0f ms, chop(64, Auto) %.0f ms, overview(4000) %.0f ms; bpm %.4f key %s onsets %zu\n", ms(t0, t1), ms(t1, t2),
                ms(t2, t3), r.bpm, A::keyName(r.key, r.minor), r.onsetsSec.size());
    CHECK(ms(t0, t1) < 3000, "analyse too slow: %.0f ms", ms(t0, t1));
    CHECK(std::fabs(r.bpm - 93.0) < 0.02, "long file bpm %.4f", r.bpm);
    CHECK(r.key == 0 && r.minor, "long file key %s", A::keyName(r.key, r.minor));
    // Phase lock at the end of a 10 minute file: the last grid beat must still sit on a kick or snare.
    const double P = 60.0 / r.bpm;
    const double lastBeat = r.firstBeatSec + std::floor((590.0 - r.firstBeatSec) / P) * P;
    const double truthNearest = 1.0 + std::round((lastBeat - 1.0) / P) * P;
    CHECK(std::fabs(lastBeat - truthNearest) < 0.010, "grid drift at 590 s: %.4f s", lastBeat - truthNearest);
    CHECK((int)cues.size() == 64 && ov.mn.size() == 4000, "long-file chop/overview sizes");
}

int main() {
    testFFT();
    testClickTracks();
    testSwing();
    testDrumLoops();
    testKeys();
    testNames();
    testEdgeCases();
    testOverview();
    testLongFileSpeed();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
