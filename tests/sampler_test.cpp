// Engine tests: HQ Stretch accuracy, the draft/final renders, and pad playback.
#include "../Source/Renderer.cpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace chop;

static int failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

static constexpr double kSr = 48000.0;
static constexpr double kPi = 3.14159265358979323846;

static StereoPtr tone (double hz, double seconds, double amp = 0.3)
{
    auto s = std::make_shared<Stereo>();
    s->resize ((int) (seconds * kSr));
    for (int i = 0; i < s->size(); ++i)
        s->l[(size_t) i] = s->r[(size_t) i] = (float) (amp * std::sin (2 * kPi * hz * i / kSr));
    return s;
}

// Frequency by zero crossings over the middle of a buffer.
static double frequency (const std::vector<float>& x, int from, int to)
{
    int crossings = 0, first = -1, last = -1;
    for (int i = std::max (1, from); i < to; ++i)
        if (x[(size_t) i - 1] <= 0.0f && x[(size_t) i] > 0.0f)
        {
            ++crossings;
            if (first < 0) first = i;
            last = i;
        }
    return crossings > 1 ? (crossings - 1) * kSr / (last - first) : 0.0;
}

static double rms (const std::vector<float>& x, int from, int to)
{
    double e = 0;
    for (int i = from; i < to; ++i)
        e += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (e / std::max (1, to - from));
}

static void testStretchAccuracy()
{
    const auto in = tone (440.0, 6.0);
    for (float semis : { -7.0f, 3.0f, 12.0f })
        for (double speed : { 0.8, 1.0, 1.25 })
        {
            Stereo out;
            const int len = (int) std::llround (in->size() / speed);
            stretchBuffer (*in, out, len, kSr, semis, false);
            CHECK (out.size() == len, "length %d != %d", out.size(), len);
            const double f = frequency (out.l, len / 4, 3 * len / 4);
            const double want = 440.0 * std::pow (2.0, semis / 12.0);
            CHECK (std::abs (f - want) / want < 0.003, "semis %.0f speed %.2f: %.2f Hz, want %.2f", semis, speed, f, want);
            const double level = rms (out.l, len / 4, 3 * len / 4);
            CHECK (std::abs (level - 0.3 / std::sqrt (2.0)) < 0.04, "level %.3f", level);
        }
}

static void testChunkedMatchesFull()
{
    // 40 s of a chord: the parallel draft must sound like the full render (same pitch, level, length,
    // no holes at the seams).
    auto in = std::make_shared<Stereo>();
    in->resize ((int) (40 * kSr));
    for (int i = 0; i < in->size(); ++i)
    {
        const double t = i / kSr;
        const float v = (float) (0.15 * std::sin (2 * kPi * 220 * t) + 0.1 * std::sin (2 * kPi * 277.18 * t)
                                 + 0.1 * std::sin (2 * kPi * 329.63 * t));
        in->l[(size_t) i] = in->r[(size_t) i] = v;
    }
    const int len = (int) std::llround (in->size() / 1.1);
    Stereo full, draft;
    auto t0 = std::chrono::steady_clock::now();
    stretchBuffer (*in, full, len, kSr, 2.0f, false);
    auto t1 = std::chrono::steady_clock::now();
    stretchBufferChunked (*in, draft, len, kSr, 2.0f, false);
    auto t2 = std::chrono::steady_clock::now();
    std::printf ("40 s stereo: full %.2f s, draft %.2f s\n", std::chrono::duration<double> (t1 - t0).count(),
                 std::chrono::duration<double> (t2 - t1).count());
    CHECK (draft.size() == len, "draft length");
    // Level in 50 ms windows never collapses (a seam hole or doubled overlap would show up here).
    const double ref = rms (full.l, len / 4, 3 * len / 4);
    const int win = (int) (0.05 * kSr);
    double worst = 1.0;
    for (int a = win * 4; a + win < len - win * 4; a += win / 2)
        worst = std::min (worst, rms (draft.l, a, a + win) / ref);
    CHECK (worst > 0.6, "draft level dips to %.2f of the full render", worst);
}

static void testRendererPublishes()
{
    Published<Rendered> rendered;
    Published<KeysRender> keys;
    Renderer r (rendered, keys);

    auto src = std::make_shared<Source>();
    src->id = 1;
    src->sampleRate = kSr;
    src->full = tone (440.0, 30.0);
    RenderSettings s;
    s.source = src;
    s.speed = 1.2;
    s.semis = -2.0f;
    r.request (s);
    auto t0 = std::chrono::steady_clock::now();
    bool sawDraft = false;
    while (r.busy || rendered.get() == nullptr || rendered.get()->draft)
    {
        r.request (s); // the plug-in re-requests every UI tick; that must not restart the render
        if (auto* cur = rendered.get(); cur != nullptr && cur->draft && ! sawDraft)
        {
            sawDraft = true;
            std::printf ("30 s draft after %.2f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    std::printf ("30 s final after %.2f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    const auto* cur = rendered.get();
    CHECK (cur != nullptr && ! cur->draft && cur->stretched, "final render");
    CHECK (cur->length() == (int) std::llround (30 * kSr / 1.2), "rendered length %d", cur->length());
    CHECK (std::abs (cur->positionsPerSecond - kSr / 1.2) < 1e-6, "positions per second");

    // Keys mode slice.
    KeysSettings k;
    k.pad = 3;
    k.startSec = 2.0;
    k.endSec = 4.0;
    r.requestKeys (k);
    while (r.busy)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    const auto* kr = keys.get();
    CHECK (kr != nullptr && kr->pad == 3 && kr->renderedGen == cur->gen, "keys render");
    if (kr != nullptr)
    {
        const auto& up = *kr->notes[(size_t) (kKeysRange + 5)];
        const double f = frequency (up.l, up.size() / 4, 3 * up.size() / 4);
        const double want = 440.0 * std::pow (2.0, (5 - 2) / 12.0);
        CHECK (std::abs (f - want) / want < 0.004, "keys +5 on top of -2: %.2f Hz, want %.2f", f, want);
        CHECK (std::abs (up.size() - 2.0 * kSr / 1.2) < 2, "keys slice length %d", up.size());
    }

    // Tape mode: no processing, speed and pitch move together.
    s.pitchAndTime = false;
    r.request (s);
    while (r.busy)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    cur = rendered.get();
    CHECK (! cur->stretched && cur->full == src->full, "tape mode plays the source");
    CHECK (std::abs (cur->rate - 1.2 * std::pow (2.0, -2.0 / 12.0)) < 1e-9, "tape rate %.4f", cur->rate);

    rendered.setOldestInUse (cur->gen);
    rendered.collect();
    CHECK (rendered.count() == 1, "old renders freed (%zu left)", rendered.count());
}

static void testPads()
{
    Published<Rendered> rendered;
    auto r = std::make_unique<Rendered>();
    // Four 0.5 s tones at different pitches = four slices.
    auto buf = std::make_shared<Stereo>();
    buf->resize ((int) (2.0 * kSr));
    const double hz[] = { 200, 300, 400, 500 };
    for (int i = 0; i < buf->size(); ++i)
        buf->l[(size_t) i] = buf->r[(size_t) i] = (float) (0.3 * std::sin (2 * kPi * hz[i / (int) (0.5 * kSr)] * i / kSr));
    r->full = buf;
    r->positionsPerSecond = kSr;
    rendered.publish (std::move (r));

    PadTable t;
    for (int i = 0; i < 4; ++i)
        t.pads[(size_t) i].cue = 0.5 * i;
    resolveEnds (t, 2.0);
    CHECK (std::abs (t.endSec[0] - 0.5) < 1e-9 && std::abs (t.endSec[3] - 2.0) < 1e-9, "ends resolve to the next cue");

    Sampler smp;
    smp.prepare (kSr);
    Context c;
    c.rendered = rendered.get();
    c.pads = &t;
    c.g.velocity = 0.0f;

    // Pad 3 (note 38) plays the 400 Hz slice for 0.5 s then stops by itself.
    std::vector<float> l ((size_t) kSr), rr ((size_t) kSr);
    smp.noteOn (kFirstPadNote + 2, 1.0f, c);
    smp.noteOff (kFirstPadNote + 2); // Trigger mode ignores the release
    smp.render (l.data(), rr.data(), (int) kSr, c);
    const double f = frequency (l, 1000, (int) (0.45 * kSr));
    CHECK (std::abs (f - 400.0) < 2.0, "pad 3 plays 400 Hz, got %.1f", f);
    CHECK (rms (l, (int) (0.52 * kSr), (int) kSr) < 1e-6, "slice stops at its end");
    CHECK (smp.activeVoices() == 0, "voice freed");

    // Mono: a second pad cuts the first within a few ms.
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (rr.begin(), rr.end(), 0.0f);
    smp.noteOn (kFirstPadNote, 1.0f, c);
    smp.render (l.data(), rr.data(), 4800, c);
    smp.noteOn (kFirstPadNote + 3, 1.0f, c);
    smp.render (l.data() + 4800, rr.data() + 4800, 4800, c);
    const double f2 = frequency (l, 4800 + 600, 9600);
    CHECK (std::abs (f2 - 500.0) < 3.0, "mono choke: only pad 4 after the cut, got %.1f Hz", f2);

    // Gate + reverse: plays backwards while held, fades on release.
    t.pads[1].mode = PadMode::Gate;
    t.pads[1].reverse = true;
    smp.prepare (kSr);
    std::fill (l.begin(), l.end(), 0.0f);
    smp.noteOn (kFirstPadNote + 1, 1.0f, c);
    smp.render (l.data(), rr.data(), 4800, c);
    smp.noteOff (kFirstPadNote + 1);
    smp.render (l.data() + 4800, rr.data() + 4800, 9600, c);
    CHECK (std::abs (frequency (l, 100, 4700) - 300.0) < 3.0, "reverse slice pitch");
    CHECK (rms (l, 4800 + 4000, 4800 + 9600) < 1e-6, "gate released");

    // Empty pads do nothing.
    smp.noteOn (kFirstPadNote + 20, 1.0f, c);
    CHECK (smp.activeVoices() == 0, "empty pad stays silent");
}

static void testStemsAndKeys()
{
    // Two stems: 300 Hz "bass" and 1200 Hz "vocals". The full mix has both.
    auto bass = tone (300.0, 2.0, 0.2), vox = tone (1200.0, 2.0, 0.2);
    auto full = std::make_shared<Stereo>();
    full->resize (bass->size());
    for (int i = 0; i < full->size(); ++i)
        full->l[(size_t) i] = full->r[(size_t) i] = bass->l[(size_t) i] + vox->l[(size_t) i];

    Published<Rendered> rendered;
    auto r = std::make_unique<Rendered>();
    r->full = full;
    r->stems = { bass, vox };
    r->positionsPerSecond = kSr;
    rendered.publish (std::move (r));

    PadTable t;
    t.pads[0].cue = 0.0;
    t.pads[0].stemMask = 0b10; // vocals only
    t.pads[1].cue = 1.0;
    resolveEnds (t, 2.0);

    Sampler smp;
    smp.prepare (kSr);
    Context c;
    c.rendered = rendered.get();
    c.pads = &t;
    c.g.velocity = 0.0f;
    c.g.numStems = 2;

    std::vector<float> l (24000), rr (24000);
    smp.noteOn (kFirstPadNote, 1.0f, c);
    smp.render (l.data(), rr.data(), 24000, c);
    CHECK (std::abs (frequency (l, 1000, 20000) - 1200.0) < 5.0, "vocals-only pad plays only the vocal stem (%.1f Hz)",
           frequency (l, 1000, 20000));

    // Global stem level applies too: muting vocals silences that pad.
    smp.prepare (kSr);
    std::fill (l.begin(), l.end(), 0.0f);
    c.g.stemGain[1] = 0.0f;
    smp.noteOn (kFirstPadNote, 1.0f, c);
    smp.render (l.data(), rr.data(), 24000, c);
    CHECK (rms (l, 0, 24000) < 1e-6, "muted stem is silent");
    c.g.stemGain[1] = 1.0f;

    // Keys mode: note 67 (C3 + 7) plays the +7 render of the selected pad, same length.
    auto keys = std::make_unique<KeysRender>();
    keys->renderedGen = c.rendered->gen;
    keys->pad = 1;
    keys->startPos = kSr;
    keys->endPos = 2 * kSr;
    for (int k = 0; k < 2 * kKeysRange + 1; ++k)
        keys->notes[(size_t) k] = tone (100.0 + 10.0 * k, 1.0);
    Published<KeysRender> keysPub;
    keysPub.publish (std::move (keys));
    c.keys = keysPub.get();
    c.g.keysMode = true;
    c.g.keysPad = 1;
    smp.prepare (kSr);
    std::vector<float> kl ((size_t) kSr), kr ((size_t) kSr);
    smp.noteOn (kKeysRootNote + 7, 1.0f, c);
    smp.render (kl.data(), kr.data(), (int) kSr, c);
    const double want = 100.0 + 10.0 * (kKeysRange + 7);
    CHECK (std::abs (frequency (kl, 1000, 40000) - want) < 2.0, "keys note +7 plays its render (%.1f Hz, want %.1f)",
           frequency (kl, 1000, 40000), want);
    // The playhead maps back into the source.
    CHECK (smp.views[0].sourceSec.load() > 1.0f || smp.activeVoices() == 0, "keys playhead inside the pad's slice");
}

int main()
{
    testStemsAndKeys();
    testStretchAccuracy();
    testChunkedMatchesFull();
    testRendererPublishes();
    testPads();
    std::printf (failures == 0 ? "sampler_test: all passed\n" : "sampler_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
