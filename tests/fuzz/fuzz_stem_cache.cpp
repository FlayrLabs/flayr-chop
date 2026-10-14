// Fuzz target: the on-disk stem cache, read back the way a project reopen does (loadCachedStems:
// stems.txt with the names, then N FLAC files that must each decode to exactly the sample's length),
// then resampled with the sample into a playable source (buildSource).
//
// Input: 4 bytes little-endian expected length (masked to 2^20 frames), then sections separated by
// kSeparator: the first is stems.txt, each further one is i.flac (at most 8).

#include "FuzzCommon.h"
#include "SampleLoader.h"

#include <cmath>

namespace
{
const char kSeparator[] = "\n--FLAYR-STEM--\n";
constexpr size_t kSeparatorLen = sizeof (kSeparator) - 1;
juce::File audioFile, cacheDir;

void putU32 (fuzz::Bytes& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back ((uint8_t) (v >> (8 * i)));
}

std::vector<fuzz::Bytes> makeSeeds()
{
    std::vector<fuzz::Bytes> seeds;
    for (const auto& [frames, names] : { std::pair<int, juce::StringArray> { 2000, { "Drums", "Bass", "Other", "Vocals" } },
                                         std::pair<int, juce::StringArray> { 777, { "Drums", "Bass", "Other", "Vocals", "Guitar", "Piano" } },
                                         std::pair<int, juce::StringArray> { 300, { "Vocals" } } })
    {
        std::vector<chop::StereoPtr> stems;
        for (int s = 0; s < names.size(); ++s)
        {
            auto st = std::make_shared<chop::Stereo>();
            st->resize (frames);
            const auto sig = fuzz::testSignal (2, frames, 44100.0);
            for (int i = 0; i < frames; ++i)
            {
                st->l[(size_t) i] = sig.getSample (0, i) / (float) (s + 1);
                st->r[(size_t) i] = sig.getSample (1, i) / (float) (s + 1);
            }
            stems.push_back (st);
        }
        // The real writer, into the work folder's cache, read back as one input.
        chop::saveCachedStems (audioFile, 44100.0, stems, names);
        fuzz::Bytes b;
        putU32 (b, (uint32_t) frames);
        const auto list = fuzz::bytesOf (cacheDir.getChildFile ("stems.txt"));
        b.insert (b.end(), list.begin(), list.end());
        for (int s = 0; s < names.size(); ++s)
        {
            b.insert (b.end(), kSeparator, kSeparator + kSeparatorLen);
            const auto flac = fuzz::bytesOf (cacheDir.getChildFile (juce::String (s) + ".flac"));
            b.insert (b.end(), flac.begin(), flac.end());
        }
        seeds.push_back (b);
    }
    cacheDir.deleteRecursively();
    return seeds;
}

std::vector<fuzz::Bytes> split (const uint8_t* data, size_t size)
{
    std::vector<fuzz::Bytes> parts (1);
    for (size_t i = 0; i < size;)
    {
        if (i + kSeparatorLen <= size && std::memcmp (data + i, kSeparator, kSeparatorLen) == 0)
        {
            parts.emplace_back();
            i += kSeparatorLen;
            continue;
        }
        parts.back().push_back (data[i++]);
    }
    return parts;
}
} // namespace

extern "C" int LLVMFuzzerInitialize (int* argc, char*** argv)
{
    fuzz::initJuce();
    fuzz::initWorkDir ("stem-cache");
    chop::setSampleLibraryFolderForTests (fuzz::workDir().getChildFile ("Music/Flayr Chop/Samples"));
    chop::setStemCacheRootForTests (fuzz::workDir().getChildFile ("stems"));
    audioFile = fuzz::workDir().getChildFile ("song.wav");
    audioFile.replaceWithText ("the cache is keyed on this file's path, size and date");
    cacheDir = chop::stemCacheFolder (audioFile);

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
    if (size < 4)
        return 0;
    const int expected = (int) ((uint32_t) data[0] | (uint32_t) data[1] << 8 | (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24) & 0xfffff;
    const auto parts = split (data + 4, size - 4);

    cacheDir.deleteRecursively();
    cacheDir.createDirectory();
    cacheDir.getChildFile ("stems.txt").replaceWithData (parts[0].data(), parts[0].size());
    for (size_t i = 1; i < parts.size() && i <= 8; ++i)
        cacheDir.getChildFile (juce::String ((int) i - 1) + ".flac").replaceWithData (parts[i].data(), parts[i].size());

    std::vector<chop::StereoPtr> stems;
    juce::StringArray names;
    if (! chop::loadCachedStems (audioFile, expected, stems, names))
    {
        FUZZ_ASSERT (stems.empty() && names.isEmpty(), "a refused cache still returned stems");
        return 0;
    }
    FUZZ_ASSERT (! stems.empty() && (int) stems.size() == names.size() && names.size() <= chop::kMaxStems,
                 "%zu stems, %d names", stems.size(), names.size());
    for (const auto& s : stems)
        FUZZ_ASSERT (s != nullptr && s->size() == expected && s->l.size() == s->r.size(), "stem of %d frames, expected %d",
                     s ? s->size() : -1, expected);

    // With a sample of the same length, into a playable source.
    chop::LoadedSample sample;
    auto original = std::make_shared<chop::Stereo>();
    original->resize (expected);
    sample.original = original;
    sample.fileRate = 44100.0;
    const auto src = chop::buildSource (sample, stems, 48000.0, 1);
    FUZZ_ASSERT (src != nullptr && src->stems.size() == stems.size(), "source has %zu stems", src ? src->stems.size() : 0);
    for (const auto& s : src->stems)
        FUZZ_ASSERT (s->size() == src->full->size(), "stem resampled to %d frames, sample to %d", s->size(), src->full->size());
    return 0;
}
