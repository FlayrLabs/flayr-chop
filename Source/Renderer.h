#pragma once

// HQ Stretch: renders the source (full mix and every stem) at the current key and tempo on a
// worker thread, and the per-semitone slices Keys mode plays. The newest request always wins,
// and a fast chunked draft is published first so knob moves are heard within a fraction of a
// second, then replaced by a seamless full-quality render.

#include <condition_variable>
#include <functional>
#include <optional>
#include <thread>

#include "Sampler.h"

namespace chop
{
struct Source
{
    uint64_t id = 0;
    double sampleRate = 48000.0;  // already at the host rate
    StereoPtr full;
    std::vector<StereoPtr> stems; // empty until separated
    double lengthSec() const { return full ? full->size() / sampleRate : 0.0; }
};
using SourcePtr = std::shared_ptr<const Source>;

struct RenderSettings
{
    SourcePtr source;
    double speed = 1.0;        // tempo factor: 1.1 plays 10% faster
    float semis = 0.0f;        // key shift
    bool pitchAndTime = true;  // false = tape (speed and pitch move together, no processing)
    bool formant = false;      // keep formants when shifting key

    bool sameAs (const RenderSettings& o) const
    {
        return source == o.source && std::abs (speed - o.speed) < 1e-6 && std::abs (semis - o.semis) < 1e-4f
               && pitchAndTime == o.pitchAndTime && formant == o.formant;
    }
};

struct KeysSettings
{
    int pad = -1;
    double startSec = 0.0, endSec = 0.0;
    uint32_t mask = 0xffffffffu;
    std::array<float, kMaxStems> stemGain { 1, 1, 1, 1, 1, 1 };
    bool formant = false;

    bool sameAs (const KeysSettings& o) const
    {
        return pad == o.pad && std::abs (startSec - o.startSec) < 1e-6 && std::abs (endSec - o.endSec) < 1e-6
               && mask == o.mask && stemGain == o.stemGain && formant == o.formant;
    }
};

// One pad with its own key shift / time stretch.
struct PadJob
{
    int pad = -1;
    double startSec = 0.0, endSec = 0.0;
    float keyShift = 0.0f, stretch = 0.0f;
    uint32_t mask = 0xffffffffu;
    std::array<float, kMaxStems> stemGain { 1, 1, 1, 1, 1, 1 };
    bool formant = false;

    bool sameAs (const PadJob& o) const
    {
        return pad == o.pad && std::abs (startSec - o.startSec) < 1e-9 && std::abs (endSec - o.endSec) < 1e-9
               && std::abs (keyShift - o.keyShift) < 1e-4f && std::abs (stretch - o.stretch) < 1e-4f && mask == o.mask
               && stemGain == o.stemGain && formant == o.formant;
    }
};

// Offline stretch of one stereo buffer to exactly outLength samples. cancel() is polled between
// pieces; returns false if cancelled.
bool stretchBuffer (const Stereo& in, Stereo& out, int outLength, double sampleRate, float semis, bool formant,
                    const std::function<bool()>& cancelled = {});

// Fast version: independent overlapping chunks stretched in parallel and crossfaded.
bool stretchBufferChunked (const Stereo& in, Stereo& out, int outLength, double sampleRate, float semis, bool formant,
                           const std::function<bool()>& cancelled = {});

// Runs fn(0..count-1) across the machine's cores.
void parallelFor (int count, const std::function<void (int)>& fn);

class Renderer
{
public:
    Renderer (Published<Rendered>& renderedOut, Published<KeysRender>& keysOut, Published<PadRenders>& padsOut);
    ~Renderer();

    void request (const RenderSettings&);
    void requestKeys (const KeysSettings&);
    void requestPads (const std::vector<PadJob>&); // every pad that needs its own render right now

    std::atomic<bool> busy { false };
    std::atomic<bool> draftOnly { false }; // tests: skip the full-quality pass

private:
    void run();
    bool renderMain (const RenderSettings&, uint64_t ticket);
    bool renderKeys (const KeysSettings&, const Rendered&, const RenderSettings& main, uint64_t ticket);
    bool renderPad (const PadJob&, const Rendered&, const RenderSettings& main, uint64_t mainTicketAtStart);
    StereoPtr mixSlice (const Source&, double startSec, double endSec, uint32_t mask, const std::array<float, kMaxStems>& gains) const;

    Published<Rendered>& renderedOut;
    Published<KeysRender>& keysOut;
    Published<PadRenders>& padsOut;

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<RenderSettings> pending, lastDone, inFlight;
    std::optional<KeysSettings> pendingKeys, lastKeys, keysInFlight;
    std::vector<PadJob> wantedPads;
    std::array<std::optional<PadJob>, kNumPads> donePads {}; // what each published pad render was made from
    uint64_t donePadsForGen = 0;
    bool padsDirty = false;
    uint64_t keysForGen = 0;
    std::atomic<uint64_t> mainTicket { 0 }, keysTicket { 0 };
    bool quit = false;
    std::thread worker;
};
} // namespace chop
