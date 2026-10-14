#pragma once
// Shared setup for the fuzz targets (tests/fuzz). Each target is a libFuzzer-style harness: it runs
// under libFuzzer where the compiler has it, or under FuzzDriver.cpp (Apple clang) otherwise.
//
//   fuzz_x -write_seeds=dir      writes the target's generated seed inputs into dir and exits
//
// Everything a target writes goes under one temporary folder (never ~/Music or the user's
// Application Support), removed at exit.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace fuzz
{
using Bytes = std::vector<uint8_t>;

// Stops the run the way a sanitizer would, so the engine saves the input: for broken invariants
// (state that can't be reloaded, non-finite analysis results, buffers of the wrong size).
#define FUZZ_ASSERT(cond, ...)                                                    \
    do {                                                                          \
        if (! (cond)) {                                                           \
            std::fprintf (stderr, "FUZZ_ASSERT failed %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            std::fprintf (stderr, __VA_ARGS__);                                   \
            std::fprintf (stderr, "\n");                                          \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

// JUCE for the whole run. Deliberately never destroyed: JUCE creates some of its statics lazily
// (after this), so a static initialiser would be torn down after them at exit and double-free.
inline void initJuce()
{
    static auto* gui = new juce::ScopedJuceInitialiser_GUI();
    (void) gui;
}

inline juce::File& workDir()
{
    static juce::File d;
    return d;
}

// A fixed folder name per target, so paths baked into seeds (project states) stay valid between runs.
inline void initWorkDir (const char* target)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::setvbuf (stderr, nullptr, _IONBF, 0);
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (juce::String ("flayr-chop-fuzz-") + target);
    d.deleteRecursively();
    d.createDirectory();
    workDir() = d;
    std::atexit ([] { workDir().deleteRecursively(); });
}

// -write_seeds=dir: returns the directory (and true) when the flag is present.
inline bool seedDirFromArgs (int argc, char** argv, juce::File& dir)
{
    for (int i = 1; i < argc; ++i)
        if (std::strncmp (argv[i], "-write_seeds=", 13) == 0)
        {
            dir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i] + 13);
            return true;
        }
    return false;
}

inline void writeSeeds (const juce::File& dir, const std::vector<Bytes>& seeds)
{
    dir.createDirectory();
    int n = 0;
    for (const auto& s : seeds)
    {
        dir.getChildFile (juce::String::formatted ("seed-%03d", n++)).replaceWithData (s.data(), s.size());
    }
    std::printf ("wrote %d seeds to %s\n", n, dir.getFullPathName().toRawUTF8());
}

inline Bytes bytesOf (const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    const auto* p = static_cast<const uint8_t*> (mb.getData());
    return Bytes (p, p + mb.getSize());
}

inline Bytes bytesOf (const juce::MemoryBlock& mb)
{
    const auto* p = static_cast<const uint8_t*> (mb.getData());
    return Bytes (p, p + mb.getSize());
}

// A short test signal: clicks every 0.1 s over a quiet tone, so analysis has something to find.
inline juce::AudioBuffer<float> testSignal (int channels, int frames, double rate)
{
    juce::AudioBuffer<float> buf (channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
        {
            const auto click = (i % (int) (rate * 0.1)) < 64 ? 0.7f : 0.0f;
            buf.setSample (c, i, click + 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate + c));
        }
    return buf;
}

// Encodes a buffer with one of JUCE's writers (through a file in the work folder: the writer owns
// and finalises its stream).
inline Bytes encode (juce::AudioFormat& format, const juce::AudioBuffer<float>& buf, double rate, int bits,
                     const std::vector<std::pair<juce::String, juce::String>>& metadata = {},
                     juce::AudioFormatWriterOptions::SampleFormat sampleFormat = juce::AudioFormatWriterOptions::SampleFormat::automatic)
{
    const auto f = workDir().getChildFile ("encode.tmp");
    f.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
        auto o = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (buf.getNumChannels())
                     .withBitsPerSample (bits).withSampleFormat (sampleFormat);
        for (const auto& [k, v] : metadata)
            o = o.withMetadata (k, v);
        auto w = format.createWriterFor (stream, o);
        if (w == nullptr)
            return {};
        w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
    auto out = bytesOf (f);
    f.deleteFile();
    return out;
}
} // namespace fuzz
