// Untrusted input through the real processor: crafted audio files, project states and paths must be
// refused cleanly (an error message, never a crash, an abort or a huge allocation).
#include "PluginProcessor.h"
#include "stems/Resampler.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstdio>
#include <atomic>
#include <thread>
#if ! defined (_WIN32)
#include <sys/stat.h>
#endif

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        ++checks;                                           \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

static const juce::File fixtures { juce::String (FLAYR_CHOP_FIXTURES) };

static void pump (double seconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

static bool waitIdle (FlayrChopProcessor& p, double timeoutSec = 60.0)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    pump (0.05);
    while (p.getBusy() != FlayrChopProcessor::Busy::None)
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > timeoutSec * 1000.0)
            return false;
        pump (0.02);
    }
    return true;
}

//==============================================================================
// WAV headers written by hand, so every field can lie.
struct Bytes
{
    juce::MemoryOutputStream out;
    Bytes& tag (const char* t) { out.write (t, 4); return *this; }
    Bytes& u16 (int v) { out.writeShort ((short) v); return *this; }
    Bytes& u32 (juce::uint32 v) { out.writeInt ((int) v); return *this; }
    Bytes& u64 (juce::uint64 v) { out.writeInt64 ((juce::int64) v); return *this; }
    Bytes& zeros (int n) { for (int i = 0; i < n; ++i) out.writeByte (0); return *this; }
    juce::File save (const juce::File& f) { f.replaceWithData (out.getData(), out.getDataSize()); return f; }
};

static Bytes& fmtChunk (Bytes& b, int format, int channels, juce::uint32 rate, int bits)
{
    const int align = channels * bits / 8;
    return b.tag ("fmt ").u32 (16).u16 (format).u16 (channels).u32 (rate).u32 (rate * (juce::uint32) align).u16 (align).u16 (bits);
}

static juce::File riffWav (const juce::File& f, juce::uint32 rate, juce::uint32 claimedDataBytes, int realDataBytes,
                           int channels = 2, int bits = 16)
{
    Bytes b;
    b.tag ("RIFF").u32 (36 + (juce::uint32) realDataBytes).tag ("WAVE");
    fmtChunk (b, 1, channels, rate, bits);
    b.tag ("data").u32 (claimedDataBytes).zeros (realDataBytes);
    return b.save (f);
}

static juce::File rf64Wav (const juce::File& f, juce::uint32 rate, juce::uint64 claimedDataBytes, int realDataBytes)
{
    Bytes b;
    b.tag ("RF64").u32 (0xffffffffu).tag ("WAVE");
    b.tag ("ds64").u32 (28).u64 (claimedDataBytes + 64).u64 (claimedDataBytes).u64 (claimedDataBytes / 8).u32 (0);
    fmtChunk (b, 3, 2, rate, 32);
    b.tag ("data").u32 (0xffffffffu).zeros (realDataBytes);
    return b.save (f);
}

static juce::File goodWav (const juce::File& f, double seconds, double sr = 48000.0)
{
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    buf.clear();
    for (int i = 0; i < buf.getNumSamples(); i += 24000)
        for (int j = 0; j < 200 && i + j < buf.getNumSamples(); ++j)
            for (int c = 0; c < 2; ++c)
                buf.setSample (c, i + j, 0.5f * (1.0f - j / 200.0f));
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
    auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return f;
}

static bool refused (const juce::File& f, juce::String& error)
{
    chop::LoadedSample s;
    error.clear();
    const bool ok = chop::loadSample (f, s, error);
    return ! ok && error.isNotEmpty() && s.original == nullptr;
}

static juce::String repeat (const juce::String& s, int n);

// Runs fn on a thread with a 512 KB stack (what macOS gives secondary threads, so what a host's or a
// std::thread's stack can be) and says whether it finished within the time.
static bool finishesOnSmallStack (std::function<void()> fn, int timeoutMs = 20000)
{
    struct Small final : juce::Thread
    {
        explicit Small (std::function<void()> f) : juce::Thread ("small stack", 512 * 1024), body (std::move (f)) {}
        void run() override { body(); }
        std::function<void()> body;
    } t (std::move (fn));
    t.startThread();
    return t.waitForThreadToExit (timeoutMs);
}

// Refused with a message, or loaded with every sample finite.
static bool refusedOrFinite (const juce::File& f)
{
    chop::LoadedSample s;
    juce::String error;
    if (! chop::loadSample (f, s, error))
        return s.original == nullptr && error.isNotEmpty();
    if (s.original == nullptr)
        return false;
    for (int i = 0; i < s.original->size(); ++i)
        if (! std::isfinite (s.original->l[(size_t) i]) || ! std::isfinite (s.original->r[(size_t) i]))
            return false;
    return std::isfinite (s.analysis.bpm);
}

//==============================================================================
static void crashFiles (const juce::File& dir)
{
    juce::String error;

    // The security report's proof-of-concept files: 4.29 GHz sample rate in RF64 (heap overflow) and
    // in plain RIFF (std::length_error abort).
    CHECK (refused (fixtures.getChildFile ("rf64-bad-rate.wav"), error), "rf64 PoC not refused");
    CHECK (refused (fixtures.getChildFile ("riff-bad-rate.wav"), error), "riff PoC not refused");

    // Rates outside 8 kHz .. 768 kHz.
    CHECK (refused (riffWav (dir.getChildFile ("slow.wav"), 4000, 400, 400), error), "4 kHz not refused");
    CHECK (refused (riffWav (dir.getChildFile ("fast.wav"), 1000000, 400, 400), error), "1 MHz not refused");

    // A sane rate, but an RF64 length of 2^62 bytes (and one that fits 20 minutes but not the file).
    CHECK (refused (rf64Wav (dir.getChildFile ("rf64-huge.wav"), 44100, 1ULL << 62, 64), error), "rf64 2^62 not refused");
    CHECK (refused (rf64Wav (dir.getChildFile ("rf64-lie.wav"), 44100, 44100ULL * 8 * 60, 64), error),
           "rf64 length beyond the file not refused: %s", error.toRawUTF8());

    // Plain RIFF claiming 2 GB of data in a tiny file: refused, or at most what the file really holds.
    {
        chop::LoadedSample s;
        const bool ok = chop::loadSample (riffWav (dir.getChildFile ("riff-lie.wav"), 44100, 0x7ffffff0u, 64), s, error);
        CHECK (! ok || (s.original != nullptr && s.original->size() <= 16), "riff lie loaded %d frames", ok ? s.original->size() : -1);
    }

    // Headers that lie in ways only one parser might notice. Whatever the reader decides, the buffer
    // must hold exactly what will be read and never more than the file can back.
    {
        auto loadedWithin = [&] (const juce::File& f, int bytesPerFrame) {
            chop::LoadedSample s;
            juce::String why;
            const bool ok = chop::loadSample (f, s, why);
            return ! ok ? why.isNotEmpty() : (s.original != nullptr && (juce::int64) s.original->size() * bytesPerFrame <= f.getSize());
        };
        // RF64 whose ds64 data size disagrees with its RIFF size field and its real size.
        {
            Bytes b;
            b.tag ("RF64").u32 (100).tag ("WAVE");
            b.tag ("ds64").u32 (28).u64 (64).u64 (1ULL << 40).u64 (4).u32 (0);
            fmtChunk (b, 1, 2, 44100, 16);
            b.tag ("data").u32 (16).zeros (16);
            CHECK (loadedWithin (b.save (dir.getChildFile ("ds64-vs-riff.wav")), 4), "ds64 vs RIFF size");
        }
        // An odd-sized chunk (with and without its pad byte) before the data.
        for (bool pad : { true, false })
        {
            Bytes b;
            b.tag ("RIFF").u32 (0x7fffffffu).tag ("WAVE");
            fmtChunk (b, 1, 2, 44100, 16);
            b.tag ("LIST").u32 (3).zeros (pad ? 4 : 3);
            b.tag ("data").u32 (0x40000000u).zeros (64);
            CHECK (loadedWithin (b.save (dir.getChildFile (pad ? "odd-pad.wav" : "odd-nopad.wav")), 4), "odd chunk padding %d", (int) pad);
        }
        // Two fmt chunks (sane, then a 4.29 GHz one) and two data chunks.
        {
            Bytes b;
            b.tag ("RIFF").u32 (200).tag ("WAVE");
            fmtChunk (b, 1, 2, 44100, 16);
            fmtChunk (b, 1, 2, 0xffffffffu, 16);
            b.tag ("data").u32 (16).zeros (16);
            b.tag ("data").u32 (0x7ffffff0u).zeros (16);
            CHECK (loadedWithin (b.save (dir.getChildFile ("two-fmt.wav")), 4), "two fmt / data chunks");
        }
        {
            Bytes b;
            b.tag ("RIFF").u32 (200).tag ("WAVE");
            fmtChunk (b, 1, 2, 0xffffffffu, 16);
            fmtChunk (b, 1, 2, 44100, 16);
            b.tag ("data").u32 (0x7ffffff0u).zeros (16);
            CHECK (loadedWithin (b.save (dir.getChildFile ("two-fmt-bad-first.wav")), 4), "bad fmt then good fmt");
        }
        // Absurd channel counts and bit depths.
        CHECK (loadedWithin (riffWav (dir.getChildFile ("chans.wav"), 44100, 0x7ffffff0u, 64, 65535, 16), 2), "65535 channels");
        CHECK (loadedWithin (riffWav (dir.getChildFile ("bits.wav"), 44100, 0x7ffffff0u, 64, 2, 0), 4), "0 bits");
        // A real file cut short (header claims 2 s).
        {
            const auto full = goodWav (dir.getChildFile ("full.wav"), 2.0);
            juce::MemoryBlock mb;
            full.loadFileAsData (mb);
            const auto cut = dir.getChildFile ("cut.wav");
            cut.replaceWithData (mb.getData(), mb.getSize() / 3);
            CHECK (loadedWithin (cut, 4), "truncated file");
        }
    }

    // Found by fuzzing: a 43-byte FLAC whose header promises 20 minutes at 192 kHz decoded to silence
    // (3 GB and minutes of CPU). Refused before any buffer is allocated.
    CHECK (refused (fixtures.getChildFile ("flac-length-bomb.flac"), error), "FLAC length bomb not refused");
    {
        // While real compressed files, even long digital silence (FLAC's best case), still load.
        juce::AudioBuffer<float> silence (2, 44100 * 30);
        silence.clear();
        const auto f = dir.getChildFile ("silence.flac");
        juce::FlacAudioFormat flac;
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
            auto w = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
            if (w != nullptr)
                w->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
        }
        chop::LoadedSample s;
        CHECK (chop::loadSample (f, s, error) && s.original != nullptr && s.original->size() == silence.getNumSamples(),
               "30 s of FLAC silence (%d bytes) refused: %s", (int) f.getSize(), error.toRawUTF8());
    }

    // Found by an independent review: within the caps, a small file of compressed silence could still
    // claim minutes at 192 kHz (a 231 KB FLAC: 20 minutes, 4-5 GB). Loads now have to fit a memory
    // budget (half the RAM) before anything is allocated; here with the budget set to 100 MB.
    {
        chop::setLoadMemoryBudgetForTests (100 * 1024 * 1024);
        juce::AudioBuffer<float> quiet (2, 192000 * 120);
        quiet.clear();
        const auto f = dir.getChildFile ("two-minutes-192k.flac");
        juce::FlacAudioFormat flac;
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
            auto w = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (192000.0).withNumChannels (2).withBitsPerSample (16));
            if (w != nullptr)
                w->writeFromAudioSampleBuffer (quiet, 0, quiet.getNumSamples());
        }
        CHECK (refused (f, error), "2 minutes at 192 kHz (%d bytes) loaded within a 100 MB budget", (int) f.getSize());
        CHECK (error.contains ("memory"), "budget refusal message: %s", error.toRawUTF8());
        chop::setLoadMemoryBudgetForTests (0);
    }

    // Found by fuzzing: 32-bit float WAV data is passed through as stored, so NaN, infinities and
    // absurd values reached the analysis and the audio. They load as silence / clamped now.
    {
        const float values[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(), 1e30f, -1e30f, 0.5f, -0.25f, 0.0f };
        Bytes b;
        b.tag ("RIFF").u32 (36 + (juce::uint32) sizeof (values) * 100).tag ("WAVE");
        fmtChunk (b, 3, 1, 8000, 32);
        b.tag ("data").u32 ((juce::uint32) sizeof (values) * 100);
        for (int k = 0; k < 100; ++k)
            for (auto v : values)
                b.out.writeFloat (v);
        chop::LoadedSample s;
        const bool ok = chop::loadSample (b.save (dir.getChildFile ("float-nan.wav")), s, error);
        bool finite = ok && s.original != nullptr && s.original->size() == 800;
        for (int i = 0; finite && i < s.original->size(); ++i)
            finite = std::isfinite (s.original->l[(size_t) i]) && std::abs (s.original->l[(size_t) i]) <= 16.0f
                     && std::isfinite (s.original->r[(size_t) i]);
        CHECK (finite, "float WAV with NaN/inf: not loaded as finite audio (%s)", error.toRawUTF8());
        CHECK (finite && s.original->l[0] == 0.0f && s.original->l[1] == 0.0f && s.original->l[3] == 16.0f && s.original->l[5] == 0.5f,
               "float WAV with NaN/inf: values not cleaned as expected");
        CHECK (ok && std::isfinite (s.analysis.bpm) && std::isfinite (s.analysis.bpmConfidence), "float WAV with NaN/inf: analysis not finite");
        // The fuzzer's file: only Core Audio reads its header (macOS), JUCE's WAV reader refuses it.
        CHECK (refusedOrFinite (fixtures.getChildFile ("float-nan.wav")), "the fuzzer's float NaN file loaded non-finite audio");
    }

    // Found by fuzzing (macOS): a damaged FLAC that JUCE's decoder refused was handed on to Core
    // Audio, whose FLAC parser read a comment count from it and asked for 64 GB. A .flac is only ever
    // read by libFLAC now, so neither that file nor anything else named .flac reaches another parser.
    CHECK (refused (fixtures.getChildFile ("flac-coreaudio-comment.flac"), error), "damaged FLAC reached another reader");
    {
        juce::MemoryBlock wavBytes;
        goodWav (dir.getChildFile ("real.wav"), 0.5).loadFileAsData (wavBytes);
        const auto renamed = dir.getChildFile ("wav-named.flac");
        renamed.replaceWithData (wavBytes.getData(), wavBytes.getSize());
        CHECK (refused (renamed, error), "a WAV named .flac was opened by a non-FLAC reader");

        // Core Audio also sniffs content: the same comment-block bug, as FLAC data named .aiff. FLAC
        // data under any name goes to libFLAC (which refuses this one, and reads a real one).
        CHECK (refused (fixtures.getChildFile ("flac-renamed.aiff"), error), "FLAC data named .aiff reached another reader");
        juce::AudioBuffer<float> tone (2, 4410);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (c, i, 0.3f * std::sin ((float) i * 0.05f));
        const auto realFlac = dir.getChildFile ("real.flac");
        {
            juce::FlacAudioFormat flac;
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (realFlac);
            auto w = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
            if (w != nullptr)
                w->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
        }
        const auto flacAsWav = dir.getChildFile ("flac-named.wav");
        realFlac.copyFileTo (flacAsWav);
        chop::LoadedSample s;
        CHECK (chop::loadSample (flacAsWav, s, error) && s.original != nullptr && s.original->size() == tone.getNumSamples(),
               "a real FLAC named .wav didn't load through libFLAC: %s", error.toRawUTF8());
    }

    // Found by fuzzing: a 21-byte MP3 frame using intensity stereo with data up to band 21 made JUCE's
    // MP3 decoder write past its sample buffer (cmake/PatchJuce.cmake fixes it in the fetched JUCE).
    // JUCE's decoder directly, since macOS tries Core Audio first: run it under ASan/UBSan to see the
    // write; here it must at least decode without crashing.
    {
        juce::MP3AudioFormat mp3;
        std::unique_ptr<juce::AudioFormatReader> reader (
            mp3.createReaderFor (new juce::FileInputStream (fixtures.getChildFile ("mp3-intensity-stereo.mp3")), true));
        if (reader != nullptr && reader->lengthInSamples > 0 && reader->lengthInSamples < 1 << 20)
        {
            juce::AudioBuffer<float> buf ((int) juce::jmax (1u, reader->numChannels), (int) reader->lengthInSamples);
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
        }
        juce::String why;
        chop::LoadedSample s;
        chop::loadSample (fixtures.getChildFile ("mp3-intensity-stereo.mp3"), s, why);
        CHECK (true, "survived the intensity-stereo MP3");
    }

    // Found by fuzzing (macOS): a CAF chunk with a negative size sent JUCE's CAF metadata reader
    // backwards forever, hanging the loader (cmake/PatchJuce.cmake fixes it in the fetched JUCE).
    // Other platforms don't read CAF and refuse it at once.
    {
        auto done = std::make_shared<std::atomic<bool>> (false);
        const auto caf = fixtures.getChildFile ("caf-negative-chunk.caf");
        std::thread ([caf, done] {
            chop::LoadedSample s;
            juce::String why;
            chop::loadSample (caf, s, why);
            *done = true;
        }).detach();
        for (int i = 0; i < 400 && ! *done; ++i)
            juce::Thread::sleep (50);
        CHECK (*done, "the negative-chunk CAF file hung the loader");
    }

    // Found by fuzzing: JUCE's WAV and AIFF readers allocated whatever a metadata chunk's header
    // claimed (bext, smpl, inst, cue, INST: up to 4 GB for a 100-byte file) before reading it. Now
    // limited to what the file holds (cmake/PatchJuce.cmake); the fuzz regression run, with its 512 MB
    // allocation limit, checks the allocation itself.
    for (auto* name : { "wav-bext-huge.wav", "aiff-inst-huge.aif" })
        CHECK (refusedOrFinite (fixtures.getChildFile (name)), "%s didn't load cleanly", name);
    {
        Bytes b;
        b.tag ("RIFF").u32 (0x7fffffffu).tag ("WAVE");
        fmtChunk (b, 1, 2, 44100, 16);
        b.tag ("smpl").u32 (0xfffffff0u).zeros (36);
        b.tag ("data").u32 (16).zeros (16);
        CHECK (refusedOrFinite (b.save (dir.getChildFile ("smpl-4gb.wav"))), "a 4 GB smpl chunk didn't load cleanly");
    }

    // Found by fuzzing: a FLAC file whose stream info changes mid-file (153 kHz checked, 545 Hz by the
    // end of the read) loaded at the unchecked rate. A reader that changes its rate or channel count
    // while decoding now means a damaged file.
    {
        chop::LoadedSample s;
        const bool ok = chop::loadSample (fixtures.getChildFile ("flac-rate-change.flac"), s, error);
        CHECK (! ok || (s.fileRate >= 8000.0 && s.fileRate <= 768000.0), "loaded at an unchecked rate: %f", s.fileRate);
    }

    // Found by fuzzing (macOS): the same Core Audio comment parser behind Ogg (an .ogg JUCE's Vorbis
    // reader refused): the comment count in the file sized a 31 GB reservation. Ogg goes to Core Audio
    // only when its comment count fits in the file (real Opus/Vorbis files still load there).
    CHECK (refusedOrFinite (fixtures.getChildFile ("ogg-comment-count.ogg")), "ogg-comment-count.ogg didn't load cleanly");

    // Found by fuzzing: JUCE's AIFF 'cate' chunk parser read three bytes per tag even when fewer were
    // left (a heap over-read; patched by cmake/PatchJuce.cmake, reported by ASan without it).
    CHECK (refusedOrFinite (fixtures.getChildFile ("aiff-cate-tag.aiff")), "aiff-cate-tag.aiff didn't load cleanly");

    // Found by an independent review: 2,000 nested elements in a 16 KB WAV's iXML (or axml) chunk made
    // JUCE's WAV reader recurse through its XML parser until the loader thread's 512 KB stack ran out
    // (SIGBUS in the host). The reader no longer parses those chunks, JUCE's XML parser stops at 64
    // levels for every caller, and the worker thread has an 8 MB stack.
    for (auto* name : { "wav-ixml-deep.wav", "wav-axml-deep.wav" })
    {
        bool ok = false;
        CHECK (finishesOnSmallStack ([&] { ok = refusedOrFinite (fixtures.getChildFile (name)); }), "%s overflowed or hung a 512 KB thread", name);
        CHECK (ok, "%s didn't load cleanly", name);
    }
    {
        bool parsedDeep = true;
        CHECK (finishesOnSmallStack ([&] { parsedDeep = juce::parseXML ("<r>" + repeat ("<a>", 100000) + repeat ("</a>", 100000) + "</r>") != nullptr; }),
               "100,000 nested XML elements overflowed a 512 KB thread");
        CHECK (! parsedDeep, "JUCE parsed 100,000 nested elements (no depth limit)");
        CHECK (juce::parseXML ("<r>" + repeat ("<a>", 60) + repeat ("</a>", 60) + "</r>") != nullptr, "61 levels refused");
    }

    // Longer than 20 minutes at a sane rate (header only; the length check comes before any read).
    CHECK (refused (rf64Wav (dir.getChildFile ("long.wav"), 8000, 8000ULL * 8 * 1300, 64), error), "21-minute file not refused");

    // A real file still loads.
    {
        chop::LoadedSample s;
        CHECK (chop::loadSample (goodWav (dir.getChildFile ("ok.wav"), 2.0), s, error) && s.original != nullptr
                   && s.original->size() == 96000,
               "good wav: %s", error.toRawUTF8());
    }

    // Resampling refuses absurd rates with an exception (callers run inside a catch), not a huge table.
    {
        bool threw = false;
        try { chop::stems::detail::SincResampler rs (4.29e9, 48000.0); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK (threw, "SincResampler accepted a 4.29 GHz rate");
        chop::Stereo st;
        st.resize (16);
        threw = false;
        try { chop::resample (st, 8000.0, std::numeric_limits<double>::infinity()); }
        catch (const std::exception&) { threw = true; }
        CHECK (threw, "resample accepted an infinite rate");
        st.resize (-5);
        CHECK (st.size() == 0, "Stereo::resize(-5) gave %d", st.size());
    }

    // Through the processor's worker: a status message, nothing loaded, and the host still alive.
    {
        FlayrChopProcessor p;
        p.loadFile (fixtures.getChildFile ("riff-bad-rate.wav"));
        CHECK (waitIdle (p), "processor stuck loading the PoC");
        CHECK (! p.hasSample() && p.getStatus().isNotEmpty(), "processor: PoC status '%s'", p.getStatus().toRawUTF8());
        p.loadFile (fixtures.getChildFile ("rf64-bad-rate.wav"));
        CHECK (waitIdle (p), "processor stuck loading the rf64 PoC");
        CHECK (! p.hasSample(), "processor loaded the rf64 PoC");
    }
}

//==============================================================================
// State blobs: the copyXmlToBinary envelope around any text, or around raw bytes with any length.
static juce::MemoryBlock rawBlob (const void* text, size_t bytes, juce::int64 declaredLength = -1)
{
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int) (declaredLength >= 0 ? declaredLength : (juce::int64) bytes));
    out.write (text, bytes);
    out.writeByte (0);
    return out.getMemoryBlock();
}

static juce::MemoryBlock blob (const juce::String& xml)
{
    return rawBlob (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

static bool stateRefused (const juce::MemoryBlock& mb)
{
    return FlayrChopProcessor::parseState (mb.getData(), (int) mb.getSize()) == nullptr;
}

static juce::String repeat (const juce::String& s, int n)
{
    juce::String r;
    r.preallocateBytes ((size_t) (s.getNumBytesAsUTF8() * (size_t) n + 1));
    for (int i = 0; i < n; ++i)
        r << s;
    return r;
}

static juce::String nested (int levels) // <a><a>...</a></a>
{
    return repeat ("<a>", levels) + repeat ("</a>", levels);
}

// Differential check: whatever parseState accepts must be, as JUCE parsed it and as
// ValueTree::fromXml turns it into a tree, within the limits (depth, elements, attributes, no binary
// "base64:" properties). Returns false (and says why) on any disagreement in the unsafe direction.
static bool acceptedOnlyWithinLimits (const juce::MemoryBlock& mb, const char* what)
{
    const auto xml = FlayrChopProcessor::parseState (mb.getData(), (int) mb.getSize());
    if (xml == nullptr)
        return true; // refused: nothing reaches JUCE's tree code
    int elements = 0, attributes = 0, maxDepth = 0;
    std::vector<std::pair<const juce::XmlElement*, int>> stack { { xml.get(), 1 } };
    while (! stack.empty())
    {
        const auto [e, depth] = stack.back();
        stack.pop_back();
        ++elements;
        maxDepth = juce::jmax (maxDepth, depth);
        attributes += e->getNumAttributes();
        for (int i = 0; i < e->getNumAttributes(); ++i)
            if (e->getAttributeName (i).containsChar (':'))
            {
                std::printf ("  %s: accepted an attribute named '%s'\n", what, e->getAttributeName (i).toRawUTF8());
                return false;
            }
        for (const auto* c : e->getChildIterator())
            stack.push_back ({ c, depth + 1 });
    }
    if (elements > FlayrChopProcessor::kMaxStateElements || maxDepth > FlayrChopProcessor::kMaxStateDepth
        || attributes > FlayrChopProcessor::kMaxStateAttributes)
    {
        std::printf ("  %s: accepted %d elements, depth %d, %d attributes\n", what, elements, maxDepth, attributes);
        return false;
    }
    const auto tree = juce::ValueTree::fromXml (*xml);
    std::vector<juce::ValueTree> trees { tree };
    while (! trees.empty())
    {
        const auto t = trees.back();
        trees.pop_back();
        for (int i = 0; i < t.getNumProperties(); ++i)
            if (t.getProperty (t.getPropertyName (i)).isBinaryData())
            {
                std::printf ("  %s: accepted a binary property\n", what);
                return false;
            }
        for (const auto& c : t)
            trees.push_back (c);
    }
    return true;
}

static void stateDifferential()
{
    const juce::String decl ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    juce::StringArray texts;
    // Entities in every spelling, including ones that decode to '<', '&', ':' and NUL.
    for (auto* e : { "&amp;", "&AMP;", "&Amp;", "&lt;", "&LT;", "&#60;", "&#x3C;", "&#X3c;", "&#0000060;", "&#58;", "&#x3a;",
                     "&#0;", "&#x0;", "&#xFFFFFFFF;", "&#99999999999;", "&#x110000;", "&#xD800;", "&unknown;", "&;", "&#;", "&#x;" })
    {
        texts.add ("<FlayrChop x=\"" + juce::String (e) + "\"/>");
        texts.add ("<FlayrChop>" + juce::String (e) + "<Sample/>" + juce::String (e) + "</FlayrChop>");
        texts.add ("<FlayrChop " + juce::String (e) + "=\"1\"/>");
    }
    // Attribute names with ':' and base64 in every position JUCE accepts a name.
    for (auto* a : { " base64:x=\"9.AAAA\"", "\tbase64:x=\"9.AAAA\"", "\nbase64:x=\"9.AAAA\"", "\rbase64:x=\"9.AAAA\"",
                     " x=\"1\"base64:y=\"9.AAAA\"", " x='1'base64:y='9.AAAA'", " BASE64:x=\"9.AAAA\"", " a:b=\"1\"",
                     " :x=\"1\"", " base64=\"1\"", " x=\"base64:9.AAAA\"" })
    {
        texts.add ("<FlayrChop" + juce::String (a) + "/>");
        texts.add ("<FlayrChop><Sample path=\"/x.wav\"" + juce::String (a) + "><Pad index=\"0\"" + juce::String (a) + "/></Sample></FlayrChop>");
    }
    // Whitespace, CR/LF/tab in names and around the declaration; nesting hidden in values.
    for (const auto& t : { decl + "<FlayrChop/>", " " + decl + "<FlayrChop/>", "\n" + decl + "<FlayrChop/>", decl + "\r\n<FlayrChop/>",
                           juce::String ("<Flayr\tChop/>"), juce::String ("<FlayrChop\r\nx=\"1\"\r\n/>"), juce::String ("<FlayrChop x\n=\n\"1\"/>"),
                           "<r x=\"" + repeat ("<a>", 600) + "\"/>", "<r>" + nested (9) + "</r>", "<r>" + nested (7) + "</r>",
                           "<r>" + repeat ("<a>t</a>", 300) + "</r>", "<r>" + nested (7).replace ("<a></a>", "<a>text</a>") + "</r>",
                           juce::String ("<r a=\"1\" a=\"2\" a=\"3\"/>"), juce::String ("<r x=\"1\"y=\"2\"/>"), juce::String ("<r x=1/>") })
        texts.add (t);
    for (const auto& t : texts)
        CHECK (acceptedOnlyWithinLimits (blob (t), t.substring (0, 60).toRawUTF8()), "differential: %s", t.substring (0, 80).toRawUTF8());

    // Raw bytes: BOMs, overlong and invalid UTF-8, NULs inside the declared length, a length shorter
    // or longer than the text, text after the declared length.
    const char* raws[] = { "\xef\xbb\xbf<FlayrChop/>", "\xfe\xff<FlayrChop/>", "<FlayrChop x=\"\xc0\xbc\"/>", "<FlayrChop x=\"\xc0\xba\"/>",
                           "<FlayrChop x=\"\xed\xa0\x80\"/>", "<FlayrChop x=\"\xf4\x90\x80\x80\"/>", "<FlayrChop x=\"\xe2\x80\"/>",
                           "<FlayrChop \xc0\xa0" "base64:x=\"9.AAAA\"/>", "<FlayrChop x=\"1\"\xc2\xa0" "base64:y=\"9.AAAA\"/>" };
    for (auto* r : raws)
    {
        const auto len = std::strlen (r);
        CHECK (acceptedOnlyWithinLimits (rawBlob (r, len), r), "differential (raw): %s", r);
        for (const juce::int64 declared : { (juce::int64) len - 3, (juce::int64) len + 1, (juce::int64) len + 100 })
            if (declared > 0)
                CHECK (acceptedOnlyWithinLimits (rawBlob (r, len, declared), r), "differential (raw, length %d): %s", (int) declared, r);
    }
    {
        juce::MemoryOutputStream withNul;
        withNul << "<FlayrChop/>";
        withNul.writeByte (0);
        withNul << "<x base64:y=\"9.AAAA\"/>";
        CHECK (acceptedOnlyWithinLimits (rawBlob (withNul.getData(), withNul.getDataSize()), "NUL inside"), "differential: NUL inside the length");
        const juce::String past = "<FlayrChop/>" + nested (100);
        CHECK (acceptedOnlyWithinLimits (rawBlob (past.toRawUTF8(), past.getNumBytesAsUTF8(), 12), "past"), "differential: text past the length");
    }
}

static void stateXml()
{
    const juce::String decl ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    const auto ok = decl + "<FlayrChop version=\"1.2.0\"><Sample path=\"/x.wav\"><Pad index=\"0\" cue=\"1\"/></Sample></FlayrChop>";
    CHECK (! stateRefused (blob (ok)), "plain state refused");
    CHECK (! stateRefused (blob ("<FlayrChop/>")), "state without a declaration refused");

    // Entity expansion (a stack overflow in JUCE's XmlDocument) and every other '<!'.
    CHECK (stateRefused (blob (decl + "<!DOCTYPE a [<!ENTITY a \"&b;\"><!ENTITY b \"&a;\">]><FlayrChop x=\"&a;\"/>")), "DOCTYPE entities");
    CHECK (stateRefused (blob ("<FlayrChop><!-- hi --></FlayrChop>")), "comment");
    CHECK (stateRefused (blob ("<FlayrChop><![CDATA[x]]></FlayrChop>")), "CDATA");
    CHECK (stateRefused (blob ("<FlayrChop/><?pi x?>")), "processing instruction after the start");
    CHECK (stateRefused (blob (" " + decl + "<FlayrChop/>")), "declaration not at the very start");

    // Nesting: deep, wide, and hidden from parsers that track quotes.
    CHECK (stateRefused (blob (nested (100000))), "100000 levels");
    CHECK (stateRefused (blob (nested (9))), "9 levels (depth limit)");
    CHECK (! stateRefused (blob (nested (8))), "8 levels refused");
    CHECK (stateRefused (blob ("<r>" + repeat ("<p/>", 600) + "</r>")), "600 elements");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("<a>", 600) + "\"/>")), "'<' inside an attribute value is not skipped");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("/>", 600) + "\">" + nested (600) + "</r>")), "'/>' inside a value");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("</a>", 600) + "\">" + nested (600) + "</r>")), "'</' inside a value");
    CHECK (stateRefused (blob ("<r x='>\" y=\">" + nested (600) + "</r>")), "'>' and mixed quotes inside values");
    CHECK (stateRefused (blob ("<r x=\"" + nested (600))), "unterminated quote");
    CHECK (stateRefused (blob ("<r>" + repeat ("&lt;", 10) + nested (600) + "</r>")), "escaped text then nesting");

    // Bytes JUCE would read differently from a naive scan.
    {
        const auto deep = nested (100000);
        juce::MemoryOutputStream text;
        text << "<FlayrChop/>";
        text.writeByte (0); // getXmlFromBinary's String stops at a NUL
        text << deep;
        CHECK (stateRefused (rawBlob (text.getData(), text.getDataSize())), "deep nesting after a NUL inside the length");
    }
    {
        // Past the declared length: JUCE never reads it, so neither do we (and the state is fine).
        const juce::String head ("<FlayrChop/>");
        const auto all = head + nested (100000);
        const auto mb = rawBlob (all.toRawUTF8(), all.getNumBytesAsUTF8(), head.getNumBytesAsUTF8());
        CHECK (mb.getSize() > (size_t) FlayrChopProcessor::kMaxStateBytes || ! stateRefused (mb), "text past the declared length was read");
        const auto small = head + nested (20);
        const auto mb2 = rawBlob (small.toRawUTF8(), small.getNumBytesAsUTF8(), head.getNumBytesAsUTF8());
        CHECK (! stateRefused (mb2), "only the declared length is parsed");
    }
    {
        // Overlong UTF-8 for '<' (C0 BC): refused as invalid, or counted after decoding.
        juce::MemoryOutputStream text;
        for (int i = 0; i < 600; ++i)
            text << "\xc0\xbc" "a>";
        CHECK (stateRefused (rawBlob (text.getData(), text.getDataSize())), "overlong '<'");
        juce::MemoryOutputStream bad;
        bad << "<FlayrChop x=\"\xff\xfe\"/>";
        CHECK (stateRefused (rawBlob (bad.getData(), bad.getDataSize())), "invalid UTF-8");
    }
    {
        // UTF-16 (with and without a BOM) has NULs; a UTF-8 BOM must not hide a declaration.
        const auto u16 = juce::String ("<FlayrChop/>");
        juce::MemoryOutputStream le, bom;
        bom.writeByte ((char) 0xff);
        bom.writeByte ((char) 0xfe);
        for (auto p = u16.getCharPointer(); ! p.isEmpty(); ++p)
        {
            le.writeShort ((short) *p);
            bom.writeShort ((short) *p);
        }
        CHECK (stateRefused (rawBlob (le.getData(), le.getDataSize())), "UTF-16LE");
        CHECK (stateRefused (rawBlob (bom.getData(), bom.getDataSize())), "UTF-16 with BOM");
        CHECK (stateRefused (blob (juce::String::fromUTF8 ("\xef\xbb\xbf") + decl + nested (600))), "UTF-8 BOM + declaration + nesting");
        FlayrChopProcessor::parseState (blob (juce::String::fromUTF8 ("\xef\xbb\xbf") + "<FlayrChop/>").getData(), 30); // must not crash
    }

    // Found by fuzzing: a hexadecimal character reference cut off by the end of the text made JUCE's
    // XML parser step past the string's terminator and read on through the heap (patched by
    // cmake/PatchJuce.cmake; under ASan this read is reported). Text, attribute, and the fuzzer's state.
    for (const auto& text : { juce::String ("<FlayrChop><Sample path=\"/x.wav\"/>&#x41"), juce::String ("<FlayrChop x=\"&#x4"),
                              juce::String ("<FlayrChop>&#X"), juce::String ("<FlayrChop>&#x41;&#65;&#x") })
        FlayrChopProcessor::parseState (blob (text).getData(), (int) blob (text).getSize());
    {
        juce::MemoryBlock mb;
        fixtures.getChildFile ("state-hex-entity.bin").loadFileAsData (mb);
        FlayrChopProcessor p;
        p.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (true, "survived the cut-off hex entity");
    }

    // Attributes named "base64:..." are decoded by ValueTree::fromXml into a block sized by a number in
    // the attribute (gigabytes for a few bytes): refused before parsing, and JUCE's decoder is patched to
    // refuse sizes the text can't hold (the LV2 wrapper decodes whole states with it).
    CHECK (stateRefused (blob ("<FlayrChop base64:x=\"2000000000.AAAA\"/>")), "base64 attribute");
    CHECK (stateRefused (blob ("<FlayrChop>\n<Sample\tbase64:path=\"9.AAAA\"/></FlayrChop>")), "base64 attribute after a tab");
    CHECK (! stateRefused (blob ("<FlayrChop><Sample path=\"/Music/base64:x.wav\"/></FlayrChop>")), "'base64:' inside a path refused");
    {
        juce::MemoryBlock mb;
        CHECK (! mb.fromBase64Encoding ("2000000000.AAAA") && mb.getSize() == 0, "JUCE's base64 decoder accepted a 2 GB size (%d bytes)",
               (int) mb.getSize());
        for (auto* lie : { "99999999999999999999.AAAA", "-1.AAAA", "+5.AAAA", " 5.AAAA", "2147483648.AAAA", "1e9.AAAA" })
        {
            juce::MemoryBlock b;
            CHECK (! b.fromBase64Encoding (lie) && b.getSize() == 0, "JUCE's base64 decoder accepted '%s' (%d bytes)", lie, (int) b.getSize());
        }
        CHECK (mb.fromBase64Encoding (juce::MemoryBlock ("hello", 5).toBase64Encoding()) && mb.toString() == "hello",
               "JUCE's base64 decoder broke a real block");
    }

    // Floods JUCE's parser handles one item at a time, slowly (entities are appended one by one,
    // attribute names go through a global pool): bounded by count, and the largest accepted state
    // still parses quickly.
    {
        CHECK (stateRefused (blob ("<FlayrChop x=\"" + repeat ("&amp;", 5000) + "\"/>")), "5000 entities");
        juce::String attrs;
        for (int i = 0; i < 5000; ++i)
            attrs << " a" << i << "=\"1\"";
        CHECK (stateRefused (blob ("<FlayrChop" + attrs + "/>")), "5000 attributes");
        juce::String big = "<FlayrChop x=\"" + repeat ("&amp;", FlayrChopProcessor::kMaxStateEntities - 10) + "\"";
        for (int i = 0; i < FlayrChopProcessor::kMaxStateAttributes - 10; ++i)
            big << " b" << i << "=\"" << repeat ("v", 20) << "\"";
        big << "/>";
        const auto mb = blob (big);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const bool parsed = ! stateRefused (mb);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("largest accepted flood: %d bytes, %s in %.0f ms\n", (int) mb.getSize(), parsed ? "parsed" : "refused", ms);
        CHECK (ms < 5000.0, "the largest accepted state took %.0f ms to parse", ms);
    }

    // Envelope.
    {
        auto mb = blob (ok);
        CHECK (FlayrChopProcessor::parseState (mb.getData(), 8) == nullptr, "8-byte blob");
        static_cast<char*> (mb.getData())[0] ^= 1;
        CHECK (stateRefused (mb), "bad magic");
        CHECK (stateRefused (rawBlob (ok.toRawUTF8(), ok.getNumBytesAsUTF8(), 1 << 30)), "declared length beyond the blob");
        CHECK (stateRefused (rawBlob (ok.toRawUTF8(), ok.getNumBytesAsUTF8(), 0)), "zero length");
        const auto big = "<FlayrChop x=\"" + juce::String::repeatedString ("a", FlayrChopProcessor::kMaxStateBytes) + "\"/>";
        CHECK (stateRefused (blob (big)), "oversize state");
    }

    // A real, full project (every pad, every parameter, long paths) fits with lots of headroom.
    {
        const auto longDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (repeat ("long folder name ", 40).trim());
        FlayrChopProcessor p;
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", longDir.getChildFile ("song.wav").getFullPathName(), nullptr);
        s.setProperty ("libraryPath", longDir.getChildFile ("library.wav").getFullPathName(), nullptr);
        for (int i = 0; i < chop::kNumPads; ++i)
        {
            juce::ValueTree t ("Pad");
            for (auto* name : { "index", "cue", "end", "gain", "pan", "keyShift", "stretch", "filter", "favorite", "attack", "release",
                                "mode", "reverse", "choke", "stems" })
                t.setProperty (name, juce::String (i) + ".123456789012", nullptr);
            s.appendChild (t, nullptr);
        }
        root.appendChild (p.apvts.copyState(), nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        std::printf ("full project state: %d bytes (limit %d)\n", (int) mb.getSize(), FlayrChopProcessor::kMaxStateBytes);
        CHECK ((int) mb.getSize() * 4 < FlayrChopProcessor::kMaxStateBytes, "full state %d bytes is too close to the limit", (int) mb.getSize());
        CHECK (! stateRefused (mb), "full project state refused");

        // And a refused blob changes nothing.
        const auto before = p.getStatus();
        const auto evil = blob (decl + "<!DOCTYPE a [<!ENTITY a \"&a;&a;\">]><FlayrChop><Sample path=\"/evil.wav\" x=\"&a;\"/></FlayrChop>");
        p.setStateInformation (evil.getData(), (int) evil.getSize());
        p.setStateInformation (blob (nested (100000)).getData(), (int) blob (nested (100000)).getSize());
        CHECK (! p.isFileMissing() && p.getStatus() == before, "refused state was applied");
    }
}

//==============================================================================
static bool waitLoaded (FlayrChopProcessor& p)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (! (p.hasSample() && p.getBusy() == FlayrChopProcessor::Busy::None))
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > 60000)
            return false;
        pump (0.02);
    }
    return true;
}

static juce::MemoryBlock projectState (FlayrChopProcessor& p, const juce::File& path, const char* paramValue = nullptr,
                                       const juce::String& libraryPath = {})
{
    juce::ValueTree root ("FlayrChop");
    root.setProperty ("version", "1.2.0", nullptr);
    auto params = p.apvts.copyState();
    if (paramValue != nullptr)
        for (auto child : params)
            child.setProperty ("value", paramValue, nullptr);
    root.appendChild (params, nullptr);
    juce::ValueTree s ("Sample");
    s.setProperty ("path", path.getFullPathName(), nullptr);
    if (libraryPath.isNotEmpty())
        s.setProperty ("libraryPath", libraryPath, nullptr);
    s.setProperty ("bpm", 120.0, nullptr);
    juce::ValueTree t ("Pad");
    t.setProperty ("index", 0, nullptr);
    t.setProperty ("cue", 0.25, nullptr);
    t.setProperty ("filter", "nan", nullptr);
    t.setProperty ("gain", "inf", nullptr);
    s.appendChild (t, nullptr);
    root.appendChild (s, nullptr);
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
    return mb;
}

// Plays pad A1 for a while; true if every output sample was finite and within +-4.
static bool playIsFinite (FlayrChopProcessor& p, float& peak)
{
    p.setRateAndBufferSizeDetails (48000.0, 512);
    p.prepareToPlay (48000.0, 512);
    for (int i = 0; i < 400 && p.isRendering(); ++i)
        pump (0.05);
    juce::AudioBuffer<float> buf (2, 512);
    bool finite = true;
    peak = 0.0f;
    for (int block = 0; block < 200; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote, (juce::uint8) 127), 0);
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const float x = buf.getSample (c, i);
                finite = finite && std::isfinite (x) && std::abs (x) <= 4.0f;
                if (std::isfinite (x))
                    peak = juce::jmax (peak, std::abs (x));
            }
    }
    return finite;
}

static void nanState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("nan.wav"), 3.0);
    FlayrChopProcessor p;
    const auto st = projectState (p, wav, "nan");
    p.setStateInformation (st.getData(), (int) st.getSize());
    CHECK (waitLoaded (p), "nan state: never loaded");
    bool allFinite = true;
    for (auto* param : p.getParameters())
        allFinite = allFinite && std::isfinite (param->getValue());
    CHECK (allFinite, "nan state: a parameter stayed non-finite");
    CHECK (std::isfinite (p.getPad (0).gainDb) && std::isfinite (p.getPad (0).filter), "nan state: pad values not finite");
    float peak = 0.0f;
    CHECK (playIsFinite (p, peak), "nan state: non-finite output");
    CHECK (peak > 0.01f, "nan state: pad A1 silent (peak %f)", peak);

    // Hosts that hand over fewer channels than the stereo layout (or an empty block): no write past them.
    for (const int channels : { 0, 1 })
    {
        juce::AudioBuffer<float> odd (channels, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote, (juce::uint8) 127), 0);
        p.processBlock (odd, midi);
        CHECK (true, "survived a %d-channel block", channels);
    }
    {
        juce::AudioBuffer<float> empty (2, 0);
        juce::MidiBuffer midi;
        p.processBlock (empty, midi);
    }

    // Found by an independent review: NaN tempo and key values (a host's tempo, the manual tempo or
    // key shift parameter) slipped through range checks (NaN fails every comparison) into the speed,
    // the key and the stretcher's transpose (undefined behaviour there).
    {
        struct NanHead final : juce::AudioPlayHead
        {
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo pos;
                pos.setIsPlaying (true);
                pos.setBpm (std::numeric_limits<double>::quiet_NaN());
                pos.setPpqPosition (std::numeric_limits<double>::infinity());
                return pos;
            }
        } head;
        p.setPlayHead (&head);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        CHECK (p.getHostBpm() == 0.0, "NaN host tempo stored as %f", p.getHostBpm());
        p.setPlayHead (nullptr);
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        p.apvts.getParameter ("tempoMode")->setValueNotifyingHost (1.0f); // Manual (the last choice)
        p.apvts.getParameter ("tempo")->setValue (nan);
        p.apvts.getParameter ("keyShift")->setValue (nan);
        CHECK (std::isfinite (p.getTargetBpm()) && std::isfinite (p.getSpeed()) && p.getSpeed() >= 0.25 && p.getSpeed() <= 4.0,
               "NaN manual tempo: target %f speed %f", p.getTargetBpm(), p.getSpeed());
        CHECK (p.getTargetKey() >= 0 && p.getTargetKey() < 12, "NaN key shift: key %d", p.getTargetKey());
        p.setSampleBpm (std::numeric_limits<double>::quiet_NaN());
        CHECK (std::isfinite (p.getSampleBpm()), "setSampleBpm took NaN");
        float peak = 0.0f;
        CHECK (playIsFinite (p, peak), "NaN tempo/key: non-finite output");
        p.apvts.getParameter ("tempoMode")->setValueNotifyingHost (0.0f);
        p.apvts.getParameter ("keyShift")->setValueNotifyingHost (p.apvts.getParameter ("keyShift")->getDefaultValue());
    }

    // A host (or automation) pushing NaN straight into a parameter: output stays finite.
    p.apvts.getParameter ("filter")->setValue (std::numeric_limits<float>::quiet_NaN());
    p.apvts.getParameter ("volume")->setValue (std::numeric_limits<float>::infinity());
    CHECK (playIsFinite (p, peak), "NaN parameter: non-finite output");
}

//==============================================================================
static juce::MemoryBlock stateFrom (FlayrChopProcessor& p, bool otherThread)
{
    juce::MemoryBlock mb;
    if (otherThread)
        std::thread ([&] { p.getStateInformation (mb); }).join();
    else
        p.getStateInformation (mb);
    return mb;
}

static juce::ValueTree sampleIn (const juce::MemoryBlock& mb)
{
    const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    return xml != nullptr ? juce::ValueTree::fromXml (*xml).getChildWithName ("Sample") : juce::ValueTree();
}

// A project this version accepted must save into a state it accepts again, or the next reopen
// silently loses it. Restored Sample and parameter trees used to be kept (and saved back) whole, extra
// elements and attributes included, so an accepted project could save back past the limits.
static void stateRoundTrip (const juce::File& dir)
{
    auto check = [] (const juce::String& xml, const char* what) {
        const auto in = blob (xml);
        CHECK (! stateRefused (in), "%s: test state refused", what);
        FlayrChopProcessor p;
        p.setStateInformation (in.getData(), (int) in.getSize());
        waitIdle (p);
        juce::MemoryBlock out;
        p.getStateInformation (out);
        CHECK (! stateRefused (out), "%s: saved state (%d bytes) refused on reopen", what, (int) out.getSize());
        return sampleIn (out);
    };
    const auto gone = dir.getChildFile ("gone.wav").getFullPathName();
    juce::String pads;
    for (int i = 0; i < 480; ++i)
        pads << "<Pad index=\"" << (i % 70) << "\" cue=\"" << (1 + i / 70) << "\" gain=\"-2\" junk=\"x\"/>";
    const auto s = check ("<FlayrChop><Sample path=\"" + gone + "\" bpm=\"99\">" + pads + "<Other/></Sample></FlayrChop>", "480 pad elements");
    // What the project means is kept: the first element for each pad, with the values this version reads.
    CHECK (s.getChildWithProperty ("index", 3).isValid() && (double) s.getChildWithProperty ("index", 3).getProperty ("cue") == 1.0
               && (double) s.getChildWithProperty ("index", 3).getProperty ("gain") == -2.0 && (double) s.getProperty ("bpm") == 99.0,
           "480 pad elements: the project's pads or tempo were not kept");
    CHECK (s.getNumChildren() <= chop::kNumPads, "480 pad elements: %d saved back", s.getNumChildren());

    juce::String junk;
    for (int i = 0; i < 480; ++i)
        junk << "<PARAM id=\"volume\" value=\"" << i << "\"/>";
    check ("<FlayrChop><FlayrChop>" + junk + "</FlayrChop><Sample path=\"" + gone + "\"/></FlayrChop>", "480 parameter elements");
    check ("<FlayrChop><FlayrChop><x/><x/><x/>" + repeat ("<y/>", 480) + "</FlayrChop></FlayrChop>", "unknown parameter elements");
    check ("<FlayrChop><Sample path=\"" + gone + "\" junk=\"" + juce::String::repeatedString ("a", 261000) + "\"/></FlayrChop>",
           "a 261 KB unknown attribute");
    check ("<FlayrChop><FlayrChop><PARAM id=\"volume\" value=\"-3\" junk=\"" + juce::String::repeatedString ("a", 261000)
               + "\"/></FlayrChop></FlayrChop>",
           "a 261 KB attribute on a parameter");
}

// Hosts that save and restore off the message thread.
static void threadedState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("threads.wav"), 3.0);
    {
        FlayrChopProcessor p;
        const auto st = projectState (p, wav);
        std::thread ([&] { p.setStateInformation (st.getData(), (int) st.getSize()); }).join();
        // Not applied yet: a save from any thread returns the restored project unchanged (as PR #5 does).
        CHECK (stateFrom (p, true) == st, "threaded restore: off-thread save before apply differs");
        CHECK (stateFrom (p, false) == st, "threaded restore: message-thread save before apply differs");
        CHECK (waitLoaded (p), "threaded restore: never applied");
        CHECK (p.getPad (0).hasCue() && std::abs (p.getPad (0).cue - 0.25) < 1e-6, "threaded restore: pad not restored");
        pump (0.2);
        const auto a = sampleIn (stateFrom (p, true)), b = sampleIn (stateFrom (p, false));
        CHECK (a.isValid() && a.isEquivalentTo (b), "threaded save differs from a message-thread save");
        CHECK (a.getChildWithProperty ("index", 0).isValid(), "threaded save lost pad A1");

        // A newer message-thread restore wins over an older queued one.
        const auto older = projectState (p, dir.getChildFile ("older.wav"));
        std::thread ([&] { p.setStateInformation (older.getData(), (int) older.getSize()); }).join();
        p.setStateInformation (st.getData(), (int) st.getSize());
        pump (0.3);
        CHECK (! p.getMissingFile().getFileName().contains ("older"), "an older queued restore overrode a newer one");
    }
    {
        // Destroyed with a restore still queued: nothing runs afterwards.
        auto p = std::make_unique<FlayrChopProcessor>();
        const auto st = projectState (*p, wav);
        std::thread ([&] { p->setStateInformation (st.getData(), (int) st.getSize()); }).join();
        p.reset();
        pump (0.2);
        CHECK (true, "survived");
    }
}

//==============================================================================
// Project paths on other machines are never opened (or checked) on restore.
static void networkPaths()
{
    for (auto* path : { "\\\\attacker\\share\\loop.wav", "//attacker/share/loop.wav", "/net/attacker/loop.wav",
                        "/Network/Servers/attacker/loop.wav", "\\\\?\\UNC\\attacker\\share\\loop.wav", "smb://attacker/loop.wav" })
    {
        CHECK (chop::isNetworkPath (path), "not a network path: %s", path);
        FlayrChopProcessor p;
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", path, nullptr);
        s.setProperty ("libraryPath", path, nullptr);
        juce::ValueTree t ("Pad");
        t.setProperty ("index", 3, nullptr);
        t.setProperty ("cue", 1.5, nullptr);
        s.appendChild (t, nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        p.setStateInformation (mb.getData(), (int) mb.getSize());
        // Decided without starting a load: no worker ever saw the path.
        CHECK (p.getBusy() == FlayrChopProcessor::Busy::None && ! p.hasSample(), "%s: a load was started", path);
        CHECK (p.isFileMissing() || ! juce::File::isAbsolutePath (path), "%s: not reported missing", path);
        CHECK (p.getStatus().contains ("Missing"), "%s: status '%s'", path, p.getStatus().toRawUTF8());
        const auto saved = sampleIn (stateFrom (p, false));
        CHECK (saved.getProperty ("path").toString() == path && saved.getChildWithProperty ("index", 3).isValid(),
               "%s: the project didn't save back unchanged", path);
    }
    {
        // A load still running when a project with a network path is restored: its late result must
        // not take that project's pads (or replace its path).
        const auto wav = goodWav (juce::File::createTempFile (".wav"), 3.0);
        FlayrChopProcessor p;
        p.loadFile (wav);
        juce::Thread::sleep (2500); // the loader finishes; its result waits in the inbox (only the message loop drains it)
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", "//attacker/share/loop.wav", nullptr);
        juce::ValueTree t ("Pad");
        t.setProperty ("index", 3, nullptr);
        t.setProperty ("cue", 1.5, nullptr);
        s.appendChild (t, nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        p.setStateInformation (mb.getData(), (int) mb.getSize());
        pump (1.0);
        waitIdle (p);
        const auto saved = sampleIn (stateFrom (p, false));
        CHECK (saved.getProperty ("path").toString() == "//attacker/share/loop.wav", "late load replaced the restored project");
        CHECK (! p.getPad (3).hasCue(), "late load consumed the restored project's pads");
        wav.deleteFile();
    }
    for (auto* path : { "/Users/me/Music/loop.wav", "C:\\Music\\loop.wav", "/Volumes/Drive/loop.wav", "/network-music/loop.wav" })
        CHECK (! chop::isNetworkPath (path), "local path taken for a network one: %s", path);

    // Found by an independent review: juce::File folds '.', '..' and repeated separators, so paths that
    // didn't look remote opened network locations anyway ("C:\..\\\host\share" is \\host\share on
    // Windows, "/tmp/../net/host" is /net/host), and '..' walked a libraryPath out of the library.
    // None of these is ever opened or even checked; the restore reports them missing.
    for (auto* path : { "C:\\..\\\\\\attacker\\share\\a.wav", "C:/..///attacker/share/a.wav", "\\..\\\\\\attacker\\share\\a.wav",
                        "C:\\Users\\x\\Music\\..\\..\\..\\\\\\attacker\\s\\a.wav", "/tmp/../net/host/share/a.wav", "/./net/host/a.wav",
                        "/tmp/../Network/Servers/host/a.wav", "~/..///host/share/a.wav", "/x/..///host/share/a.wav",
                        "/System/Volumes/Data/Network/Servers/h/a.wav", "/Users/me//Music/a.wav", "/Users/me/Music/./a.wav",
                        "/Users/me/Music/Flayr Chop/Samples/../../../.ssh/id_rsa", "/Users/me/Music/.../a.wav", "C:\\Music\\CON.wav",
                        "/x/nul.wav", "C:\\Music\\COM1", "/Users/me/Music/a.wav/.." })
    {
        CHECK (! chop::isSafeProjectPath (path), "unsafe project path accepted: %s", path);
        FlayrChopProcessor p;
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", path, nullptr);
        s.setProperty ("libraryPath", path, nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        p.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (p.getBusy() == FlayrChopProcessor::Busy::None && ! p.hasSample(), "%s: a load was started", path);
    }
    for (auto* path : { "/Users/me/Music/My Song.wav", "/Volumes/Drive/loop.wav", "/x/.hidden.wav", "/x/a..b.wav", "/x/console.wav",
                        "/x/COM10.wav" })
        CHECK (chop::isSafeProjectPath (path) || ! juce::File::isAbsolutePath (path), "safe project path refused: %s", path);
#if ! JUCE_WINDOWS
    {
        // A project pointing at a FIFO named like audio: refused at once instead of blocking the worker.
        const auto fifo = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("flayr-chop-fifo.wav");
        fifo.deleteFile();
        ::mkfifo (fifo.getFullPathName().toRawUTF8(), 0600);
        bool ok = false;
        CHECK (finishesOnSmallStack ([&] { ok = refusedOrFinite (fifo); }, 5000), "opening a FIFO blocked the loader");
        CHECK (ok, "FIFO not refused cleanly");
        fifo.deleteFile();
    }
#endif
}

// The library copy (fix for sandboxed hosts): a project whose original can't be read loads from it.
static juce::File noiseWav (const juce::File& f, double seconds)
{
    f.getParentDirectory().createDirectory();
    juce::Random rng (juce::Time::currentTimeMillis()); // unique audio, so the copy is ours alone
    const double sr = 48000.0;
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            buf.setSample (c, i, (i % 24000 < 300 ? 0.6f : 0.05f) * (rng.nextFloat() * 2.0f - 1.0f));
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
    auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return f;
}

static void libraryCopies (const juce::File& dir)
{
    const auto original = noiseWav (dir.getChildFile ("sandboxed/My Song.wav"), 3.0);
    juce::File copy;
    juce::MemoryBlock saved;
    {
        FlayrChopProcessor p;
        p.loadFile (original);
        CHECK (waitLoaded (p), "library: never loaded");
        copy = p.getLibraryCopy();
        CHECK (copy.existsAsFile() && copy.isAChildOf (chop::sampleLibraryFolder()), "library: no copy (%s)", copy.getFullPathName().toRawUTF8());
        CHECK (copy.getFileName() == juce::SHA256 (original).toHexString() + ".wav", "library: copy named %s", copy.getFileName().toRawUTF8());
        CHECK (copy.getSize() == original.getSize(), "library: copy size");
        CHECK (p.getFileName() == "My Song", "library: shows '%s' instead of the original name", p.getFileName().toRawUTF8());
        chop::Pad pad;
        pad.cue = 1.0;
        p.setPad (9, pad);
        saved = stateFrom (p, false);
        const auto s = sampleIn (saved);
        CHECK (s.getProperty ("path").toString() == original.getFullPathName(), "library: path is not the original");
        CHECK (s.getProperty ("libraryPath").toString() == copy.getFullPathName(), "library: libraryPath not saved");

        // The same audio from somewhere else shares the copy.
        const auto twin = dir.getChildFile ("twin.wav");
        original.copyFileTo (twin);
        CHECK (chop::libraryCopyOf (twin) == copy, "library: same audio, second copy");

#if ! JUCE_WINDOWS
        // Found by an independent review: a link to a link resolved one level, and the clone (which
        // never follows links) then copied the second link: the library "copy" pointed back at the
        // original. Links are resolved all the way and the copy is always a plain file.
        {
            const auto other = noiseWav (dir.getChildFile ("linked/other.wav"), 1.0);
            const auto link1 = dir.getChildFile ("linked/link1.wav"), link2 = dir.getChildFile ("linked/link2.wav");
            other.createSymbolicLink (link1, true);
            link1.createSymbolicLink (link2, true);
            const auto c = chop::libraryCopyOf (link2);
            CHECK (c.existsAsFile() && chop::isPlainFile (c) && c.getSize() == other.getSize(), "library: copy through two links isn't a plain file");
            // A link planted where a copy should be (same name, same size elsewhere) is replaced, not reused.
            const auto decoy = dir.getChildFile ("linked/decoy.wav");
            noiseWav (decoy, 1.0);
            c.deleteFile();
            decoy.createSymbolicLink (c, true);
            const auto c2 = chop::libraryCopyOf (other);
            CHECK (c2 == c && chop::isPlainFile (c2) && juce::SHA256 (c2).toHexString() == juce::SHA256 (other).toHexString(),
                   "library: a planted link was reused as the copy");
            // A copy whose contents changed is replaced too.
            {
                juce::MemoryBlock bytes;
                other.loadFileAsData (bytes);
                static_cast<char*> (bytes.getData())[bytes.getSize() / 2] ^= 0x55; // same size, other audio
                c2.replaceWithData (bytes.getData(), bytes.getSize());
            }
            const auto c3 = chop::libraryCopyOf (other);
            CHECK (c3 == c && juce::SHA256 (c3).toHexString() == juce::SHA256 (other).toHexString(), "library: a changed copy was reused");
            // Cancelled: nothing returned.
            CHECK (chop::libraryCopyOf (other, [] { return true; }) == juce::File(), "library: cancelled copy returned a file");
        }
#endif
    }
    {
        // The original exists but can't be read (what Logic's AU sandbox does to ~/Music/Logic and
        // ~/Downloads): the project loads from the library copy and still saves the original path.
        original.setReadOnly (true);
#if ! JUCE_WINDOWS
        ::chmod (original.getFullPathName().toRawUTF8(), 0);
#endif
        if (juce::FileInputStream (original).openedOk()) // Windows, or running as root: make it unloadable instead
        {
            original.setReadOnly (false);
            original.replaceWithText ("not audio any more");
        }
        FlayrChopProcessor p;
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        CHECK (waitLoaded (p), "unreadable original: library copy not used (status '%s')", p.getStatus().toRawUTF8());
        CHECK (p.getPad (9).hasCue() && std::abs (p.getPad (9).cue - 1.0) < 1e-6, "unreadable original: pads not restored");
        const auto s = sampleIn (stateFrom (p, false));
        CHECK (s.getProperty ("path").toString() == original.getFullPathName(), "unreadable original: path changed");
        CHECK (s.getProperty ("libraryPath").toString() == copy.getFullPathName(), "unreadable original: libraryPath lost");
#if ! JUCE_WINDOWS
        ::chmod (original.getFullPathName().toRawUTF8(), 0644);
#endif
    }
    {
        // A libraryPath outside the library folder is ignored (a project can't point the loader anywhere).
        FlayrChopProcessor p;
        const auto st = projectState (p, dir.getChildFile ("gone.wav"), nullptr, original.getFullPathName());
        p.setStateInformation (st.getData(), (int) st.getSize());
        pump (0.5);
        waitIdle (p);
        CHECK (! p.hasSample() && p.isFileMissing(), "libraryPath outside the library was loaded");
    }
    {
        // Both gone: missing, and the project saves back as it came (relink still possible).
        copy.deleteFile();
        original.deleteFile();
        FlayrChopProcessor p;
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        waitIdle (p);
        CHECK (! p.hasSample() && p.isFileMissing(), "both gone: not missing");
        const auto s = sampleIn (stateFrom (p, false));
        CHECK (s.getChildWithProperty ("index", 9).isValid() && s.getProperty ("libraryPath").toString() == copy.getFullPathName(),
               "both gone: project not saved back unchanged");
    }
    dir.getChildFile ("twin.wav").deleteFile();
}

// The state format older versions (and hand-made Logic projects) carry: no version, no UI, no
// libraryPath, a parameters child and a Sample with only a path and pads.
static void legacyState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("legacy.wav"), 3.0);
    const juce::String xml = "<FlayrChop><FlayrChop><PARAM id=\"volume\" value=\"-6.0\"/><PARAM id=\"keyShift\" value=\"2.0\"/></FlayrChop>"
                             "<Sample path=\"" + wav.getFullPathName() + "\" bpm=\"100.0\">"
                             "<Pad index=\"0\" cue=\"0.5\"/><Pad index=\"12\" cue=\"2.0\" gain=\"-3.0\"/></Sample></FlayrChop>";
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int) xml.getNumBytesAsUTF8());
    out << xml;
    out.writeByte (0);
    FlayrChopProcessor p;
    p.setStateInformation (out.getData(), (int) out.getDataSize());
    CHECK (waitLoaded (p), "legacy state: never loaded");
    CHECK (p.getPad (12).hasCue() && std::abs (p.getPad (12).cue - 2.0) < 1e-6 && std::abs (p.getSampleBpm() - 100.0) < 1e-6,
           "legacy state: project not applied");
    CHECK (std::abs (p.apvts.getRawParameterValue ("volume")->load() + 6.0f) < 1e-3f, "legacy state: parameters not applied");
    if (const auto copy = p.getLibraryCopy(); copy.existsAsFile())
        copy.deleteFile();

    // The auto-chop menu has 8 modes; the restore used to clamp to the first 5.
    for (auto [saved, expected] : { std::pair<int, int> { 7, 7 }, { 5, 5 }, { 99, 7 }, { -3, 0 } })
    {
        const auto ui = blob ("<FlayrChop><UI chopMode=\"" + juce::String (saved) + "\"/></FlayrChop>");
        p.setStateInformation (ui.getData(), (int) ui.getSize());
        CHECK (p.chopModeUI == expected, "chopMode %d restored as %d", saved, p.chopModeUI);
    }
}

int main()
{
    // Unbuffered, so a CI log that ends in a crash still shows the last section reached.
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::setvbuf (stderr, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::createTempFile ("flayr-chop-hardening");
    dir.createDirectory();
    chop::setSampleLibraryFolderForTests (dir.getChildFile ("Music/Flayr Chop/Samples")); // never the real one
    chop::setStemCacheRootForTests (dir.getChildFile ("stems")); // nor the real stem cache

    std::printf ("-- crashFiles\n");
    crashFiles (dir);
    std::printf ("-- stateXml\n");
    stateXml();
    std::printf ("-- stateDifferential\n");
    stateDifferential();
    std::printf ("-- stateRoundTrip\n");
    stateRoundTrip (dir);
    std::printf ("-- nanState\n");
    nanState (dir);
    std::printf ("-- threadedState\n");
    threadedState (dir);
    std::printf ("-- networkPaths\n");
    networkPaths();
    std::printf ("-- libraryCopies\n");
    libraryCopies (dir);
    std::printf ("-- legacyState\n");
    legacyState (dir);

    dir.deleteRecursively();
    std::printf ("hardening_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
