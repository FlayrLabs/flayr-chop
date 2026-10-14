#pragma once

// Playback engine: pads, voices, stems mixing and the DJ filter. Framework-free so it can be
// tested without a host. Audio data is prepared off the audio thread (see Renderer.h) and handed
// over through Published<T> snapshots, so nothing here allocates or locks while playing.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chop
{
constexpr int kNumPads = 64;
constexpr int kPadsPerBank = 16;
constexpr int kNumBanks = kNumPads / kPadsPerBank;
constexpr int kMaxVoices = 16;
constexpr int kMaxStems = 6;
constexpr int kKeysRange = 12;     // Keys mode renders -12..+12 semitones with time kept
constexpr int kFirstPadNote = 36;  // C1 plays pad 1, the MPC / drum-rack convention
constexpr int kKeysRootNote = 60;  // C3 plays the selected pad at its own pitch
constexpr double kMaxKeysSliceSec = 12.0;

struct Stereo
{
    std::vector<float> l, r;
    int size() const { return (int) l.size(); }
    void resize (int n) { l.assign ((size_t) n, 0.0f); r.assign ((size_t) n, 0.0f); }
};
using StereoPtr = std::shared_ptr<const Stereo>;

enum class PadMode : int { Trigger = 0, Gate, Loop };

struct Pad
{
    double cue = -1.0;      // seconds into the source; negative = empty pad
    double end = -1.0;      // seconds; negative = runs to the next cue
    float gainDb = 0.0f;
    float pan = 0.0f;       // -1..1
    float keyShift = 0.0f;  // per-pad key in semitones, time kept (rendered with HQ Stretch)
    float stretch = 0.0f;   // per-pad length change in percent (-75 .. +300), pitch kept
    float filter = 0.0f;    // per-pad filter: -1 low-pass .. 0 off .. +1 high-pass
    float attackMs = 1.0f;
    float releaseMs = 40.0f;
    PadMode mode = PadMode::Trigger;
    bool reverse = false;
    int choke = 0;          // 0 = none, otherwise pads in the same group cut each other
    uint32_t stemMask = 0xffffffffu;
    bool favorite = false;  // locked: auto-chop leaves this pad alone

    bool hasCue() const { return cue >= 0.0; }
    bool needsRender() const { return std::abs (keyShift) > 1e-3f || std::abs (stretch) > 1e-3f; }
};

// A snapshot handed from worker / message threads to the audio thread. Every snapshot gets a
// generation number; the audio thread reports the oldest one it still reads, and older ones are
// freed by collect() on a non-audio thread.
template <typename T>
class Published
{
public:
    void publish (std::unique_ptr<T> item)
    {
        std::lock_guard<std::mutex> lock (mutex);
        item->gen = ++lastGen;
        current.store (item.get(), std::memory_order_release);
        items.push_back (std::move (item));
    }

    T* get() const { return current.load (std::memory_order_acquire); }

    // Audio thread, once per block: nothing older than this is read any more.
    void setOldestInUse (uint64_t gen) { oldestInUse.store (gen, std::memory_order_release); }

    void collect()
    {
        std::lock_guard<std::mutex> lock (mutex);
        const auto oldest = oldestInUse.load (std::memory_order_acquire);
        const auto* cur = current.load (std::memory_order_acquire);
        items.erase (std::remove_if (items.begin(), items.end(),
                                     [&] (const std::unique_ptr<T>& p) { return p.get() != cur && p->gen < oldest; }),
                     items.end());
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock (mutex);
        current.store (nullptr, std::memory_order_release);
        items.clear();
        oldestInUse.store (0);
    }

    size_t count() const { std::lock_guard<std::mutex> lock (mutex); return items.size(); }

private:
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<T>> items;
    std::atomic<T*> current { nullptr };
    std::atomic<uint64_t> oldestInUse { 0 };
    uint64_t lastGen = 0;
};

// The source with HQ Stretch applied, at the host sample rate.
struct Rendered
{
    uint64_t gen = 0;
    uint64_t sourceId = 0;
    double sampleRate = 48000.0;
    double positionsPerSecond = 48000.0; // buffer samples per second of the ORIGINAL source
    double rate = 1.0;                   // playback increment (1 when stretched, speed when tape)
    double speed = 1.0;
    float semis = 0.0f;
    bool stretched = false;
    bool draft = false;                  // fast chunked render; the full-quality one follows
    StereoPtr full;
    std::vector<StereoPtr> stems;

    int length() const { return full ? full->size() : 0; }
};

// Keys mode: one pad's slice rendered at every semitone with its length kept.
struct KeysRender
{
    uint64_t gen = 0;
    uint64_t renderedGen = 0;
    int pad = -1;
    uint32_t mask = 0;
    double startPos = 0.0, endPos = 0.0; // in Rendered positions, for the playhead
    std::array<StereoPtr, 2 * kKeysRange + 1> notes {};
};

// Pads with their own key shift or time stretch, rendered from the source with HQ Stretch.
struct PadRender
{
    StereoPtr buf;              // the slice, stems mixed by the pad's mask, start to end
    uint64_t renderedGen = 0;   // the main render it belongs to
    double cue = -1.0, end = -1.0;
    float keyShift = 0.0f, stretch = 0.0f;
    uint32_t mask = 0;
    double startPos = 0.0, endPos = 0.0; // in Rendered positions, for the playhead

    bool matches (const Pad& p, double endSec, uint64_t gen) const
    {
        return buf != nullptr && renderedGen == gen && std::abs (cue - p.cue) < 1e-9 && std::abs (end - endSec) < 1e-9
               && std::abs (keyShift - p.keyShift) < 1e-4f && std::abs (stretch - p.stretch) < 1e-4f && mask == p.stemMask;
    }
};

struct PadRenders
{
    uint64_t gen = 0;
    std::array<PadRender, kNumPads> pads {};
};

struct PadTable
{
    uint64_t gen = 0;
    std::array<Pad, kNumPads> pads {};
    std::array<double, kNumPads> endSec {}; // resolved end of each pad's slice
};

// Resolves each pad's end: its own end, else the next cue in time, else the end of the file.
inline void resolveEnds (PadTable& t, double lengthSec)
{
    std::vector<double> cues;
    for (const auto& p : t.pads)
        if (p.hasCue())
            cues.push_back (p.cue);
    std::sort (cues.begin(), cues.end());
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = t.pads[(size_t) i];
        double e = lengthSec;
        if (p.end > p.cue && p.hasCue())
            e = std::min (p.end, lengthSec);
        else if (p.hasCue())
        {
            auto it = std::upper_bound (cues.begin(), cues.end(), p.cue + 1e-6);
            if (it != cues.end())
                e = *it;
        }
        t.endSec[(size_t) i] = e;
    }
}

struct Globals
{
    float gain = 1.0f;
    float filter = 0.0f;      // -1 low-pass .. 0 off .. +1 high-pass
    float velocity = 0.5f;    // velocity sensitivity 0..1
    bool mono = true;         // one pad at a time
    bool keysMode = false;
    int keysPad = 0;
    int numStems = 0;
    std::array<float, kMaxStems> stemGain { 1, 1, 1, 1, 1, 1 };
};

struct Context
{
    const Rendered* rendered = nullptr;
    const PadTable* pads = nullptr;
    const KeysRender* keys = nullptr;
    const PadRenders* padRenders = nullptr;
    Globals g;
};

namespace detail
{
inline float hermite (const float* d, int n, double pos)
{
    const int i = (int) std::floor (pos);
    const float f = (float) (pos - i);
    auto at = [&] (int k) { return (k >= 0 && k < n) ? d[k] : 0.0f; };
    const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

// Zero-delay-feedback state-variable filter, one per channel.
struct Svf
{
    float ic1 = 0, ic2 = 0;
    void reset() { ic1 = ic2 = 0; }
    // returns {low, high}
    inline void tick (float x, float g, float k, float& low, float& high)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1, a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        low = v2;
        high = x - k * v1 - v2;
    }
};
} // namespace detail

class Sampler
{
public:
    struct VoiceView
    {
        std::atomic<int> pad { -1 };
        std::atomic<float> sourceSec { 0.0f };
        std::atomic<float> level { 0.0f };
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& v : voices)
            v = Voice {};
        preview = Voice {};
        previewView.on.store (false);
        followVoice.store (-1);
        for (auto& f : filt)
            f.reset();
        filterSmoothed = 0.0f;
        clock = 0;
    }

    void noteOn (int note, float velocity, const Context& c)
    {
        if (c.rendered == nullptr || c.pads == nullptr || c.rendered->length() == 0)
            return;

        int pad = -1;
        float keysSemis = 0.0f;
        if (c.g.keysMode)
        {
            pad = c.g.keysPad;
            keysSemis = (float) (note - kKeysRootNote);
        }
        else
        {
            pad = note - kFirstPadNote;
        }
        if (pad < 0 || pad >= kNumPads)
            return;
        const auto& p = c.pads->pads[(size_t) pad];
        if (! p.hasCue())
            return;

        // Cut what this hit replaces: everything in mono, the same pad, or the same choke group.
        for (auto& v : voices)
        {
            if (! v.active)
                continue;
            const bool samePad = v.pad == pad && ! c.g.keysMode;
            const bool group = p.choke != 0 && v.choke == p.choke;
            const bool sameKey = c.g.keysMode && v.note == note;
            if (c.g.mono || samePad || group || sameKey)
                v.fastRelease (sr);
        }
        // In mono a pad also takes over from the transport, so the song never plays twice.
        if (c.g.mono)
            stopPreview();

        auto& v = freeVoice();
        // With the transport stopped, the playhead follows the latest pad hit (one playhead).
        if (! isPreviewing())
            followVoice.store ((int) (&v - voices.data()), std::memory_order_relaxed);
        const auto& r = *c.rendered;
        v = Voice {};
        v.active = true;
        v.note = note;
        v.pad = pad;
        v.choke = p.choke;
        v.mode = p.mode;
        v.reverse = p.reverse;
        v.age = ++clock;
        v.rendered = &r;
        v.renderedGen = r.gen;

        const double start = p.cue * r.positionsPerSecond;
        const double end = std::max (start + 1.0, c.pads->endSec[(size_t) pad] * r.positionsPerSecond);
        v.startPos = start;
        v.endPos = std::min (end, (double) r.length());
        v.inc = r.rate;

        // A pad with its own key shift / stretch plays its own render when it's ready, and
        // falls back to tape-style pitch until then.
        if (! c.g.keysMode && p.needsRender())
        {
            const auto* pr = c.padRenders != nullptr ? &c.padRenders->pads[(size_t) pad] : nullptr;
            if (pr != nullptr && pr->matches (p, c.pads->endSec[(size_t) pad], r.gen))
            {
                useBuffer (v, pr->buf.get(), pr->startPos, pr->endPos);
                v.padRenders = c.padRenders;
                v.padRendersGen = c.padRenders->gen;
                v.inc = r.stretched ? 1.0 : r.rate;
            }
            else
            {
                v.inc *= std::pow (2.0, p.keyShift / 12.0);
            }
        }

        // Keys mode: use the time-kept render of this semitone when it is ready.
        if (c.g.keysMode)
        {
            const int idx = (int) std::lround (keysSemis) + kKeysRange;
            const auto* k = c.keys;
            const bool ready = k != nullptr && k->pad == pad && k->renderedGen == r.gen && idx >= 0
                               && idx < (int) k->notes.size() && k->notes[(size_t) idx] != nullptr;
            if (ready)
            {
                v.keys = k;
                v.keysGen = k->gen;
                useBuffer (v, k->notes[(size_t) idx].get(), k->startPos, k->endPos);
                // Keys buffers are already stretched; in tape mode they carry no tempo change.
                v.inc = r.stretched ? 1.0 : r.rate;
            }
            else
            {
                v.inc *= std::pow (2.0, keysSemis / 12.0); // tape-style until the render lands
            }
        }

        v.pos = v.reverse ? v.endPos - 1.0 : v.startPos;
        if (v.reverse)
            v.inc = -v.inc;

        const float velGain = 1.0f - c.g.velocity * (1.0f - velocity);
        const float g = std::pow (10.0f, p.gainDb / 20.0f) * velGain;
        const float pan = std::clamp (p.pan, -1.0f, 1.0f);
        v.gainL = g * std::cos ((pan + 1.0f) * 0.25f * 3.14159265f) * 1.41421356f;
        v.gainR = g * std::sin ((pan + 1.0f) * 0.25f * 3.14159265f) * 1.41421356f;
        v.attackStep = 1.0f / std::max (1.0f, p.attackMs * 0.001f * (float) sr);
        v.releaseSamples = std::max (1.0f, p.releaseMs * 0.001f * (float) sr);
        v.stemMask = p.stemMask;
        v.filter = std::clamp (p.filter, -1.0f, 1.0f);
    }

    // PLAY: the whole track from a position. Pads still ringing are cut first, otherwise a
    // slice that runs to the end of the song plays on top of it.
    void startTransport (double sourceSec, const Context& c)
    {
        allNotesOff();
        startPreview (sourceSec, c);
    }

    // STOP: silence everything, pads included.
    void stopTransport()
    {
        allNotesOff();
        stopPreview();
    }

    // The whole track, played from a position (the transport above the waveform). In poly mode
    // pads can play over it; in mono a pad hit stops it.
    void startPreview (double sourceSec, const Context& c)
    {
        if (c.rendered == nullptr || c.rendered->length() == 0)
            return;
        const auto& r = *c.rendered;
        preview = Voice {};
        preview.active = true;
        preview.rendered = &r;
        preview.renderedGen = r.gen;
        preview.startPos = 0.0;
        preview.endPos = (double) r.length();
        preview.pos = std::clamp (sourceSec * r.positionsPerSecond, 0.0, preview.endPos - 1.0);
        preview.inc = r.rate;
        preview.attackStep = 1.0f / (float) (0.003 * sr);
        preview.releaseSamples = (float) (0.01 * sr);
        previewView.on.store (true);
    }

    void stopPreview()
    {
        if (preview.active)
            preview.fastRelease (sr);
    }

    bool isPreviewing() const { return preview.active && ! preview.released; }
    double previewSourceSec() const
    {
        return preview.rendered != nullptr ? preview.pos / preview.rendered->positionsPerSecond : 0.0;
    }

    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.active && v.note == note && ! v.released && v.mode != PadMode::Trigger)
                v.release();
    }

    void allNotesOff()
    {
        for (auto& v : voices)
            if (v.active)
                v.fastRelease (sr);
    }

    void render (float* outL, float* outR, int n, const Context& c)
    {
        // The preview follows a new render (key or tempo changed) from the same spot in the song.
        if (preview.active && ! preview.released && c.rendered != nullptr && c.rendered != preview.rendered)
        {
            const double at = previewSourceSec();
            startPreview (at, c);
        }

        // Stem weights per voice are fixed for the block.
        const int numStems = c.g.numStems;
        for (auto* vp : allVoices())
        {
            auto& v = *vp;
            if (! v.active)
                continue;
            v.useFull = true;
            if (v.buf == nullptr && numStems > 0 && v.rendered != nullptr && (int) v.rendered->stems.size() >= numStems)
            {
                for (int s = 0; s < numStems; ++s)
                {
                    v.w[(size_t) s] = ((v.stemMask >> s) & 1u) ? c.g.stemGain[(size_t) s] : 0.0f;
                    if (std::abs (v.w[(size_t) s] - 1.0f) > 1e-4f)
                        v.useFull = false;
                }
            }
        }

        for (auto* v : allVoices())
            if (v->active)
                renderVoice (*v, outL, outR, n, numStems);
        if (! preview.active)
            previewView.on.store (false, std::memory_order_relaxed);
        else
            previewView.sourceSec.store ((float) previewSourceSec(), std::memory_order_relaxed);

        applyFilter (outL, outR, n, c.g.filter);

        for (int i = 0; i < n; ++i)
        {
            outL[i] *= c.g.gain;
            outR[i] *= c.g.gain;
        }

        for (int i = 0; i < kMaxVoices; ++i)
        {
            const auto& v = voices[(size_t) i];
            auto& view = views[(size_t) i];
            view.pad.store (v.active ? v.pad : -1, std::memory_order_relaxed);
            if (! v.active && followVoice.load (std::memory_order_relaxed) == i)
                followVoice.store (-1, std::memory_order_relaxed);
            if (v.active && v.rendered != nullptr)
            {
                const double renderedPos = v.buf != nullptr ? v.bufOrigin + v.pos * v.bufScale : v.pos;
                view.sourceSec.store ((float) (renderedPos / v.rendered->positionsPerSecond), std::memory_order_relaxed);
                view.level.store (v.env, std::memory_order_relaxed);
            }
        }
    }

    // Oldest snapshot generations any voice still reads (UINT64_MAX when none).
    uint64_t oldestRenderedInUse() const
    {
        uint64_t g = UINT64_MAX;
        for (const auto& v : voices)
            if (v.active)
                g = std::min (g, v.renderedGen);
        if (preview.active)
            g = std::min (g, preview.renderedGen);
        return g;
    }
    uint64_t oldestPadRendersInUse() const
    {
        uint64_t g = UINT64_MAX;
        for (const auto& v : voices)
            if (v.active && v.padRenders != nullptr)
                g = std::min (g, v.padRendersGen);
        return g;
    }
    uint64_t oldestKeysInUse() const
    {
        uint64_t g = UINT64_MAX;
        for (const auto& v : voices)
            if (v.active && v.keys != nullptr)
                g = std::min (g, v.keysGen);
        return g;
    }

    int activeVoices() const
    {
        int n = 0;
        for (const auto& v : voices)
            n += v.active ? 1 : 0;
        return n;
    }

    std::array<VoiceView, kMaxVoices> views;
    std::atomic<int> followVoice { -1 }; // the pad voice the playhead follows, or -1
    struct PreviewView
    {
        std::atomic<bool> on { false };
        std::atomic<float> sourceSec { 0.0f };
    } previewView;

private:
    struct Voice
    {
        bool active = false, released = false, reverse = false, useFull = true;
        int note = -1, pad = -1, choke = 0;
        PadMode mode = PadMode::Trigger;
        uint64_t age = 0, renderedGen = 0, keysGen = 0, padRendersGen = 0;
        const Rendered* rendered = nullptr;
        const KeysRender* keys = nullptr;
        const PadRenders* padRenders = nullptr;
        const Stereo* buf = nullptr;            // a pre-rendered slice (Keys mode or a pad's own key/stretch)
        double bufOrigin = 0.0, bufScale = 1.0; // maps buffer positions back to Rendered positions
        float filter = 0.0f;
        std::array<detail::Svf, 2> svf {};
        double pos = 0, inc = 1, startPos = 0, endPos = 0;
        float gainL = 1, gainR = 1;
        float env = 0, attackStep = 1, releaseSamples = 1, releaseStep = 0;
        uint32_t stemMask = 0xffffffffu;
        std::array<float, kMaxStems> w {};

        void release()
        {
            released = true;
            releaseStep = env / releaseSamples;
        }
        void fastRelease (double rate)
        {
            const float fast = (float) (0.004 * rate);
            if (! released || releaseStep * fast < env)
            {
                released = true;
                releaseStep = env / fast + 1e-7f;
            }
        }
    };

    static void useBuffer (Voice& v, const Stereo* b, double originPos, double endPos)
    {
        v.buf = b;
        v.startPos = 0.0;
        v.endPos = (double) b->size();
        v.bufOrigin = originPos;
        v.bufScale = (endPos - originPos) / std::max (1.0, v.endPos);
    }

    std::array<Voice*, kMaxVoices + 1> allVoices()
    {
        std::array<Voice*, kMaxVoices + 1> a {};
        for (int i = 0; i < kMaxVoices; ++i)
            a[(size_t) i] = &voices[(size_t) i];
        a[kMaxVoices] = &preview;
        return a;
    }

    Voice& freeVoice()
    {
        for (auto& v : voices)
            if (! v.active)
                return v;
        // Steal: prefer released voices, then the oldest.
        Voice* best = &voices[0];
        for (auto& v : voices)
            if ((v.released && ! best->released) || (v.released == best->released && v.age < best->age))
                best = &v;
        return *best;
    }

    void renderVoice (Voice& v, float* outL, float* outR, int n, int numStems)
    {
        const Stereo* single = v.buf != nullptr ? v.buf : (v.useFull ? v.rendered->full.get() : nullptr);
        // Per-pad filter, same curve as the global one.
        const float fa = std::abs (v.filter);
        const bool fOn = fa > 0.02f, fHigh = v.filter > 0.0f;
        const float fHz = fHigh ? 20.0f * std::pow (400.0f, fa) : 20000.0f * std::pow (1.0f / 250.0f, fa);
        const float fG = std::tan (3.14159265f * std::min (fHz, (float) sr * 0.45f) / (float) sr);
        const double edgeFade = 0.003 * sr; // de-click the end of every slice
        const double len = (double) (single != nullptr ? single->size() : v.rendered->length());

        for (int i = 0; i < n; ++i)
        {
            // Loop wraps; other modes stop at the slice edge.
            if (v.inc > 0 ? v.pos >= v.endPos : v.pos < v.startPos)
            {
                if (v.mode == PadMode::Loop && ! v.released)
                    v.pos += (v.inc > 0 ? -1.0 : 1.0) * (v.endPos - v.startPos);
                else
                {
                    v.active = false;
                    return;
                }
            }
            if (v.pos < 0.0 || v.pos >= len)
            {
                v.active = false;
                return;
            }

            if (v.released)
            {
                v.env -= v.releaseStep;
                if (v.env <= 0.0f)
                {
                    v.active = false;
                    return;
                }
            }
            else if (v.env < 1.0f)
            {
                v.env = std::min (1.0f, v.env + v.attackStep);
            }

            float edge = 1.0f;
            if (v.mode != PadMode::Loop || v.released)
            {
                const double left = v.inc > 0 ? v.endPos - v.pos : v.pos - v.startPos;
                if (left < edgeFade)
                    edge = (float) std::max (0.0, left / edgeFade);
            }

            float l = 0.0f, r = 0.0f;
            if (single != nullptr)
            {
                l = detail::hermite (single->l.data(), single->size(), v.pos);
                r = detail::hermite (single->r.data(), single->size(), v.pos);
            }
            else
            {
                for (int s = 0; s < numStems; ++s)
                {
                    const float w = v.w[(size_t) s];
                    if (w <= 0.0f)
                        continue;
                    const auto& st = *v.rendered->stems[(size_t) s];
                    l += w * detail::hermite (st.l.data(), st.size(), v.pos);
                    r += w * detail::hermite (st.r.data(), st.size(), v.pos);
                }
            }

            if (fOn)
            {
                float lo, hi;
                v.svf[0].tick (l, fG, 1.2f, lo, hi);
                l = fHigh ? hi : lo;
                v.svf[1].tick (r, fG, 1.2f, lo, hi);
                r = fHigh ? hi : lo;
            }

            const float e = v.env * edge;
            outL[i] += l * e * v.gainL;
            outR[i] += r * e * v.gainR;
            v.pos += v.inc;
        }
    }

    void applyFilter (float* l, float* r, int n, float target)
    {
        if (std::abs (target) < 0.02f && std::abs (filterSmoothed) < 0.02f)
        {
            filterSmoothed = 0.0f;
            for (auto& f : filt)
                f.reset();
            return;
        }
        const float k = 1.2f; // gentle resonance
        for (int i = 0; i < n; ++i)
        {
            filterSmoothed += 0.002f * (target - filterSmoothed);
            const float a = std::abs (filterSmoothed);
            const bool high = filterSmoothed > 0.0f;
            const float hz = high ? 20.0f * std::pow (400.0f, a)          // 20 Hz .. 8 kHz
                                  : 20000.0f * std::pow (1.0f / 250.0f, a); // 20 kHz .. 80 Hz
            const float g = std::tan (3.14159265f * std::min (hz, (float) sr * 0.45f) / (float) sr);
            // Fade the filter in over the first few percent so the centre detent is seamless.
            const float mix = std::min (1.0f, a * 25.0f);
            float lo, hi;
            filt[0].tick (l[i], g, k, lo, hi);
            l[i] += mix * ((high ? hi : lo) - l[i]);
            filt[1].tick (r[i], g, k, lo, hi);
            r[i] += mix * ((high ? hi : lo) - r[i]);
        }
    }

    double sr = 48000.0;
    std::array<Voice, kMaxVoices> voices {};
    Voice preview {};
    std::array<detail::Svf, 2> filt {};
    float filterSmoothed = 0.0f;
    uint64_t clock = 0;
};
} // namespace chop
