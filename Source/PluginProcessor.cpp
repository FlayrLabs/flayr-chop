#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "stems/Stems.h"

#include <juce_cryptography/juce_cryptography.h>
#include <set>

using namespace chop;

namespace
{
const juce::StringArray kTempoModes { "Original", "Host Sync", "Manual" };
const juce::StringArray kQuantize { "Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar" };
const double kQuantizeBeats[] = { 0.0, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };

SourcePtr buildSource (const LoadedSample& s, const std::vector<StereoPtr>& stems, double rate, uint64_t id)
{
    auto src = std::make_shared<Source>();
    src->id = id;
    src->sampleRate = rate;
    std::vector<StereoPtr> all (stems.size() + 1);
    all[0] = s.original;
    std::copy (stems.begin(), stems.end(), all.begin() + 1);
    std::vector<StereoPtr> out (all.size());
    parallelFor ((int) all.size(), [&] (int i) { out[(size_t) i] = resample (*all[(size_t) i], s.fileRate, rate); });
    src->full = out[0];
    src->stems.assign (out.begin() + 1, out.end());
    return src;
}

// Tempo and key again, from the separated parts: drums for the beat, bass and the rest for the key.
analysis::Result analyseFromStems (const LoadedSample& s, const std::vector<StereoPtr>& stems, const juce::StringArray& names)
{
    const int n = s.original->size();
    const auto mix = monoMix (*s.original);
    std::vector<float> drums, bass, harmonic ((size_t) n, 0.0f);
    bool anyHarmonic = false;
    for (int i = 0; i < names.size() && i < (int) stems.size(); ++i)
    {
        const auto m = monoMix (*stems[(size_t) i]);
        const auto name = names[i].toLowerCase();
        if (name.contains ("drum"))
            drums = m;
        else if (name.contains ("bass"))
            bass = m;
        else
        {
            for (int k = 0; k < n; ++k)
                harmonic[(size_t) k] += m[(size_t) k];
            anyHarmonic = true;
        }
    }
    return analysis::analyseWithStems (mix.data(), drums.empty() ? nullptr : drums.data(), bass.empty() ? nullptr : bass.data(),
                                       anyHarmonic ? harmonic.data() : nullptr, n, s.fileRate);
}

Pad sanitise (Pad p, double lengthSec)
{
    auto clampF = [] (float v, float lo, float hi, float fallback) { return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback; };
    if (! std::isfinite (p.cue) || p.cue >= lengthSec)
        p.cue = -1.0;
    if (! std::isfinite (p.end) || p.end <= p.cue || p.end > lengthSec)
        p.end = -1.0;
    p.gainDb = clampF (p.gainDb, -24.0f, 12.0f, 0.0f);
    p.pan = clampF (p.pan, -1.0f, 1.0f, 0.0f);
    p.keyShift = clampF (p.keyShift, -24.0f, 24.0f, 0.0f);
    p.stretch = clampF (p.stretch, -75.0f, 300.0f, 0.0f);
    p.filter = clampF (p.filter, -1.0f, 1.0f, 0.0f);
    p.attackMs = clampF (p.attackMs, 0.0f, 2000.0f, 1.0f);
    p.releaseMs = clampF (p.releaseMs, 1.0f, 4000.0f, 40.0f);
    p.mode = (PadMode) juce::jlimit (0, 2, (int) p.mode);
    p.choke = juce::jlimit (0, 8, p.choke);
    return p;
}

juce::String padName (int i)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + i / kPadsPerBank)) + juce::String (i % kPadsPerBank + 1);
}
} // namespace

FlayrChopProcessor::FlayrChopProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FlayrChop", createLayout())
{
    rawVolume = apvts.getRawParameterValue ("volume");
    rawFilter = apvts.getRawParameterValue ("filter");
    rawVelocity = apvts.getRawParameterValue ("velocity");
    rawMono = apvts.getRawParameterValue ("mono");
    rawKeys = apvts.getRawParameterValue ("keys");
    rawQuantize = apvts.getRawParameterValue ("quantize");
    for (int s = 0; s < kMaxStems; ++s)
    {
        rawStemLevel[(size_t) s] = apvts.getRawParameterValue (stemLevelId (s));
        rawStemMute[(size_t) s] = apvts.getRawParameterValue (stemMuteId (s));
        rawStemSolo[(size_t) s] = apvts.getRawParameterValue (stemSoloId (s));
    }
    publishPads();
    refreshStateSnapshot();
    startTimerHz (30);
#if FLAYR_CHOP_TEST_HOOKS
    // Local UI tests only (never in release builds): load a track at start-up.
    if (const auto* path = std::getenv ("FLAYR_CHOP_AUTOLOAD"))
        loadFile (juce::File (juce::String::fromUTF8 (path)));
#endif
}

FlayrChopProcessor::~FlayrChopProcessor()
{
    lifetime.reset(); // queued state restores see this and do nothing
    stopTimer();
    cancelJob = true;
    if (worker.joinable())
        worker.join();
}

juce::AudioProcessorValueTreeState::ParameterLayout FlayrChopProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto pct = AudioParameterFloatAttributes().withLabel ("%");

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "keyShift", 1 }, "Key Shift", -12, 12, 0,
                                                      AudioParameterIntAttributes().withLabel ("st")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "stretch", 1 }, "HQ Stretch", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "formant", 1 }, "Keep Formants", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "tempoMode", 1 }, "Tempo Mode", kTempoModes, 1));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tempo", 1 }, "Tempo",
                                                        NormalisableRange<float> (40.0f, 250.0f, 0.01f), 120.0f,
                                                        AudioParameterFloatAttributes().withLabel ("BPM")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "volume", 1 }, "Volume",
                                                        NormalisableRange<float> (-48.0f, 12.0f, 0.1f, 2.0f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "filter", 1 }, "Filter",
                                                        NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "velocity", 1 }, "Velocity Sensitivity",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 30.0f, pct));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "mono", 1 }, "Mono", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "keys", 1 }, "Keys Mode", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "quantize", 1 }, "Quantize", kQuantize, 0));

    for (int s = 0; s < kMaxStems; ++s)
    {
        const String n ("Stem " + String (s + 1));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { stemLevelId (s), 1 }, n + " Level",
                                                            NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pct));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { stemMuteId (s), 1 }, n + " Mute", false));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { stemSoloId (s), 1 }, n + " Solo", false));
    }
    return { p.begin(), p.end() };
}

bool FlayrChopProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void FlayrChopProcessor::prepareToPlay (double sampleRate, int)
{
    sampler.prepare (sampleRate);
    hostRate = sampleRate;
    for (auto& p : pending)
        p = Pending {};
    held.fill (false);
}

//==============================================================================
Globals FlayrChopProcessor::readGlobals() const
{
    // Parameters can be set by hosts and automation; a non-finite one must not reach the audio.
    auto finiteOr = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };
    Globals g;
    g.gain = juce::Decibels::decibelsToGain (finiteOr (rawVolume->load(), 0.0f), -47.9f);
    g.filter = juce::jlimit (-1.0f, 1.0f, finiteOr (rawFilter->load(), 0.0f) / 100.0f);
    g.velocity = juce::jlimit (0.0f, 1.0f, finiteOr (rawVelocity->load(), 30.0f) / 100.0f);
    g.mono = rawMono->load() > 0.5f;
    g.keysMode = rawKeys->load() > 0.5f;
    g.keysPad = keysPadAudio.load (std::memory_order_relaxed);
    g.numStems = numStemsAudio.load (std::memory_order_relaxed);
    bool anySolo = false;
    for (int s = 0; s < g.numStems; ++s)
        anySolo = anySolo || rawStemSolo[(size_t) s]->load() > 0.5f;
    for (int s = 0; s < kMaxStems; ++s)
    {
        float v = juce::jlimit (0.0f, 1.0f, finiteOr (rawStemLevel[(size_t) s]->load(), 100.0f) / 100.0f);
        if (rawStemMute[(size_t) s]->load() > 0.5f || (anySolo && rawStemSolo[(size_t) s]->load() < 0.5f))
            v = 0.0f;
        g.stemGain[(size_t) s] = v;
    }
    return g;
}

double FlayrChopProcessor::quantizeGridBeats() const
{
    const int q = juce::jlimit (0, kQuantize.size() - 1, (int) rawQuantize->load());
    return kQuantizeBeats[q];
}

void FlayrChopProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int n = buffer.getNumSamples();

    double bpm = 0.0, ppq = 0.0;
    bool playing = false, hasPpq = false;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto b = pos->getBpm())
                bpm = *b;
            if (const auto q = pos->getPpqPosition())
            {
                ppq = *q;
                hasPpq = true;
            }
        }
    hostBpm.store (bpm, std::memory_order_relaxed);
    hostPlaying.store (playing, std::memory_order_relaxed);
    const bool canQuantize = playing && hasPpq && bpm > 0.0;

    Context c;
    c.rendered = rendered.get();
    c.pads = padTable.get();
    c.keys = keysRendered.get();
    c.padRenders = padRendered.get();
    c.g = readGlobals();

    // Transport commands from the editor.
    if (const int cmd = transportCmd.exchange (0))
    {
        if (cmd == 1)
            sampler.startTransport (transportFrom.load(), c);
        else
            sampler.stopTransport();
    }

    // Notes from the editor play at the start of the block.
    {
        const auto scope = uiFifo.read (uiFifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i)
            midi.addEvent (uiNotes[(size_t) (scope.startIndex1 + i)].second
                               ? juce::MidiMessage::noteOn (1, uiNotes[(size_t) (scope.startIndex1 + i)].first, (juce::uint8) 100)
                               : juce::MidiMessage::noteOff (1, uiNotes[(size_t) (scope.startIndex1 + i)].first),
                           0);
        for (int i = 0; i < scope.blockSize2; ++i)
            midi.addEvent (uiNotes[(size_t) (scope.startIndex2 + i)].second
                               ? juce::MidiMessage::noteOn (1, uiNotes[(size_t) (scope.startIndex2 + i)].first, (juce::uint8) 100)
                               : juce::MidiMessage::noteOff (1, uiNotes[(size_t) (scope.startIndex2 + i)].first),
                           0);
    }

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    auto renderTo = [&] (int from, int to) {
        if (to > from)
            sampler.render (L + from, R + from, to - from, c);
    };

    // Events in time order: pending quantized hits and incoming MIDI.
    int cursor = 0;
    auto firePendingUntil = [&] (int until) {
        for (;;)
        {
            int best = -1;
            for (int i = 0; i < (int) pending.size(); ++i)
                if (pending[(size_t) i].note >= 0 && pending[(size_t) i].samplesLeft < until
                    && (best < 0 || pending[(size_t) i].samplesLeft < pending[(size_t) best].samplesLeft))
                    best = i;
            if (best < 0)
                return;
            auto& p = pending[(size_t) best];
            const int at = (int) juce::jlimit<int64_t> (cursor, until, p.samplesLeft);
            renderTo (cursor, at);
            cursor = at;
            sampler.noteOn (p.note, p.velocity, c);
            if (p.releaseAfter)
                sampler.noteOff (p.note);
            p = Pending {};
        }
    };

    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        firePendingUntil (at);
        renderTo (cursor, at);
        cursor = at;
        const auto msg = meta.getMessage();

        if (msg.isNoteOn())
        {
            const int note = msg.getNoteNumber();
            held[(size_t) note] = true;
            const double grid = quantizeGridBeats();
            int64_t delay = 0;
            if (grid > 0.0 && canQuantize)
            {
                const double beatsPerSample = bpm / 60.0 / getSampleRate();
                const double now = ppq + at * beatsPerSample;
                const double prev = std::floor (now / grid + 1e-9) * grid;
                const double late = (now - prev) / beatsPerSample;
                // Hits up to 30 ms late snap back to "now" rather than waiting a whole step.
                if (late > 0.03 * getSampleRate())
                    delay = (int64_t) std::llround ((prev + grid - now) / beatsPerSample);
            }
            // An empty pad drops a cue at the playhead instead of playing (pads mode only).
            const int padHit = note - kFirstPadNote;
            if (! c.g.keysMode && c.pads != nullptr && padHit >= 0 && padHit < kNumPads && ! c.pads->pads[(size_t) padHit].hasCue())
            {
                const float cueAt = sampler.isPreviewing() ? (float) sampler.previewSourceSec() : (float) idlePlayhead.load();
                const auto scope = cueFifo.write (1);
                if (scope.blockSize1 > 0)
                    cueHits[(size_t) scope.startIndex1] = { padHit, cueAt };
                else if (scope.blockSize2 > 0)
                    cueHits[(size_t) scope.startIndex2] = { padHit, cueAt };
                continue;
            }
            if (delay <= 0)
                sampler.noteOn (note, msg.getFloatVelocity(), c);
            else
                for (auto& p : pending)
                    if (p.note < 0)
                    {
                        p = { note, msg.getFloatVelocity(), at + delay, false };
                        break;
                    }
        }
        else if (msg.isNoteOff())
        {
            const int note = msg.getNoteNumber();
            held[(size_t) note] = false;
            bool waiting = false;
            for (auto& p : pending)
                if (p.note == note)
                {
                    p.releaseAfter = true;
                    waiting = true;
                }
            if (! waiting)
                sampler.noteOff (note);
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            sampler.allNotesOff();
            for (auto& p : pending)
                p = Pending {};
        }
    }
    firePendingUntil (n);
    renderTo (cursor, n);

    // Last line of defence for the host: never hand it NaN, inf or a blast. Anything non-finite is
    // silenced and the filter state (where it would otherwise stick) starts over.
    bool nonFinite = false;
    for (int ch = 0; ch < 2; ++ch)
    {
        float* x = ch == 0 ? L : R;
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (x[i]))
            {
                x[i] = 0.0f;
                nonFinite = true;
            }
            x[i] = juce::jlimit (-4.0f, 4.0f, x[i]);
        }
    }
    if (nonFinite)
        sampler.resetFilter();

    // The playhead rides along with the pad that's playing, and stays where it stopped.
    if (! sampler.isPreviewing())
        if (const int fv = sampler.followVoice.load (std::memory_order_relaxed); fv >= 0)
            if (sampler.views[(size_t) fv].pad.load (std::memory_order_relaxed) >= 0)
                idlePlayhead.store ((double) sampler.views[(size_t) fv].sourceSec.load (std::memory_order_relaxed), std::memory_order_relaxed);

    // Hits still waiting move one block closer.
    for (auto& p : pending)
        if (p.note >= 0)
            p.samplesLeft -= n;

    // Tell the publishers what is still being read.
    const uint64_t renderedOldest = std::min (c.rendered != nullptr ? c.rendered->gen : UINT64_MAX, sampler.oldestRenderedInUse());
    rendered.setOldestInUse (renderedOldest == UINT64_MAX ? 0 : renderedOldest);
    const uint64_t keysOldest = std::min (c.keys != nullptr ? c.keys->gen : UINT64_MAX, sampler.oldestKeysInUse());
    keysRendered.setOldestInUse (keysOldest == UINT64_MAX ? 0 : keysOldest);
    padTable.setOldestInUse (c.pads != nullptr ? c.pads->gen : 0);
    const uint64_t padsOldest = std::min (c.padRenders != nullptr ? c.padRenders->gen : UINT64_MAX, sampler.oldestPadRendersInUse());
    padRendered.setOldestInUse (padsOldest == UINT64_MAX ? 0 : padsOldest);

    midi.clear();
}

//==============================================================================
void FlayrChopProcessor::triggerFromUI (int note, bool on)
{
    const auto scope = uiFifo.write (1);
    if (scope.blockSize1 > 0)
        uiNotes[(size_t) scope.startIndex1] = { note, on };
    else if (scope.blockSize2 > 0)
        uiNotes[(size_t) scope.startIndex2] = { note, on };
}

void FlayrChopProcessor::publishPads()
{
    auto t = std::make_unique<PadTable>();
    t->pads = pads;
    resolveEnds (*t, sample ? sample->lengthSec() : 0.0);
    padTable.publish (std::move (t));
    keysPadAudio = selectedPad;
    sendChangeMessage();
}

void FlayrChopProcessor::setPad (int i, const Pad& p)
{
    if (! juce::isPositiveAndBelow (i, kNumPads))
        return;
    pads[(size_t) i] = p;
    publishPads();
}

void FlayrChopProcessor::setCue (int pad, double seconds)
{
    if (! juce::isPositiveAndBelow (pad, kNumPads) || sample == nullptr)
        return;
    seconds = juce::jlimit (0.0, juce::jmax (0.0, sample->lengthSec() - 0.01), seconds);
    if (snapUI)
        seconds = analysis::snapToTransient (sample->analysis, seconds, 0.03);
    auto p = pads[(size_t) pad];
    p.cue = seconds;
    if (p.end <= p.cue)
        p.end = -1.0;
    setPad (pad, p);
}

double FlayrChopProcessor::padEnd (int pad) const
{
    if (const auto* t = padTable.get())
        return t->endSec[(size_t) juce::jlimit (0, kNumPads - 1, pad)];
    return 0.0;
}

int FlayrChopProcessor::padAtTime (double seconds) const
{
    const auto* t = padTable.get();
    if (t == nullptr)
        return -1;
    int best = -1;
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = t->pads[(size_t) i];
        if (p.hasCue() && seconds >= p.cue && seconds < t->endSec[(size_t) i])
            if (best < 0 || p.cue > t->pads[(size_t) best].cue)
                best = i;
    }
    return best;
}

void FlayrChopProcessor::chop (analysis::ChopMode mode, int count, int firstPad, double beatsPerCue)
{
    if (sample == nullptr)
        return;
    count = juce::jlimit (1, kNumPads - firstPad, count);
    auto a = sample->analysis;
    a.bpm = sampleBpm;
    a.firstBeatSec = sampleFirstBeat;
    const auto mono = monoMix (*sample->original);
    const auto cues = analysis::chop (a, mono.data(), (int) mono.size(), sample->fileRate, count, mode, beatsPerCue);
    // Favorites keep their cue; the rest are filled in order with cues not already taken.
    size_t next = 0;
    for (int i = 0; i < count; ++i)
    {
        auto& p = pads[(size_t) (firstPad + i)];
        if (p.favorite)
            continue;
        p = Pad {};
        p.cue = next < cues.size() ? cues[next++] : -1.0;
    }
    publishPads();
}

void FlayrChopProcessor::clearPads (int firstPad, int count)
{
    for (int i = firstPad; i < juce::jmin (kNumPads, firstPad + count); ++i)
        pads[(size_t) i] = Pad {};
    publishPads();
}

void FlayrChopProcessor::setSampleBpm (double bpm, bool fromUser)
{
    sampleBpm = juce::jlimit (20.0, 400.0, bpm);
    bpmFromUser = bpmFromUser || fromUser;
    sendChangeMessage();
}

void FlayrChopProcessor::setSampleKey (int key, bool minor, bool fromUser)
{
    sampleKey = ((key % 12) + 12) % 12;
    sampleMinor = minor;
    keyFromUser = keyFromUser || fromUser;
    sendChangeMessage();
}

int FlayrChopProcessor::getTargetKey() const
{
    const int shift = (int) apvts.getRawParameterValue ("keyShift")->load();
    return (((sampleKey + shift) % 12) + 12) % 12;
}

double FlayrChopProcessor::getTargetBpm() const
{
    const int mode = (int) apvts.getRawParameterValue ("tempoMode")->load();
    if (mode == 1)
    {
        const double host = hostBpm.load();
        if (host <= 0.0)
            return sampleBpm;
        // Half/double-time aware: follow the host at whichever octave is closest to the sample.
        double best = host;
        for (double f : { 0.5, 2.0, 0.25, 4.0 })
            if (std::abs (std::log (host * f / sampleBpm)) < std::abs (std::log (best / sampleBpm)))
                best = host * f;
        return best;
    }
    if (mode == 2)
        return apvts.getRawParameterValue ("tempo")->load();
    return sampleBpm;
}

double FlayrChopProcessor::getSpeed() const
{
    return sampleBpm > 0.0 ? juce::jlimit (0.25, 4.0, getTargetBpm() / sampleBpm) : 1.0;
}

RenderSettings FlayrChopProcessor::currentRenderSettings() const
{
    RenderSettings s;
    s.source = source;
    s.speed = getSpeed();
    s.semis = (float) apvts.getRawParameterValue ("keyShift")->load();
    s.hqStretch = apvts.getRawParameterValue ("stretch")->load() > 0.5f;
    s.formant = apvts.getRawParameterValue ("formant")->load() > 0.5f;
    return s;
}

//==============================================================================
void FlayrChopProcessor::runJob (std::function<void()> job)
{
    cancelJob = true;
    if (worker.joinable())
        worker.join();
    cancelJob = false;
    // An exception escaping a thread would std::terminate the host (and the user's whole session),
    // so every job ends here: whatever went wrong becomes a status message.
    worker = std::thread ([this, body = std::move (job)] {
        juce::String error;
        try
        {
            body();
            return;
        }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory for that file";
        }
        catch (const std::exception& e)
        {
            error = "Something went wrong: " + juce::String (e.what());
        }
        catch (...)
        {
            error = "Something went wrong";
        }
        std::lock_guard<std::mutex> lock (inboxMutex);
        inbox.push_back ([this, error] {
            busy = Busy::None;
            status = error;
            sendChangeMessage();
        });
    });
}

void FlayrChopProcessor::loadFile (const juce::File& file)
{
    // A relink while a restored project waits: the project now saves with the new path (and the old
    // library copy no longer belongs to it).
    if (pendingPadState != nullptr)
    {
        pendingPadState->setProperty ("path", file.getFullPathName(), nullptr);
        pendingPadState->removeProperty ("libraryPath", nullptr);
    }
    loadCandidates (file, { file }, {});
}

// Restoring a project: the original path first, then its library copy. Network paths from the
// project are never touched (see isNetworkPath); the user can relink to them by hand.
void FlayrChopProcessor::restoreSample (const juce::String& path, const juce::String& libraryPath)
{
    const bool absolute = juce::File::isAbsolutePath (path);
    const auto original = absolute ? juce::File (path) : juce::File();
    std::vector<juce::File> candidates;
    if (absolute && ! isNetworkPath (path))
        candidates.push_back (original);
    juce::File library;
    if (libraryPath.isNotEmpty() && ! isNetworkPath (libraryPath) && juce::File::isAbsolutePath (libraryPath))
        if (const juce::File f (libraryPath); f.isAChildOf (sampleLibraryFolder()))
            library = f;
    if (library != juce::File())
        candidates.push_back (library);

    if (candidates.empty())
    {
        // A load still running belongs to the old project: drop its result, it must not take this one's pads.
        ++loadGeneration;
        if (busy == Busy::Loading)
        {
            cancelJob = true;
            busy = Busy::None;
        }
        // Keep everything so a relink restores the project as it was.
        missingFile = original;
        status = "Missing file: " + path;
        if (isNetworkPath (path))
            status << "  (network locations aren't opened automatically: drop the file here to relink)";
        sendChangeMessage();
        return;
    }
    loadCandidates (original, candidates, library);
}

void FlayrChopProcessor::loadCandidates (const juce::File& original, std::vector<juce::File> candidates,
                                         const juce::File& knownLibraryCopy)
{
    const auto generation = ++loadGeneration;
    busy = Busy::Loading;
    status = "Loading " + original.getFileName() + "...";
    progress = 0.0f;
    sendChangeMessage();
    const double rate = hostRate.load() > 0.0 ? hostRate.load() : 48000.0;

    runJob ([this, original, candidates, knownLibraryCopy, rate, generation] {
        auto loaded = std::make_shared<LoadedSample>();
        juce::String error;
        bool ok = false;
        for (const auto& f : candidates)
        {
            juce::String why;
            if (loadSample (f, *loaded, why, [this] { return cancelJob.load(); }))
            {
                ok = true;
                break;
            }
            if (error.isEmpty())
                error = why;
            if (cancelJob)
                break;
        }
        if (! ok)
        {
            const bool gone = ! isNetworkPath (original.getFullPathName()) && ! original.existsAsFile();
            std::lock_guard<std::mutex> lock (inboxMutex);
            inbox.push_back ([this, error, original, gone, generation] {
                if (generation != loadGeneration)
                    return; // a newer load owns the model (and the pending restore) now
                busy = Busy::None;
                status = gone ? "Missing file: " + original.getFullPathName() : error;
                if (sample == nullptr && error.isNotEmpty())
                    missingFile = original;
                sendChangeMessage();
            });
            return;
        }

        // A library copy, so a sandboxed host can read the sample next time too (see libraryCopyOf).
        juce::File library = knownLibraryCopy;
        if (isInLibrary (loaded->file)) // already somewhere hosts can read: no copy needed
            library = loaded->file.isAChildOf (sampleLibraryFolder()) ? loaded->file : juce::File();
        else if (! (library.existsAsFile() && library.getSize() == loaded->file.getSize()))
            library = libraryCopyOf (loaded->file);
        loaded->cacheFile = library != juce::File() ? library : loaded->file;

        std::vector<StereoPtr> stems;
        juce::StringArray names;
        if (! loadCachedStems (loaded->cacheFile, loaded->original->size(), stems, names) && loaded->cacheFile != loaded->file)
            loadCachedStems (loaded->file, loaded->original->size(), stems, names);
        const auto id = (uint64_t) juce::Time::currentTimeMillis();
        auto src = buildSource (*loaded, stems, rate, id);
        const auto refined = stems.empty() ? analysis::Result {} : analyseFromStems (*loaded, stems, names);

        std::lock_guard<std::mutex> lock (inboxMutex);
        inbox.push_back ([this, loaded, names, src, rate, refined, original, library, generation] {
            if (generation != loadGeneration)
                return; // an older load finishing late must not consume a newer pending restore
            applyLoaded (loaded, pendingPadState != nullptr);
            sampleOriginal = original;
            sampleLibrary = library;
            applyRefined (loaded, refined);
            stemNames = names;
            source = src;
            sourceRate = rate;
            numStemsAudio = names.size();
            busy = Busy::None;
            status = names.isEmpty() ? juce::String() : "Stems loaded from cache";
            renderer.request (currentRenderSettings());
            sendChangeMessage();
        });
    });
}

void FlayrChopProcessor::applyLoaded (std::shared_ptr<LoadedSample> loaded, bool keepPads)
{
    sample = loaded;
    missingFile = juce::File();
    refinedFromStems = false;
    const auto& a = loaded->analysis;
    if (pendingPadState != nullptr)
    {
        // Restoring a project: use its tempo, key and pads instead of the fresh analysis.
        const auto& st = *pendingPadState;
        // Project data is untrusted: keep every value finite and in range.
        auto finiteOr = [] (double v, double fallback) { return std::isfinite (v) ? v : fallback; };
        sampleBpm = juce::jlimit (20.0, 400.0, finiteOr (st.getProperty ("bpm", a.bpm), 120.0));
        sampleFirstBeat = juce::jlimit (0.0, loaded->lengthSec(), finiteOr (st.getProperty ("firstBeat", a.firstBeatSec), 0.0));
        sampleKey = ((int) st.getProperty ("key", a.key) % 12 + 12) % 12;
        sampleMinor = st.getProperty ("minor", a.minor);
        bpmFromUser = st.getProperty ("bpmFromUser", false);
        keyFromUser = st.getProperty ("keyFromUser", false);
        for (int i = 0; i < kNumPads; ++i)
        {
            const auto t = st.getChildWithProperty ("index", i);
            Pad p;
            if (t.isValid())
            {
                p.cue = t.getProperty ("cue", -1.0);
                p.end = t.getProperty ("end", -1.0);
                p.gainDb = t.getProperty ("gain", 0.0f);
                p.pan = t.getProperty ("pan", 0.0f);
                // 1.0/1.1 had tape-style "tape" pitch; it becomes the (time-kept) key shift.
                p.keyShift = t.getProperty ("keyShift", t.getProperty ("tape", 0.0f));
                p.stretch = t.getProperty ("stretch", 0.0f);
                p.filter = t.getProperty ("filter", 0.0f);
                p.favorite = t.getProperty ("favorite", false);
                p.attackMs = t.getProperty ("attack", 1.0f);
                p.releaseMs = t.getProperty ("release", 40.0f);
                p.mode = (PadMode) (int) t.getProperty ("mode", 0);
                p.reverse = t.getProperty ("reverse", false);
                p.choke = t.getProperty ("choke", 0);
                p.stemMask = (uint32_t) (juce::int64) t.getProperty ("stems", (juce::int64) 0xffffffff);
            }
            pads[(size_t) i] = sanitise (p, loaded->lengthSec());
        }
        pendingPadState.reset();
        publishPads();
    }
    else
    {
        bpmFromUser = keyFromUser = false;
        sampleBpm = a.bpm > 0 ? a.bpm : 120.0;
        sampleFirstBeat = a.firstBeatSec;
        sampleKey = a.key;
        sampleMinor = a.minor;
        if (! keepPads)
        {
            pads.fill (Pad {});
            chop (analysis::ChopMode::Auto, kPadsPerBank, 0);
            selectedPad = 0;
            bank = 0;
        }
    }
}

// Downloads the stem model on first use (checked against its pinned SHA-256).
static bool downloadModel (const std::string& path, std::atomic<float>& progress, const std::atomic<bool>& cancel, juce::String& error)
{
    const juce::File dest { juce::String (path) };
    dest.getParentDirectory().createDirectory();
    // A temp file of our own in the same folder (so the final move is a rename): several plug-in
    // instances can download at once, and they used to write into one shared ".part".
    const auto tmp = dest.getSiblingFile (dest.getFileName() + "." + juce::Uuid().toString() + ".part");
    const juce::int64 expected = stems::defaultModelSize();
    int status = 0;
    auto stream = juce::URL (stems::defaultModelUrl())
                      .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                              .withConnectionTimeoutMs (15000)
                                              .withNumRedirectsToFollow (8)
                                              .withStatusCode (&status));
    if (stream == nullptr || (status != 0 && status != 200))
    {
        error = "couldn't reach the model download (HTTP " + juce::String (status) + ")";
#if JUCE_LINUX
        error << ". Run scripts/fetch-model.sh from the Flayr Chop source to install it.";
#endif
        return false;
    }
    const auto total = stream->getTotalLength();
    if (total > 0 && total != expected)
    {
        error = "the download server sent an unexpected file (" + juce::String (total) + " bytes)";
        return false;
    }
    juce::int64 done = 0;
    {
        juce::FileOutputStream out (tmp);
        if (out.failedToOpen())
        {
            error = "can't write " + tmp.getFullPathName();
            return false;
        }
        juce::HeapBlock<char> buf (1 << 16);
        while (! stream->isExhausted())
        {
            if (cancel)
            {
                out.flush();
                error = "cancelled";
                break;
            }
            const int n = stream->read (buf.get(), 1 << 16);
            if (n <= 0)
                break;
            done += n;
            if (done > expected)
            {
                error = "the download is bigger than the model should be";
                break;
            }
            if (! out.write (buf.get(), (size_t) n))
            {
                error = "can't write " + tmp.getFullPathName();
                break;
            }
            progress = (float) done / (float) expected;
        }
    }
    if (error.isEmpty() && done != expected)
        error = "the download was incomplete";
    if (error.isEmpty() && ! juce::SHA256 (tmp).toHexString().equalsIgnoreCase (stems::defaultModelSha256()))
        error = "the downloaded model failed its checksum";
    if (error.isNotEmpty())
    {
        tmp.deleteFile();
        return false;
    }
    if (! tmp.moveFileTo (dest))
    {
        tmp.deleteFile();
        error = "can't move the model into " + dest.getParentDirectory().getFullPathName();
        return false;
    }
    return true;
}

void FlayrChopProcessor::applyRefined (const std::shared_ptr<LoadedSample>& s, const analysis::Result& r)
{
    if (sample != s || r.bpm <= 0.0)
        return;
    // Onsets stay from the full mix (cues snap to what you hear); tempo, grid and key come from the stems.
    auto& a = sample->analysis;
    a.bpm = r.bpm;
    a.bpmConfidence = r.bpmConfidence;
    a.firstBeatSec = r.firstBeatSec;
    a.key = r.key;
    a.minor = r.minor;
    a.keyConfidence = r.keyConfidence;
    a.tempoCandidates = r.tempoCandidates;
    a.keyCandidates = r.keyCandidates;
    if (! bpmFromUser)
    {
        sampleBpm = r.bpm;
        sampleFirstBeat = r.firstBeatSec;
    }
    if (! keyFromUser)
    {
        sampleKey = r.key;
        sampleMinor = r.minor;
    }
    refinedFromStems = true;
    sendChangeMessage();
}

void FlayrChopProcessor::separateStems()
{
    if (sample == nullptr || busy != Busy::None)
        return;
    auto modelPath = stems::defaultModelPath();
    if (modelPath.empty()) // no HOME / APPDATA in the host's environment: ask the OS instead
        modelPath = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                        .getChildFile ("Application Support")
#endif
                        .getChildFile ("Flayr Chop/models")
                        .getChildFile (stems::defaultModelFileName())
                        .getFullPathName()
                        .toStdString();
    const bool needModel = ! stems::modelAvailable (modelPath);
    busy = Busy::Separating;
    progress = 0.0f;
    status = needModel ? "Downloading the stem model (84 MB, one time)..." : "Separating stems...";
    sendChangeMessage();

    auto s = sample;
    const double rate = sourceRate > 0 ? sourceRate : 48000.0;
    runJob ([this, s, modelPath, rate, needModel] {
        stems::Separated sep;
        std::string err;
        if (needModel)
        {
            juce::String dlError;
            if (! downloadModel (modelPath, progress, cancelJob, dlError))
            {
                std::lock_guard<std::mutex> lock (inboxMutex);
                inbox.push_back ([this, dlError] {
                    busy = Busy::None;
                    status = "Stem model download failed: " + dlError;
                    sendChangeMessage();
                });
                return;
            }
            progress = 0.0f;
            std::lock_guard<std::mutex> lock (inboxMutex);
            inbox.push_back ([this] { status = "Separating stems..."; sendChangeMessage(); });
        }
        // 0 = the engine's own default, which caps workers by RAM (each one peaks around 0.5-0.8 GB).
        const int threads = 0;
        const bool ok = stems::separate (s->original->l.data(), s->original->r.data(), s->original->size(), s->fileRate,
                                         modelPath, sep,
                                         [this] (float p) { progress = p; return ! cancelJob.load(); }, threads, &err);
        stems::releaseCachedModels(); // ~160 MB we don't need until the next separation
        if (! ok)
        {
            std::lock_guard<std::mutex> lock (inboxMutex);
            inbox.push_back ([this, err] {
                busy = Busy::None;
                status = cancelJob ? juce::String ("Stem separation cancelled") : juce::String ("Stem separation failed: ") + err;
                sendChangeMessage();
            });
            return;
        }
        std::vector<StereoPtr> stemBufs;
        juce::StringArray names;
        for (size_t i = 0; i < sep.audio.size() && (int) i < kMaxStems; ++i)
        {
            auto st = std::make_shared<Stereo>();
            st->l = std::move (sep.audio[i][0]);
            st->r = std::move (sep.audio[i][1]);
            stemBufs.push_back (st);
            names.add (sep.names[i]);
        }
        saveCachedStems (s->cacheFile, s->fileRate, stemBufs, names);
        auto src = buildSource (*s, stemBufs, rate, (uint64_t) juce::Time::currentTimeMillis());
        const auto refined = analyseFromStems (*s, stemBufs, names);

        std::lock_guard<std::mutex> lock (inboxMutex);
        inbox.push_back ([this, s, names, src, rate, refined] {
            busy = Busy::None;
            if (sample != s)
                return;
            applyRefined (s, refined);
            stemNames = names;
            source = src;
            sourceRate = rate;
            numStemsAudio = names.size();
            status = "Stems ready";
            renderer.request (currentRenderSettings());
            sendChangeMessage();
        });
    });
}

void FlayrChopProcessor::cancelStems()
{
    if (busy == Busy::Separating)
        cancelJob = true;
}

void FlayrChopProcessor::removeStems()
{
    if (sample == nullptr || busy != Busy::None)
        return;
    stemCacheFolder (sample->cacheFile).deleteRecursively();
    stemCacheFolder (sample->file).deleteRecursively(); // older caches were keyed on the file read
    stemNames.clear();
    numStemsAudio = 0;
    auto s = std::make_shared<Source> (*source);
    s->stems.clear();
    s->id = (uint64_t) juce::Time::currentTimeMillis();
    source = s;
    renderer.request (currentRenderSettings());
    sendChangeMessage();
}

void FlayrChopProcessor::timerCallback()
{
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lock (inboxMutex);
        todo.swap (inbox);
    }
    for (auto& f : todo)
        f();

    // Saves from other threads read this snapshot; a few times a second keeps it current.
    if (! todo.empty() || ++snapshotTicks >= 3)
    {
        snapshotTicks = 0;
        refreshStateSnapshot();
    }

    // The host changed sample rate: rebuild the source at the new rate.
    const double rate = hostRate.load();
    if (sample != nullptr && rate > 0.0 && source != nullptr && std::abs (sourceRate - rate) > 1e-6 && busy == Busy::None)
    {
        busy = Busy::Loading;
        auto s = sample;
        runJob ([this, s, rate] {
            // Stems at the file's rate live in the on-disk cache, not in memory.
            std::vector<StereoPtr> st;
            juce::StringArray names;
            loadCachedStems (s->cacheFile, s->original->size(), st, names);
            auto src = buildSource (*s, st, rate, (uint64_t) juce::Time::currentTimeMillis());
            std::lock_guard<std::mutex> lock (inboxMutex);
            inbox.push_back ([this, src, rate, s] {
                busy = Busy::None;
                if (sample != s)
                    return;
                source = src;
                sourceRate = rate;
            });
        });
    }

    drainCueHits();

    if (source != nullptr)
    {
        renderer.request (currentRenderSettings());

        // Pads with their own key shift / stretch get their own renders.
        std::vector<PadJob> jobs;
        const auto stemGains = readGlobals().stemGain;
        const bool formant = apvts.getRawParameterValue ("formant")->load() > 0.5f;
        for (int i = 0; i < kNumPads; ++i)
        {
            const auto& p = pads[(size_t) i];
            if (! p.hasCue() || ! p.needsRender())
                continue;
            PadJob j;
            j.pad = i;
            j.startSec = p.cue;
            j.endSec = padEnd (i);
            j.keyShift = p.keyShift;
            j.stretch = p.stretch;
            j.mask = p.stemMask;
            j.stemGain = stemGains;
            j.formant = formant;
            jobs.push_back (j);
        }
        renderer.requestPads (jobs);
        keysPadAudio = selectedPad;
        if (apvts.getRawParameterValue ("keys")->load() > 0.5f && pads[(size_t) selectedPad].hasCue())
        {
            KeysSettings k;
            k.pad = selectedPad;
            k.startSec = pads[(size_t) selectedPad].cue;
            k.endSec = padEnd (selectedPad);
            k.mask = pads[(size_t) selectedPad].stemMask;
            k.stemGain = readGlobals().stemGain;
            k.formant = apvts.getRawParameterValue ("formant")->load() > 0.5f;
            renderer.requestKeys (k);
        }
    }

    rendered.collect();
    keysRendered.collect();
    padRendered.collect();
    padTable.collect();
}

//==============================================================================
void FlayrChopProcessor::play()
{
    transportFrom = getPlayhead();
    transportCmd = 1;
}

void FlayrChopProcessor::stop()
{
    // Stopping leaves the playhead where the music was, like a DJ deck.
    if (sampler.isPreviewing())
        setPlayhead (sampler.previewSourceSec());
    transportCmd = 2;
}

double FlayrChopProcessor::getPlayhead() const
{
    return sampler.isPreviewing() ? (double) sampler.previewView.sourceSec.load() : idlePlayhead.load();
}

void FlayrChopProcessor::setPlayhead (double seconds)
{
    playheadSec = juce::jlimit (0.0, sample ? sample->lengthSec() : 0.0, seconds);
    sampler.followVoice = -1; // a click takes the playhead off a playing pad
    idlePlayhead = playheadSec;
    if (sampler.isPreviewing())
    {
        transportFrom = playheadSec;
        transportCmd = 1; // jump
    }
}

// Where a live tap really meant: people hit a little late, so look back for the transient.
static double liveCueTime (const analysis::Result& a, double t)
{
    double best = t, bestScore = 0.0;
    for (size_t i = 0; i < a.onsetsSec.size(); ++i)
    {
        const double d = a.onsetsSec[i] - t;
        if (d < -0.12 || d > 0.03)
            continue;
        const double score = (i < a.onsetStrength.size() ? a.onsetStrength[i] : 1.0) * (1.0 - std::abs (d + 0.04) / 0.16);
        if (score > bestScore)
        {
            bestScore = score;
            best = a.onsetsSec[i];
        }
    }
    return best;
}

void FlayrChopProcessor::setCueAtPlayhead (int pad)
{
    if (sample == nullptr || ! juce::isPositiveAndBelow (pad, kNumPads))
        return;
    double t = getPlayhead();
    if (snapUI && sampler.isPreviewing())
        t = liveCueTime (sample->analysis, t);
    auto p = pads[(size_t) pad];
    const bool wasEmpty = ! p.hasCue();
    p.cue = juce::jlimit (0.0, sample->lengthSec() - 0.01, t);
    p.end = -1.0;
    if (wasEmpty)
    {
        const auto keep = p;
        p = Pad {};
        p.cue = keep.cue;
    }
    setPad (pad, p);
    selection.reset();
    selection.set ((size_t) pad);
    selectedPad = pad;
}

void FlayrChopProcessor::drainCueHits()
{
    const auto scope = cueFifo.read (cueFifo.getNumReady());
    auto apply = [this] (const std::pair<int, float>& hit) {
        if (sample == nullptr || pads[(size_t) hit.first].hasCue())
            return;
        double t = hit.second;
        if (snapUI && sampler.isPreviewing())
            t = liveCueTime (sample->analysis, t);
        Pad p;
        p.cue = juce::jlimit (0.0, sample->lengthSec() - 0.01, t);
        setPad (hit.first, p);
        selection.reset();
        selection.set ((size_t) hit.first);
        selectedPad = hit.first;
    };
    for (int i = 0; i < scope.blockSize1; ++i)
        apply (cueHits[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        apply (cueHits[(size_t) (scope.startIndex2 + i)]);
}

void FlayrChopProcessor::nudge (const std::vector<int>& which, double seconds)
{
    if (sample == nullptr)
        return;
    for (int i : which)
    {
        auto& p = pads[(size_t) i];
        if (! p.hasCue())
            continue;
        p.cue = juce::jlimit (0.0, sample->lengthSec() - 0.01, p.cue + seconds);
        if (p.end >= 0.0 && p.end <= p.cue + 0.005)
            p.end = -1.0;
    }
    publishPads();
}

void FlayrChopProcessor::setEnd (int pad, double seconds)
{
    if (sample == nullptr || ! juce::isPositiveAndBelow (pad, kNumPads) || ! pads[(size_t) pad].hasCue())
        return;
    auto p = pads[(size_t) pad];
    // Dragging the end handle onto (or before) the start removes the explicit end.
    p.end = seconds > p.cue + 0.005 ? juce::jmin (seconds, sample->lengthSec()) : -1.0;
    setPad (pad, p);
}

void FlayrChopProcessor::deletePads (const std::vector<int>& which)
{
    for (int i : which)
        if (juce::isPositiveAndBelow (i, kNumPads))
            pads[(size_t) i] = Pad {};
    publishPads();
}

void FlayrChopProcessor::movePads (const std::vector<int>& from, int toFirst, bool copy)
{
    if (from.empty())
        return;
    const int base = from.front();
    std::vector<std::pair<int, Pad>> moves;
    for (int f : from)
    {
        const int to = toFirst + (f - base);
        if (! juce::isPositiveAndBelow (to, kNumPads))
            return; // would fall off the end: refuse rather than drop pads
        moves.push_back ({ to, pads[(size_t) f] });
    }
    if (! copy)
        for (int f : from)
            pads[(size_t) f] = Pad {};
    selection.reset();
    for (const auto& [to, p] : moves)
    {
        pads[(size_t) to] = p;
        selection.set ((size_t) to);
    }
    selectedPad = moves.front().first;
    publishPads();
}

int FlayrChopProcessor::firstEmptyPad (int bankHint) const
{
    for (int k = 0; k < kNumBanks; ++k)
    {
        const int b = (bankHint + k) % kNumBanks;
        for (int i = b * kPadsPerBank; i < (b + 1) * kPadsPerBank; ++i)
            if (! pads[(size_t) i].hasCue())
                return i;
    }
    return -1;
}

std::vector<int> FlayrChopProcessor::getSelection() const
{
    std::vector<int> v;
    for (int i = 0; i < kNumPads; ++i)
        if (selection.test ((size_t) i))
            v.push_back (i);
    if (v.empty())
        v.push_back (selectedPad);
    return v;
}

void FlayrChopProcessor::select (int pad, bool additive)
{
    if (! juce::isPositiveAndBelow (pad, kNumPads))
        return;
    if (additive)
    {
        if (selection.none())
            selection.set ((size_t) selectedPad);
        selection.flip ((size_t) pad);
        if (selection.none())
            selection.set ((size_t) pad);
    }
    else
    {
        selection.reset();
        selection.set ((size_t) pad);
    }
    selectedPad = pad;
    keysPadAudio = selectedPad;
    sendChangeMessage();
}

void FlayrChopProcessor::selectAll (int bankIndex)
{
    selection.reset();
    for (int i = bankIndex * kPadsPerBank; i < (bankIndex + 1) * kPadsPerBank; ++i)
        if (pads[(size_t) i].hasCue())
            selection.set ((size_t) i);
    sendChangeMessage();
}

void FlayrChopProcessor::slicerFromPlayhead (int firstPad, int count, double beatsApart)
{
    if (sample == nullptr || sampleBpm <= 0.0)
        return;
    const double step = beatsApart * 60.0 / sampleBpm;
    double t = getPlayhead();
    for (int i = firstPad; i < juce::jmin (kNumPads, firstPad + count); ++i)
    {
        if (pads[(size_t) i].favorite)
            continue;
        Pad p;
        p.cue = t < sample->lengthSec() - 0.01 ? t : -1.0;
        pads[(size_t) i] = p;
        t += step;
    }
    publishPads();
}

void FlayrChopProcessor::randomCues (int firstPad, int count)
{
    if (sample == nullptr)
        return;
    // Random, but on the beat grid like a producer would pick (whole beats, never the same twice).
    const double beat = 60.0 / juce::jmax (20.0, sampleBpm);
    const int beats = juce::jmax (1, (int) ((sample->lengthSec() - sampleFirstBeat) / beat) - 1);
    juce::Random rng;
    std::set<int> used;
    for (int i = firstPad; i < juce::jmin (kNumPads, firstPad + count); ++i)
    {
        if (pads[(size_t) i].favorite)
            continue;
        int b = rng.nextInt (beats);
        for (int tries = 0; used.count (b) != 0 && tries < 50; ++tries)
            b = rng.nextInt (beats);
        used.insert (b);
        Pad p;
        p.cue = juce::jmax (0.0, sampleFirstBeat + b * beat);
        pads[(size_t) i] = p;
    }
    publishPads();
}

void FlayrChopProcessor::keyShiftPad (int sourcePad, int firstPad)
{
    const auto src = pads[(size_t) juce::jlimit (0, kNumPads - 1, sourcePad)];
    if (! src.hasCue())
        return;
    const double end = padEnd (sourcePad);
    for (int i = 0; i < kPadsPerBank && firstPad + i < kNumPads; ++i)
    {
        auto& p = pads[(size_t) (firstPad + i)];
        if (p.favorite && firstPad + i != sourcePad)
            continue;
        p = src;
        p.end = end; // every copy plays the same slice
        p.keyShift = src.keyShift + (float) (i - 7); // pad 8 is the original
    }
    publishPads();
}

//==============================================================================
juce::File FlayrChopProcessor::exportSlice (int pad, const juce::File& folder)
{
    const auto* r = rendered.get();
    if (r == nullptr || sample == nullptr || ! pads[(size_t) pad].hasCue())
        return {};
    const auto& p = pads[(size_t) pad];
    const int a = juce::jlimit (0, r->length(), (int) (p.cue * r->positionsPerSecond));
    const int b = juce::jlimit (a, r->length(), (int) (padEnd (pad) * r->positionsPerSecond));
    Stereo out;
    out.resize (b - a);
    const auto g = readGlobals();
    const float gain = juce::Decibels::decibelsToGain (p.gainDb);
    bool useStems = false;
    for (int s = 0; s < (int) r->stems.size(); ++s)
        useStems = useStems || std::abs ((((p.stemMask >> s) & 1u) ? g.stemGain[(size_t) s] : 0.0f) - 1.0f) > 1e-4f;
    for (int i = a; i < b; ++i)
    {
        float l = 0, rr = 0;
        if (useStems)
            for (int s = 0; s < (int) r->stems.size(); ++s)
            {
                const float w = ((p.stemMask >> s) & 1u) ? g.stemGain[(size_t) s] : 0.0f;
                l += w * r->stems[(size_t) s]->l[(size_t) i];
                rr += w * r->stems[(size_t) s]->r[(size_t) i];
            }
        else
        {
            l = r->full->l[(size_t) i];
            rr = r->full->r[(size_t) i];
        }
        const int o = p.reverse ? (b - 1 - i) : (i - a);
        out.l[(size_t) o] = l * gain;
        out.r[(size_t) o] = rr * gain;
    }
    // 3 ms fades so the exported slice never clicks.
    const int fade = juce::jmin (out.size() / 2, (int) (0.003 * r->sampleRate));
    for (int i = 0; i < fade; ++i)
    {
        const float w = (float) i / fade;
        out.l[(size_t) i] *= w;
        out.r[(size_t) i] *= w;
        out.l[(size_t) (out.size() - 1 - i)] *= w;
        out.r[(size_t) (out.size() - 1 - i)] *= w;
    }
    const auto name = juce::File::createLegalFileName (getFileName() + " - " + padName (pad) + " ("
                                                       + analysis::keyName (getTargetKey(), sampleMinor) + " "
                                                       + juce::String (getTargetBpm(), 1) + " BPM).wav");
    folder.createDirectory();
    const auto f = folder.getChildFile (name);
    return writeWav (f, out, r->sampleRate) ? f : juce::File();
}

bool FlayrChopProcessor::exportAll (const juce::File& folder, bool withStems)
{
    const auto* r = rendered.get();
    if (r == nullptr || sample == nullptr)
        return false;
    folder.createDirectory();
    const auto tag = " (" + juce::String (analysis::keyName (getTargetKey(), sampleMinor)) + " "
                     + juce::String (getTargetBpm(), 1) + " BPM)";
    bool ok = writeWav (folder.getChildFile (juce::File::createLegalFileName (getFileName() + tag + ".wav")), *r->full, r->sampleRate);
    if (withStems)
        for (int s = 0; s < (int) r->stems.size() && s < stemNames.size(); ++s)
            ok = writeWav (folder.getChildFile (juce::File::createLegalFileName (getFileName() + " - " + stemNames[s] + tag + ".wav")),
                           *r->stems[(size_t) s], r->sampleRate)
                 && ok;
    return ok;
}

//==============================================================================
static bool onMessageThread()
{
    const auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm == nullptr || mm->isThisTheMessageThread();
}

// Hosts may save and restore from any thread, but the model (sample, pads, status, the pending
// restore) belongs to the message thread. The message thread keeps a snapshot of it for saves from
// elsewhere; parameters come from the thread-safe apvts.copyState().
void FlayrChopProcessor::refreshStateSnapshot()
{
    auto sampleTree = buildSampleState();
    auto uiTree = buildUiState();
    std::lock_guard<std::mutex> lock (stateMutex);
    snapshotSample = sampleTree;
    snapshotUi = uiTree;
}

void FlayrChopProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree sampleTree, uiTree;
    if (onMessageThread())
        refreshStateSnapshot();
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        // Restored from another thread and not applied yet: that state is the project.
        if (pendingState != nullptr)
        {
            destData = *pendingState;
            return;
        }
        sampleTree = snapshotSample.createCopy();
        uiTree = snapshotUi.createCopy();
    }

    juce::ValueTree root ("FlayrChop");
    root.setProperty ("version", JucePlugin_VersionString, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (sampleTree, nullptr);
    root.appendChild (uiTree, nullptr);
    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

juce::ValueTree FlayrChopProcessor::buildSampleState() const
{
    juce::ValueTree s ("Sample");
    // "path" stays the file the user chose (older versions read only this); "libraryPath" is the copy
    // a sandboxed host can always read (see libraryCopyOf).
    const auto file = sample ? sampleOriginal : missingFile;
    s.setProperty ("path", file.getFullPathName(), nullptr);
    if (sample != nullptr && sampleLibrary != juce::File())
        s.setProperty ("libraryPath", sampleLibrary.getFullPathName(), nullptr);
    s.setProperty ("bpm", sampleBpm, nullptr);
    s.setProperty ("firstBeat", sampleFirstBeat, nullptr);
    s.setProperty ("key", sampleKey, nullptr);
    s.setProperty ("minor", sampleMinor, nullptr);
    s.setProperty ("bpmFromUser", bpmFromUser, nullptr);
    s.setProperty ("keyFromUser", keyFromUser, nullptr);
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = pads[(size_t) i];
        if (! p.hasCue())
            continue;
        juce::ValueTree t ("Pad");
        t.setProperty ("index", i, nullptr);
        t.setProperty ("cue", p.cue, nullptr);
        t.setProperty ("end", p.end, nullptr);
        t.setProperty ("gain", p.gainDb, nullptr);
        t.setProperty ("pan", p.pan, nullptr);
        t.setProperty ("keyShift", p.keyShift, nullptr);
        t.setProperty ("stretch", p.stretch, nullptr);
        t.setProperty ("filter", p.filter, nullptr);
        t.setProperty ("favorite", p.favorite, nullptr);
        t.setProperty ("attack", p.attackMs, nullptr);
        t.setProperty ("release", p.releaseMs, nullptr);
        t.setProperty ("mode", (int) p.mode, nullptr);
        t.setProperty ("reverse", p.reverse, nullptr);
        t.setProperty ("choke", p.choke, nullptr);
        t.setProperty ("stems", (juce::int64) p.stemMask, nullptr);
        s.appendChild (t, nullptr);
    }
    // A restored project not applied yet (sample missing or still loading) saves as it came in.
    return pendingPadState != nullptr ? pendingPadState->createCopy() : s;
}

juce::ValueTree FlayrChopProcessor::buildUiState() const
{
    juce::ValueTree ui ("UI");
    ui.setProperty ("selected", selectedPad, nullptr);
    ui.setProperty ("bank", bank, nullptr);
    ui.setProperty ("chopMode", chopModeUI, nullptr);
    ui.setProperty ("chopCount", chopCountUI, nullptr);
    ui.setProperty ("snap", snapUI, nullptr);
    return ui;
}

// Project state comes from files anyone can hand out, and JUCE's XmlDocument recurses per element and
// expands <!DOCTYPE>/<!ENTITY> definitions recursively, so a crafted state could exhaust the stack.
// The checks run on exactly the characters XmlDocument would read and only ever over-count:
//  - the envelope as copyXmlToBinary writes it, the declared length (which getXmlFromBinary uses),
//    no NUL inside it (getXmlFromBinary would stop there) and valid UTF-8;
//  - every '<' not followed by '/' counts as an element, wherever it is (attribute values, text,
//    unterminated quotes: nothing changes the count, and nothing ever decrements it), so the total
//    bounds the nesting XmlDocument can reach no matter how it reads the text;
//  - no '<!' at all (DTDs, entities, comments, CDATA), and '<?' only as the leading declaration;
//  - after parsing, the tree's real depth is checked without recursion before ValueTree::fromXml
//    (which recurses too) sees it.
std::unique_ptr<juce::XmlElement> FlayrChopProcessor::parseState (const void* data, int sizeInBytes)
{
    constexpr juce::uint32 magicXmlNumber = 0x21324356; // AudioProcessor::copyXmlToBinary
    if (data == nullptr || sizeInBytes <= 8 || sizeInBytes > kMaxStateBytes)
        return {};
    const auto* bytes = static_cast<const char*> (data);
    if (juce::ByteOrder::littleEndianInt (bytes) != magicXmlNumber)
        return {};
    const auto declared = (juce::int64) juce::ByteOrder::littleEndianInt (bytes + 4);
    if (declared <= 0 || declared > sizeInBytes - 8)
        return {};
    const int length = (int) declared;
    const char* text = bytes + 8;
    if (std::memchr (text, 0, (size_t) length) != nullptr || ! juce::CharPointer_UTF8::isValidString (text, length))
        return {};

    const auto xmlText = juce::String::fromUTF8 (text, length);
    int elements = 0;
    bool atStart = true;
    for (auto p = xmlText.getCharPointer();;)
    {
        const auto c = p.getAndAdvance();
        if (c == 0)
            break;
        if (c == '<')
        {
            const auto next = *p;
            if (next == '!' || (next == '?' && ! atStart))
                return {};
            if (next != '/' && next != '?' && ++elements > kMaxStateElements)
                return {};
        }
        atStart = false;
    }

    auto xml = juce::parseXML (xmlText);
    if (xml == nullptr)
        return {};
    std::vector<std::pair<const juce::XmlElement*, int>> stack { { xml.get(), 1 } };
    while (! stack.empty())
    {
        const auto [e, depth] = stack.back();
        stack.pop_back();
        if (depth > kMaxStateDepth)
            return {};
        for (const auto* child : e->getChildIterator())
            stack.push_back ({ child, depth + 1 });
    }
    return xml;
}

void FlayrChopProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = parseState (data, sizeInBytes); // pure: fine on any thread
    if (xml == nullptr)
        return;
    if (onMessageThread())
    {
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            pendingState.reset();
            ++stateGeneration;
        }
        applyState (*xml);
        return;
    }

    // Another thread: keep the blob (a save before it's applied returns it unchanged) and apply it
    // on the message thread, unless a newer restore or this processor's destruction comes first.
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        pendingState = std::make_unique<juce::MemoryBlock> (data, (size_t) sizeInBytes);
        generation = ++stateGeneration;
    }
    std::shared_ptr<const juce::XmlElement> parsed (xml.release());
    juce::MessageManager::callAsync ([this, alive = std::weak_ptr<int> (lifetime), parsed, generation] {
        if (alive.expired())
            return;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            if (stateGeneration != generation)
                return;
            pendingState.reset();
        }
        applyState (*parsed);
    });
}

void FlayrChopProcessor::applyState (const juce::XmlElement& xml)
{
    const auto root = juce::ValueTree::fromXml (xml);
    if (! root.hasType ("FlayrChop"))
        return;

    const auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid())
    {
        apvts.replaceState (params);
        // value="nan" (or inf) survives the parameter's range clamp; put such parameters back to default.
        for (auto* param : getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (const auto* raw = apvts.getRawParameterValue (ranged->getParameterID());
                    ! std::isfinite (ranged->getValue()) || (raw != nullptr && ! std::isfinite (raw->load())))
                    ranged->setValueNotifyingHost (ranged->getDefaultValue());
    }

    const auto ui = root.getChildWithName ("UI");
    selectedPad = juce::jlimit (0, kNumPads - 1, (int) ui.getProperty ("selected", 0));
    bank = juce::jlimit (0, kNumBanks - 1, (int) ui.getProperty ("bank", 0));
    chopModeUI = juce::jlimit (0, 7, (int) ui.getProperty ("chopMode", 0)); // the menu has 8 modes
    chopCountUI = juce::jlimit (4, 64, (int) ui.getProperty ("chopCount", 16));
    snapUI = ui.getProperty ("snap", true);

    const auto s = root.getChildWithName ("Sample");
    const auto path = s.getProperty ("path").toString();
    if (s.isValid() && path.isNotEmpty())
    {
        // Kept until the sample loads, so a relink (or a save meanwhile) restores the project as it was.
        pendingPadState = std::make_unique<juce::ValueTree> (s.createCopy());
        restoreSample (path, s.getProperty ("libraryPath").toString());
    }
    refreshStateSnapshot();
}

juce::AudioProcessorEditor* FlayrChopProcessor::createEditor() { return new FlayrChopEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FlayrChopProcessor(); }
