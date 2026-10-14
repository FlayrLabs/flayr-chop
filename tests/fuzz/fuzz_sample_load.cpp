// Fuzz target: an audio file through the whole load path, the way a dropped file or a restored
// project reaches it: openChecked (every header check) -> readAll -> loadSample (decode, mono mix,
// analysis, overview) -> buildSource (resampling to the host rate).
//
// Input: byte 0 picks the file extension (and so the reader JUCE tries), the rest is the file.

#include "FuzzCommon.h"
#include "SampleLoader.h"

#include <cmath>

namespace
{
const char* const kExtensions[] = { ".wav", ".aiff", ".flac", ".mp3", ".ogg", ".caf", ".m4a", ".aif" };
constexpr int kNumExtensions = (int) (sizeof (kExtensions) / sizeof (kExtensions[0]));
const double kHostRates[] = { 48000.0, 44100.0, 96000.0, 22050.0 };

// Above this many frames only the header checks run: decoding and analysing millions of frames per
// input would make every such input a timeout. (The 20-minute cap itself is a unit test.)
int maxFramesToDecode = 1 << 18;
bool verbose = false; // FUZZ_VERBOSE=1: say what happened to each input (for replaying artefacts)

fuzz::Bytes withExtension (int ext, const fuzz::Bytes& file)
{
    fuzz::Bytes b;
    b.push_back ((uint8_t) ext);
    b.insert (b.end(), file.begin(), file.end());
    return b;
}

// A RF64 file with real audio (JUCE only writes RF64 past 4 GB).
fuzz::Bytes rf64 (int channels, int frames, double rate)
{
    juce::MemoryOutputStream o;
    const auto dataBytes = (juce::uint64) frames * (juce::uint64) channels * 2;
    o.write ("RF64", 4);
    o.writeInt (-1);
    o.write ("WAVE", 4);
    o.write ("ds64", 4);
    o.writeInt (28);
    o.writeInt64 ((juce::int64) (dataBytes + 72));
    o.writeInt64 ((juce::int64) dataBytes);
    o.writeInt64 (frames);
    o.writeInt (0);
    o.write ("fmt ", 4);
    o.writeInt (16);
    o.writeShort (1);
    o.writeShort ((short) channels);
    o.writeInt ((int) rate);
    o.writeInt ((int) rate * channels * 2);
    o.writeShort ((short) (channels * 2));
    o.writeShort (16);
    o.write ("data", 4);
    o.writeInt (-1);
    const auto sig = fuzz::testSignal (channels, frames, rate);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            o.writeShort ((short) (sig.getSample (c, i) * 30000.0f));
    return fuzz::bytesOf (o.getMemoryBlock());
}

// MPEG-1 Layer III frames (128 kbit/s, 44.1 kHz, no CRC): header plus zeroed side info and data,
// which decodes to silence. Optionally an ID3v2 tag in front and a Xing frame count.
fuzz::Bytes mp3 (int frames, bool id3, bool xing)
{
    fuzz::Bytes b;
    if (id3)
    {
        const uint8_t tag[] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10, 'T', 'I', 'T', '2', 0, 0, 0, 0, 0, 0 };
        b.insert (b.end(), tag, tag + sizeof (tag));
    }
    for (int f = 0; f < frames; ++f)
    {
        const size_t start = b.size();
        const uint8_t header[] = { 0xff, 0xfb, 0x90, 0x00 }; // 417 bytes per frame
        b.insert (b.end(), header, header + 4);
        b.resize (start + 417, 0);
        if (xing && f == 0)
        {
            const uint8_t x[] = { 'X', 'i', 'n', 'g', 0, 0, 0, 1 };
            std::copy (x, x + 8, b.begin() + (long) start + 36);
            const auto n = (uint32_t) frames;
            const uint8_t cnt[] = { (uint8_t) (n >> 24), (uint8_t) (n >> 16), (uint8_t) (n >> 8), (uint8_t) n };
            std::copy (cnt, cnt + 4, b.begin() + (long) start + 44);
        }
    }
    return b;
}

std::vector<fuzz::Bytes> makeSeeds()
{
    std::vector<fuzz::Bytes> seeds;
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    using SF = juce::AudioFormatWriterOptions::SampleFormat;

    const auto stereo = fuzz::testSignal (2, 2205, 22050.0);
    const auto mono = fuzz::testSignal (1, 1600, 8000.0);
    seeds.push_back (withExtension (0, fuzz::encode (wav, stereo, 22050.0, 16)));
    seeds.push_back (withExtension (0, fuzz::encode (wav, mono, 8000.0, 24)));
    seeds.push_back (withExtension (0, fuzz::encode (wav, mono, 8000.0, 8)));
    seeds.push_back (withExtension (0, fuzz::encode (wav, mono, 8000.0, 32, {}, SF::floatingPoint)));
    seeds.push_back (withExtension (0, fuzz::encode (wav, mono, 8000.0, 32, {}, SF::integral)));
    seeds.push_back (withExtension (0, fuzz::encode (wav, stereo, 22050.0, 16,
                                                      { { "bwav description", "fuzz" }, { "NumCuePoints", "1" },
                                                        { "Cue0Identifier", "1" }, { "Cue0Offset", "100" },
                                                        { "acid flags", "1" }, { "acid tempo", "120" },
                                                        { "MidiUnityNote", "60" }, { "NumSampleLoops", "1" },
                                                        { "Loop0Start", "10" }, { "Loop0End", "1000" } })));
    seeds.push_back (withExtension (0, rf64 (2, 800, 16000.0)));
    seeds.push_back (withExtension (1, fuzz::encode (aiff, stereo, 22050.0, 16)));
    seeds.push_back (withExtension (1, fuzz::encode (aiff, mono, 8000.0, 24)));
    seeds.push_back (withExtension (7, fuzz::encode (aiff, mono, 8000.0, 8, { { "MidiUnityNote", "60" }, { "Loop0Start", "1" },
                                                                               { "Loop0End", "100" }, { "NumSampleLoops", "1" } })));
    seeds.push_back (withExtension (2, fuzz::encode (flac, stereo, 22050.0, 16)));
    seeds.push_back (withExtension (2, fuzz::encode (flac, mono, 8000.0, 24)));
    seeds.push_back (withExtension (3, mp3 (6, false, false)));
    seeds.push_back (withExtension (3, mp3 (6, true, true)));
#if JUCE_USE_OGGVORBIS
    {
        juce::OggVorbisAudioFormat ogg;
        seeds.push_back (withExtension (4, fuzz::encode (ogg, stereo, 22050.0, 16)));
    }
#endif
    // Core Audio (macOS) reads WAV/AIFF/CAF by content too.
    seeds.push_back (withExtension (5, fuzz::encode (wav, mono, 8000.0, 16)));
    seeds.push_back (withExtension (6, fuzz::encode (aiff, mono, 8000.0, 16)));

    // The committed proof-of-concept and regression files.
    for (const auto& f : juce::File (FLAYR_CHOP_FIXTURES).findChildFiles (juce::File::findFiles, false, "*.wav"))
        seeds.push_back (withExtension (0, fuzz::bytesOf (f)));
    for (const auto& f : juce::File (FLAYR_CHOP_FIXTURES).findChildFiles (juce::File::findFiles, false, "*.flac"))
        seeds.push_back (withExtension (2, fuzz::bytesOf (f)));
    for (const auto& f : juce::File (FLAYR_CHOP_FIXTURES).findChildFiles (juce::File::findFiles, false, "*.aif*"))
        seeds.push_back (withExtension (1, fuzz::bytesOf (f)));
    return seeds;
}

bool allFinite (const std::vector<float>& v)
{
    for (auto x : v)
        if (! std::isfinite (x))
            return false;
    return true;
}
} // namespace

extern "C" int LLVMFuzzerInitialize (int* argc, char*** argv)
{
    fuzz::initJuce();
    fuzz::initWorkDir ("sample-load");
    chop::setSampleLibraryFolderForTests (fuzz::workDir().getChildFile ("Music/Flayr Chop/Samples"));
    chop::setStemCacheRootForTests (fuzz::workDir().getChildFile ("stems"));
    if (const char* env = std::getenv ("FUZZ_MAX_FRAMES"))
        maxFramesToDecode = std::atoi (env);
    verbose = std::getenv ("FUZZ_VERBOSE") != nullptr;
    juce::File seedDir;
    if (fuzz::seedDirFromArgs (*argc, *argv, seedDir))
    {
        fuzz::writeSeeds (seedDir, makeSeeds());
        std::exit (0);
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    if (size < 1)
        return 0;
    const int ext = data[0] % kNumExtensions;
    const auto file = fuzz::workDir().getChildFile (juce::String ("input") + kExtensions[ext]);
    file.replaceWithData (data + 1, size - 1);

    // Header checks: whatever a reader claims, the count handed on is within the caps.
    int frames = 0;
    juce::String error;
    auto reader = chop::detail::openChecked (file, frames, error);
    if (verbose)
        std::printf ("%s: %s, %d frames at %.0f Hz, %d channels%s%s\n", kExtensions[ext],
                     reader != nullptr ? reader->getFormatName().toRawUTF8() : "refused", frames,
                     reader != nullptr ? reader->sampleRate : 0.0, reader != nullptr ? (int) reader->numChannels : 0,
                     error.isNotEmpty() ? ": " : "", error.toRawUTF8());
    if (reader == nullptr)
    {
        FUZZ_ASSERT (frames == 0 && error.isNotEmpty(), "refused without a message");
        return 0;
    }
    FUZZ_ASSERT (frames > 0 && std::isfinite (reader->sampleRate) && reader->sampleRate >= 8000.0 && reader->sampleRate <= 768000.0,
                 "frames %d rate %f", frames, reader->sampleRate);
    FUZZ_ASSERT ((double) frames <= 20.0 * 60.0 * reader->sampleRate + 1.0, "longer than 20 minutes: %d frames", frames);
    if (frames > maxFramesToDecode)
        return 0;

    {
        auto st = chop::detail::readAll (*reader, frames, {});
        FUZZ_ASSERT (st == nullptr || st->size() == frames, "readAll gave %d frames, not %d", st ? st->size() : -1, frames);
    }
    reader.reset();

    // The whole load, as the processor's worker runs it.
    chop::LoadedSample s;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const bool loaded = chop::loadSample (file, s, error);
    if (verbose)
        std::printf ("loadSample: %s in %.0f ms%s%s\n", loaded ? "loaded" : "failed", juce::Time::getMillisecondCounterHiRes() - t0,
                     error.isNotEmpty() ? ": " : "", error.toRawUTF8());
    if (! loaded)
    {
        FUZZ_ASSERT (s.original == nullptr && error.isNotEmpty(), "failed load left a sample or no message");
        return 0;
    }
    FUZZ_ASSERT (s.original != nullptr && s.original->size() > 0 && s.original->l.size() == s.original->r.size(),
                 "loaded sample has a bad buffer");
    FUZZ_ASSERT (std::isfinite (s.fileRate) && s.fileRate >= 8000.0, "rate %f", s.fileRate);
    // Float files can hold NaN and infinities; none of that may reach the audio (or the analysis).
    FUZZ_ASSERT (allFinite (s.original->l) && allFinite (s.original->r), "decoded audio is not finite");
    const auto& a = s.analysis;
    FUZZ_ASSERT (std::isfinite (a.bpm) && std::isfinite (a.firstBeatSec) && std::isfinite (a.bpmConfidence)
                     && std::isfinite (a.keyConfidence) && a.key >= 0 && a.key < 12,
                 "analysis: bpm %f firstBeat %f key %d", a.bpm, a.firstBeatSec, a.key);
    FUZZ_ASSERT (a.onsetsSec.size() == a.onsetStrength.size(), "onsets %zu strengths %zu", a.onsetsSec.size(), a.onsetStrength.size());
    for (size_t i = 0; i < a.onsetsSec.size(); ++i)
        FUZZ_ASSERT (std::isfinite (a.onsetsSec[i]) && a.onsetsSec[i] >= 0.0 && a.onsetsSec[i] <= s.lengthSec() + 1.0
                         && std::isfinite (a.onsetStrength[i]),
                     "onset %zu at %f (length %f)", i, a.onsetsSec[i], s.lengthSec());
    FUZZ_ASSERT (allFinite (s.overview.mn) && allFinite (s.overview.mx) && allFinite (s.overview.low) && allFinite (s.overview.mid)
                     && allFinite (s.overview.high),
                 "overview not finite");

    // Resampled to a host rate, as buildSource does for playback.
    const double rate = kHostRates[(size > 1 ? data[size - 1] : 0) % 4];
    const auto src = chop::buildSource (s, {}, rate, 1);
    FUZZ_ASSERT (src != nullptr && src->full != nullptr, "no source");
    const auto expected = (double) s.original->size() * rate / s.fileRate;
    FUZZ_ASSERT (std::abs (src->full->size() - expected) <= 1.0, "resampled to %d frames, expected %f", src->full->size(), expected);
    return 0;
}
