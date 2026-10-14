// Untrusted input through the real processor: crafted audio files, project states and paths must be
// refused cleanly (an error message, never a crash, an abort or a huge allocation).
#include "PluginProcessor.h"
#include "stems/Resampler.h"

#include <cstdio>

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

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::createTempFile ("flayr-chop-hardening");
    dir.createDirectory();

    crashFiles (dir);

    dir.deleteRecursively();
    std::printf ("hardening_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
