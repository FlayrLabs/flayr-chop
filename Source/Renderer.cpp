#include "Renderer.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace chop
{
namespace
{
using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

void configure (Stretch& s, double sampleRate, float semis, bool formant)
{
    s.presetDefault (2, (float) sampleRate);
    // Above ~8 kHz content is treated as noise, which keeps hats and transients crisp when shifting up.
    s.setTransposeSemitones (semis, (float) (8000.0 / sampleRate));
    if (formant)
        s.setFormantFactor (1.0f, true);
}

// Linear resample for the tiny-buffer fallback (shorter than the stretcher's window).
void resampleLinear (const Stereo& in, Stereo& out, int outLength)
{
    out.resize (outLength);
    const int n = in.size();
    if (n == 0)
        return;
    const double step = outLength > 1 ? (double) (n - 1) / (outLength - 1) : 0.0;
    for (int i = 0; i < outLength; ++i)
    {
        const double p = i * step;
        const int a = std::min ((int) p, n - 1), b = std::min (a + 1, n - 1);
        const float f = (float) (p - a);
        out.l[(size_t) i] = in.l[(size_t) a] + f * (in.l[(size_t) b] - in.l[(size_t) a]);
        out.r[(size_t) i] = in.r[(size_t) a] + f * (in.r[(size_t) b] - in.r[(size_t) a]);
    }
}

struct Offset
{
    float* const* p;
    int offset;
    float* operator[] (int c) const { return p[c] + offset; }
};
struct ConstOffset
{
    const float* const* p;
    int offset;
    const float* operator[] (int c) const { return p[c] + offset; }
};
} // namespace

void parallelFor (int count, const std::function<void (int)>& fn)
{
    const int threads = std::max (1, std::min (count, (int) std::thread::hardware_concurrency() - 1));
    std::atomic<int> next { 0 };
    auto work = [&] {
        for (int i = next++; i < count; i = next++)
            fn (i);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back (work);
    work();
    for (auto& t : pool)
        t.join();
}

bool stretchBuffer (const Stereo& in, Stereo& out, int outLength, double sampleRate, float semis, bool formant,
                    const std::function<bool()>& cancelled)
{
    const int inLength = in.size();
    out.resize (outLength);
    if (inLength == 0 || outLength == 0)
        return true;

    const bool identity = inLength == outLength && std::abs (semis) < 1e-4f;
    if (identity)
    {
        out.l = in.l;
        out.r = in.r;
        return true;
    }

    Stretch s;
    configure (s, sampleRate, semis, formant);
    const float rate = (float) inLength / (float) outLength;
    const int seekLength = s.outputSeekLength (rate);
    if (inLength < seekLength + 16)
    {
        resampleLinear (in, out, outLength); // too short to stretch properly
        return true;
    }

    const float* inPtr[2] = { in.l.data(), in.r.data() };
    float* outPtr[2] = { out.l.data(), out.r.data() };

    // Same steps as SignalsmithStretch::exact(), split into pieces so it can be cancelled.
    s.outputSeek (inPtr, seekLength);
    const int outMain = outLength - (int) (seekLength / rate);
    const int inMain = inLength - seekLength;
    const int piece = (int) (sampleRate * 2.0);
    int doneOut = 0, doneIn = 0;
    while (doneOut < outMain)
    {
        if (cancelled && cancelled())
            return false;
        const int o = std::min (piece, outMain - doneOut);
        const int targetIn = (int) std::llround ((double) (doneOut + o) * inMain / std::max (1, outMain));
        const int i = std::max (0, targetIn - doneIn);
        s.process (ConstOffset { inPtr, seekLength + doneIn }, i, Offset { outPtr, doneOut }, o);
        doneOut += o;
        doneIn += i;
    }
    s.flush (Offset { outPtr, outMain }, outLength - outMain, rate);
    return true;
}

bool stretchBufferChunked (const Stereo& in, Stereo& out, int outLength, double sampleRate, float semis, bool formant,
                           const std::function<bool()>& cancelled)
{
    const int inLength = in.size();
    const int chunk = (int) (sampleRate * 12.0);
    if (inLength <= chunk * 2)
        return stretchBuffer (in, out, outLength, sampleRate, semis, formant, cancelled);

    out.resize (outLength);
    const double ratio = (double) outLength / inLength;
    const int margin = (int) (sampleRate * 0.5);
    const int fade = (int) (sampleRate * 0.02);
    const int numChunks = (inLength + chunk - 1) / chunk;
    std::vector<Stereo> parts ((size_t) numChunks);
    std::atomic<bool> stop { false };

    parallelFor (numChunks, [&] (int k) {
        if (stop || (cancelled && cancelled()))
        {
            stop = true;
            return;
        }
        const int a = std::max (0, k * chunk - margin);
        const int b = std::min (inLength, (k + 1) * chunk + margin);
        Stereo piece;
        piece.l.assign (in.l.begin() + a, in.l.begin() + b);
        piece.r.assign (in.r.begin() + a, in.r.begin() + b);
        const int pieceOut = (int) std::llround ((b - a) * ratio);
        stretchBuffer (piece, parts[(size_t) k], pieceOut, sampleRate, semis, formant);
    });
    if (stop)
        return false;

    // Each chunk owns [k*chunk, (k+1)*chunk) of the input; neighbours crossfade across the boundary.
    for (int k = 0; k < numChunks; ++k)
    {
        const int a = std::max (0, k * chunk - margin);
        const auto& part = parts[(size_t) k];
        const int ownStart = (int) std::llround (k * chunk * ratio);
        const int ownEnd = std::min (outLength, (int) std::llround (std::min (inLength, (k + 1) * chunk) * ratio));
        const int partOrigin = (int) std::llround (a * ratio);
        const int from = k == 0 ? 0 : ownStart - fade;
        const int to = k == numChunks - 1 ? outLength : ownEnd + fade;
        for (int o = std::max (0, from); o < std::min (outLength, to); ++o)
        {
            const int idx = o - partOrigin;
            if (idx < 0 || idx >= part.size())
                continue;
            float w = 1.0f;
            if (k > 0 && o < ownStart + fade)
                w = (float) (o - (ownStart - fade)) / (2.0f * fade);
            if (k < numChunks - 1 && o > ownEnd - fade)
                w = std::min (w, (float) ((ownEnd + fade) - o) / (2.0f * fade));
            w = std::clamp (w, 0.0f, 1.0f);
            out.l[(size_t) o] += w * part.l[(size_t) idx];
            out.r[(size_t) o] += w * part.r[(size_t) idx];
        }
    }
    return true;
}

//==============================================================================
Renderer::Renderer (Published<Rendered>& r, Published<KeysRender>& k, Published<PadRenders>& pr)
    : renderedOut (r), keysOut (k), padsOut (pr), worker ([this] { run(); })
{
}

Renderer::~Renderer()
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        quit = true;
        ++mainTicket;
        ++keysTicket;
    }
    cv.notify_all();
    worker.join();
}

void Renderer::request (const RenderSettings& s)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (pending && pending->sameAs (s))
            return;
        if (! pending && inFlight && inFlight->sameAs (s))
            return;
        if (! pending && ! inFlight && lastDone && lastDone->sameAs (s))
            return;
        pending = s;
        ++mainTicket;
    }
    busy = true;
    cv.notify_all();
}

void Renderer::requestKeys (const KeysSettings& k)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        const auto* cur = renderedOut.get();
        if (! pendingKeys && lastKeys && lastKeys->sameAs (k) && cur != nullptr && cur->gen == keysForGen)
            return;
        if (pendingKeys && pendingKeys->sameAs (k))
            return;
        if (! pendingKeys && keysInFlight && keysInFlight->sameAs (k))
            return;
        pendingKeys = k;
        ++keysTicket;
    }
    busy = true;
    cv.notify_all();
}

void Renderer::requestPads (const std::vector<PadJob>& jobs)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        bool same = jobs.size() == wantedPads.size();
        for (size_t i = 0; same && i < jobs.size(); ++i)
            same = jobs[i].sameAs (wantedPads[i]);
        if (same)
            return;
        wantedPads = jobs;
        padsDirty = true;
    }
    busy = true;
    cv.notify_all();
}

void Renderer::run()
{
    for (;;)
    {
        std::optional<RenderSettings> main;
        std::optional<KeysSettings> keys;
        uint64_t mTicket = 0, kTicket = 0;
        {
            std::unique_lock<std::mutex> lock (mutex);
            cv.wait (lock, [this] { return quit || pending || pendingKeys || padsDirty; });
            if (quit)
                return;
            main = pending;
            inFlight = pending;
            pending.reset();
            mTicket = mainTicket;
            if (! main)
            {
                keys = pendingKeys;
                keysInFlight = pendingKeys;
                pendingKeys.reset();
                kTicket = keysTicket;
            }
        }

        // Pads come last: one at a time, so a newer main or Keys request can cut in between.
        if (! main && ! keys)
        {
            std::optional<PadJob> job;
            std::optional<RenderSettings> done;
            uint64_t startTicket = 0;
            const auto* cur = renderedOut.get();
            {
                std::lock_guard<std::mutex> lock (mutex);
                done = lastDone;
                startTicket = mainTicket;
                if (cur != nullptr && cur->gen != donePadsForGen)
                {
                    donePads.fill (std::nullopt); // a new main render invalidates every pad render
                    donePadsForGen = cur->gen;
                }
                for (const auto& j : wantedPads)
                    if (j.pad >= 0 && j.pad < kNumPads && ! (donePads[(size_t) j.pad] && donePads[(size_t) j.pad]->sameAs (j)))
                    {
                        job = j;
                        break;
                    }
                if (! job)
                    padsDirty = false;
            }
            if (job && cur != nullptr && done)
            {
                if (renderPad (*job, *cur, *done, startTicket))
                {
                    std::lock_guard<std::mutex> lock (mutex);
                    donePads[(size_t) job->pad] = job;
                }
                else
                {
                    std::lock_guard<std::mutex> lock (mutex);
                    if (mainTicket == startTicket)
                        donePads[(size_t) job->pad] = job; // unrenderable (too short): don't retry forever
                }
            }
            else if (job)
            {
                std::lock_guard<std::mutex> lock (mutex);
                padsDirty = false; // nothing to render against yet; the next main render re-arms it
            }
        }

        if (main)
        {
            if (renderMain (*main, mTicket))
            {
                std::lock_guard<std::mutex> lock (mutex);
                lastDone = main;
                // Keys renders belong to one main render, so redo them on top of the new one.
                if (lastKeys && ! pendingKeys)
                {
                    pendingKeys = lastKeys;
                    ++keysTicket;
                }
            }
        }
        else if (keys)
        {
            std::optional<RenderSettings> done;
            {
                std::lock_guard<std::mutex> lock (mutex);
                done = lastDone;
            }
            const auto* cur = renderedOut.get();
            if (cur != nullptr && done)
                if (renderKeys (*keys, *cur, *done, kTicket))
                {
                    std::lock_guard<std::mutex> lock (mutex);
                    lastKeys = keys;
                    keysForGen = cur->gen;
                }
        }

        std::lock_guard<std::mutex> lock (mutex);
        inFlight.reset();
        keysInFlight.reset();
        if (main)
            padsDirty = ! wantedPads.empty(); // pads belong to the main render; redo them on top of it
        if (! pending && ! pendingKeys && ! padsDirty)
            busy = false;
    }
}

bool Renderer::renderMain (const RenderSettings& s, uint64_t ticket)
{
    if (s.source == nullptr || s.source->full == nullptr)
        return false;
    const auto& src = *s.source;
    const double sr = src.sampleRate;
    auto cancelled = [this, ticket] { return mainTicket.load() != ticket; };

    auto base = [&] {
        auto r = std::make_unique<Rendered>();
        r->sourceId = src.id;
        r->sampleRate = sr;
        r->speed = s.speed;
        r->semis = s.semis;
        return r;
    };

    // Tape mode, or nothing to do: play the source as is.
    if (! s.pitchAndTime || (std::abs (s.speed - 1.0) < 1e-6 && std::abs (s.semis) < 1e-4f))
    {
        auto r = base();
        r->stretched = false;
        r->positionsPerSecond = sr;
        r->rate = s.pitchAndTime ? 1.0 : s.speed * std::pow (2.0, s.semis / 12.0);
        r->full = src.full;
        r->stems = src.stems;
        renderedOut.publish (std::move (r));
        return true;
    }

    const int outLength = (int) std::llround (src.full->size() / s.speed);
    std::vector<const Stereo*> inputs { src.full.get() };
    for (const auto& st : src.stems)
        inputs.push_back (st.get());

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool draft = pass == 0;
        if (! draft && draftOnly)
            break;
        std::vector<std::shared_ptr<Stereo>> outs (inputs.size());
        for (auto& o : outs)
            o = std::make_shared<Stereo>();
        std::atomic<bool> ok { true };

        if (draft)
        {
            // Chunks of every buffer in parallel.
            for (size_t i = 0; i < inputs.size() && ok; ++i)
                ok = stretchBufferChunked (*inputs[i], *outs[i], outLength, sr, s.semis, s.formant, cancelled);
        }
        else
        {
            parallelFor ((int) inputs.size(), [&] (int i) {
                if (! stretchBuffer (*inputs[(size_t) i], *outs[(size_t) i], outLength, sr, s.semis, s.formant, cancelled))
                    ok = false;
            });
        }
        if (! ok || cancelled())
            return false;

        auto r = base();
        r->stretched = true;
        r->draft = draft;
        r->positionsPerSecond = sr / s.speed;
        r->rate = 1.0;
        r->full = outs[0];
        for (size_t i = 1; i < outs.size(); ++i)
            r->stems.push_back (outs[i]);
        renderedOut.publish (std::move (r));
    }
    return true;
}

bool Renderer::renderKeys (const KeysSettings& k, const Rendered& r, const RenderSettings& main, uint64_t ticket)
{
    if (k.pad < 0 || main.source == nullptr || main.source->full == nullptr)
        return false;
    const uint64_t mainAtStart = mainTicket.load();
    auto cancelled = [this, ticket, mainAtStart] { return keysTicket.load() != ticket || mainTicket.load() != mainAtStart; };

    // Rendered straight from the source (not from the already-stretched buffer), so each note
    // goes through HQ Stretch once.
    const auto& src = *main.source;
    const bool stretched = r.stretched;
    const double endSec = std::min (k.endSec, k.startSec + kMaxKeysSliceSec);
    const int a = std::clamp ((int) (k.startSec * src.sampleRate), 0, src.full->size());
    const int b = std::clamp ((int) (endSec * src.sampleRate), a, src.full->size());
    if (b - a < 16)
        return false;

    // Mix the slice the way the pad would hear it (its stems, at the current stem levels).
    Stereo slice;
    slice.resize (b - a);
    const auto& stems = src.stems;
    const bool useStems = ! stems.empty() && [&] {
        for (size_t s = 0; s < stems.size(); ++s)
        {
            const float w = ((k.mask >> s) & 1u) ? k.stemGain[s] : 0.0f;
            if (std::abs (w - 1.0f) > 1e-4f)
                return true;
        }
        return false;
    }();
    for (int i = a; i < b; ++i)
    {
        float l = 0, rr = 0;
        if (useStems)
            for (size_t s = 0; s < stems.size(); ++s)
            {
                const float w = ((k.mask >> s) & 1u) ? k.stemGain[s] : 0.0f;
                l += w * stems[s]->l[(size_t) i];
                rr += w * stems[s]->r[(size_t) i];
            }
        else
        {
            l = src.full->l[(size_t) i];
            rr = src.full->r[(size_t) i];
        }
        slice.l[(size_t) (i - a)] = l;
        slice.r[(size_t) (i - a)] = rr;
    }

    const int outLength = stretched ? (int) std::llround (slice.size() / main.speed) : slice.size();
    const float baseSemis = stretched ? main.semis : 0.0f;

    auto out = std::make_unique<KeysRender>();
    out->renderedGen = r.gen;
    out->pad = k.pad;
    out->mask = k.mask;
    out->startPos = k.startSec * r.positionsPerSecond;
    out->endPos = endSec * r.positionsPerSecond;
    std::atomic<bool> ok { true };
    parallelFor (2 * kKeysRange + 1, [&] (int idx) {
        auto buf = std::make_shared<Stereo>();
        const float semis = baseSemis + (float) (idx - kKeysRange);
        if (! stretchBuffer (slice, *buf, outLength, src.sampleRate, semis, k.formant, cancelled))
            ok = false;
        out->notes[(size_t) idx] = buf;
    });
    if (! ok || cancelled())
        return false;
    keysOut.publish (std::move (out));
    return true;
}
StereoPtr Renderer::mixSlice (const Source& src, double startSec, double endSec, uint32_t mask,
                              const std::array<float, kMaxStems>& gains) const
{
    const int a = std::clamp ((int) (startSec * src.sampleRate), 0, src.full->size());
    const int b = std::clamp ((int) (endSec * src.sampleRate), a, src.full->size());
    auto slice = std::make_shared<Stereo>();
    slice->resize (b - a);
    const auto& stems = src.stems;
    bool useStems = false;
    for (size_t s = 0; s < stems.size(); ++s)
        useStems = useStems || std::abs ((((mask >> s) & 1u) ? gains[s] : 0.0f) - 1.0f) > 1e-4f;
    for (int i = a; i < b; ++i)
    {
        float l = 0, r = 0;
        if (useStems)
            for (size_t s = 0; s < stems.size(); ++s)
            {
                const float w = ((mask >> s) & 1u) ? gains[s] : 0.0f;
                l += w * stems[s]->l[(size_t) i];
                r += w * stems[s]->r[(size_t) i];
            }
        else
        {
            l = src.full->l[(size_t) i];
            r = src.full->r[(size_t) i];
        }
        slice->l[(size_t) (i - a)] = l;
        slice->r[(size_t) (i - a)] = r;
    }
    return slice;
}

bool Renderer::renderPad (const PadJob& j, const Rendered& r, const RenderSettings& main, uint64_t ticketAtStart)
{
    if (main.source == nullptr || main.source->full == nullptr || j.endSec <= j.startSec)
        return false;
    auto cancelled = [this, ticketAtStart] { return mainTicket.load() != ticketAtStart; };
    const auto& src = *main.source;
    const auto slice = mixSlice (src, j.startSec, j.endSec, j.mask, j.stemGain);
    if (slice->size() < 16)
        return false;

    // In HQ Stretch the pad also carries the song's key and tempo; in tape mode the voice adds them.
    const double lengthFactor = 1.0 + j.stretch / 100.0;
    const double base = r.stretched ? slice->size() / main.speed : (double) slice->size();
    const int outLength = std::max (16, (int) std::llround (base * lengthFactor));
    const float semis = (r.stretched ? main.semis : 0.0f) + j.keyShift;
    auto buf = std::make_shared<Stereo>();
    if (! stretchBuffer (*slice, *buf, outLength, src.sampleRate, semis, main.formant, cancelled))
        return false;

    PadRender pr;
    pr.buf = buf;
    pr.renderedGen = r.gen;
    pr.cue = j.startSec;
    pr.end = j.endSec;
    pr.keyShift = j.keyShift;
    pr.stretch = j.stretch;
    pr.mask = j.mask;
    pr.startPos = j.startSec * r.positionsPerSecond;
    pr.endPos = j.endSec * r.positionsPerSecond;

    auto next = std::make_unique<PadRenders>();
    if (const auto* cur = padsOut.get())
        next->pads = cur->pads;
    next->pads[(size_t) j.pad] = pr;
    padsOut.publish (std::move (next));
    return true;
}
} // namespace chop
