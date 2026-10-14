#include "SampleLoader.h"
#include "stems/Resampler.h"

#include <juce_cryptography/juce_cryptography.h>

#if JUCE_MAC
#include <copyfile.h>
#endif

namespace chop
{
namespace
{
juce::AudioFormatManager& formats()
{
    static const auto m = [] {
        auto f = std::make_unique<juce::AudioFormatManager>();
        f->registerBasicFormats();
        return f;
    }();
    return *m;
}

constexpr double kMaxSeconds = 20.0 * 60.0;

// File headers are untrusted: a rate or length from a crafted file must never size a buffer on its own.
constexpr double kMinRate = 8000.0, kMaxRate = 768000.0;
constexpr juce::int64 kMaxFrames = (juce::int64) (kMaxSeconds * 192000.0); // 20 minutes at 192 kHz
static_assert (kMaxFrames <= std::numeric_limits<int>::max(), "frame counts are stored as int");

// Uncompressed formats store every frame, so a length the file can't hold is a lie.
bool isUncompressed (const juce::AudioFormatReader& r)
{
    return r.getFormatName().containsIgnoreCase ("WAV") || r.getFormatName().containsIgnoreCase ("AIFF");
}

juce::AudioFormatWriterOptions writerOptions (double rate)
{
    return juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24);
}
} // namespace

juce::StringArray audioFileWildcards()
{
    juce::StringArray w;
    w.addTokens (formats().getWildcardForAllFormats(), ";", "");
    return w;
}

bool isAudioFile (const juce::File& f)
{
    for (const auto& w : audioFileWildcards())
        if (f.getFileName().matchesWildcard (w, true))
            return true;
    return false;
}

bool isNetworkPath (const juce::String& path)
{
    const auto p = path.trimStart();
    return p.startsWith ("\\\\") || p.startsWith ("//") || p.startsWith ("\\/") || p.startsWith ("/\\")
           || p.startsWithIgnoreCase ("/net/") || p.startsWithIgnoreCase ("/Network/") || p.startsWithIgnoreCase ("smb:")
           || p.startsWithIgnoreCase ("file:");
}

static juce::File& libraryFolderOverride()
{
    static juce::File f;
    return f;
}

void setSampleLibraryFolderForTests (const juce::File& folder)
{
    libraryFolderOverride() = folder;
}

juce::File sampleLibraryFolder()
{
    if (libraryFolderOverride() != juce::File())
        return libraryFolderOverride();
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Flayr Chop").getChildFile ("Samples");
}

bool isInLibrary (const juce::File& f)
{
    return f.isAChildOf (sampleLibraryFolder().getParentDirectory());
}

juce::File libraryCopyOf (const juce::File& original)
{
    const auto src = original.getLinkedTarget(); // copy the audio, never a link back to the original
    if (! src.existsAsFile())
        return {};
    const auto hash = juce::SHA256 (src).toHexString();
    auto ext = src.getFileExtension().toLowerCase();
    if (ext.length() > 8 || ! ext.substring (1).containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789"))
        ext = {};
    const auto folder = sampleLibraryFolder();
    const auto dest = folder.getChildFile (hash + ext);
    if (dest.existsAsFile() && dest.getSize() == src.getSize())
        return dest;
    if (! folder.createDirectory())
        return {};

    // Written under a temporary name and renamed, so a half-written copy is never picked up.
    const auto tmp = folder.getChildFile (hash + "." + juce::Uuid().toString() + ".part");
    bool copied = false;
#if JUCE_MAC
    copied = copyfile (src.getFullPathName().toRawUTF8(), tmp.getFullPathName().toRawUTF8(), nullptr, COPYFILE_CLONE) == 0;
#else
    copied = src.copyFileTo (tmp);
#endif
    if (! copied || tmp.getSize() != src.getSize() || ! tmp.moveFileTo (dest))
    {
        tmp.deleteFile();
        return dest.existsAsFile() && dest.getSize() == src.getSize() ? dest : juce::File();
    }
    return dest;
}

std::vector<float> monoMix (const Stereo& s)
{
    std::vector<float> m ((size_t) s.size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (s.l[i] + s.r[i]);
    return m;
}

bool loadSample (const juce::File& file, LoadedSample& out, juce::String& error, const std::function<bool()>& cancelled)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Can't read " + file.getFileName();
        return false;
    }
    const auto rate = reader->sampleRate;
    const auto length = reader->lengthInSamples;
    if (! std::isfinite (rate) || rate < kMinRate || rate > kMaxRate)
    {
        error = file.getFileName() + " has an unsupported sample rate";
        return false;
    }
    if (length <= 0)
    {
        error = file.getFileName() + " is empty";
        return false;
    }
    if (length > (juce::int64) (kMaxSeconds * rate) || length > kMaxFrames)
    {
        error = file.getFileName() + " is longer than 20 minutes";
        return false;
    }
    if (isUncompressed (*reader))
    {
        const auto bytesPerFrame = (juce::int64) reader->numChannels * (juce::int64) ((reader->bitsPerSample + 7) / 8);
        if (bytesPerFrame <= 0 || length > file.getSize() / bytesPerFrame)
        {
            error = file.getFileName() + " is damaged (its header claims more audio than the file holds)";
            return false;
        }
    }

    auto st = std::make_shared<Stereo>();
    st->resize ((int) length); // length <= kMaxFrames, checked above
    juce::AudioBuffer<float> tmp (2, 1 << 16);
    for (juce::int64 pos = 0; pos < length; pos += tmp.getNumSamples())
    {
        if (cancelled && cancelled())
            return false;
        const int n = (int) std::min<juce::int64> (tmp.getNumSamples(), length - pos);
        reader->read (&tmp, 0, n, pos, true, true);
        const int rightCh = reader->numChannels > 1 ? 1 : 0;
        std::copy (tmp.getReadPointer (0), tmp.getReadPointer (0) + n, st->l.begin() + pos);
        std::copy (tmp.getReadPointer (rightCh), tmp.getReadPointer (rightCh) + n, st->r.begin() + pos);
    }

    out.file = file;
    out.fileRate = rate;
    out.original = st;
    const auto mono = monoMix (*st);
    out.analysis = analysis::analyse (mono.data(), (int) mono.size(), rate);
    out.overview = analysis::overview (mono.data(), (int) mono.size(), rate, 4096);
    return true;
}

StereoPtr resample (const Stereo& in, double fromRate, double toRate)
{
    if (std::abs (fromRate - toRate) < 1e-6)
        return std::make_shared<Stereo> (in);

    if (! std::isfinite (fromRate) || ! std::isfinite (toRate) || fromRate <= 0.0 || toRate <= 0.0)
        throw std::invalid_argument ("resample: unsupported sample rate");
    const auto outLen64 = std::llround (in.size() * toRate / fromRate);
    if (outLen64 < 0 || outLen64 > std::numeric_limits<int>::max())
        throw std::length_error ("resample: result too long");
    const int outLen = (int) outLen64;
    auto out = std::make_shared<Stereo>();
    out->resize (outLen);
    const stems::detail::SincResampler rs (fromRate, toRate);
    parallelFor (2, [&] (int ch) {
        rs.process (ch == 0 ? in.l.data() : in.r.data(), in.size(), ch == 0 ? out->l.data() : out->r.data(), outLen);
    });
    return out;
}

//==============================================================================
juce::File stemCacheFolder (const juce::File& audioFile)
{
    const auto key = audioFile.getFullPathName() + "|" + juce::String (audioFile.getSize()) + "|"
                     + juce::String (audioFile.getLastModificationTime().toMilliseconds());
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
        .getChildFile ("Application Support")
#endif
        .getChildFile ("Flayr Chop")
        .getChildFile ("stems")
        .getChildFile (juce::String::toHexString (key.hashCode64()));
}

bool loadCachedStems (const juce::File& audioFile, int expectedLength, std::vector<StereoPtr>& stems,
                      juce::StringArray& names)
{
    const auto dir = stemCacheFolder (audioFile);
    const auto list = dir.getChildFile ("stems.txt");
    if (! list.existsAsFile())
        return false;
    juce::StringArray n;
    n.addLines (list.loadFileAsString());
    n.removeEmptyStrings();
    if (n.isEmpty() || n.size() > kMaxStems)
        return false;

    std::vector<StereoPtr> loaded;
    for (int i = 0; i < n.size(); ++i)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (dir.getChildFile (juce::String (i) + ".flac")));
        if (reader == nullptr || reader->lengthInSamples != expectedLength)
            return false;
        auto st = std::make_shared<Stereo>();
        st->resize (expectedLength);
        juce::AudioBuffer<float> tmp (2, expectedLength);
        reader->read (&tmp, 0, expectedLength, 0, true, true);
        std::copy (tmp.getReadPointer (0), tmp.getReadPointer (0) + expectedLength, st->l.begin());
        std::copy (tmp.getReadPointer (1), tmp.getReadPointer (1) + expectedLength, st->r.begin());
        loaded.push_back (st);
    }
    stems = std::move (loaded);
    names = n;
    return true;
}

void saveCachedStems (const juce::File& audioFile, double rate, const std::vector<StereoPtr>& stems,
                      const juce::StringArray& names)
{
    const auto dir = stemCacheFolder (audioFile);
    dir.createDirectory();
    juce::FlacAudioFormat flac;
    for (size_t i = 0; i < stems.size(); ++i)
    {
        const auto f = dir.getChildFile (juce::String ((int) i) + ".flac");
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
        auto w = flac.createWriterFor (stream, writerOptions (rate));
        if (w == nullptr)
            return;
        const float* ch[2] = { stems[i]->l.data(), stems[i]->r.data() };
        w->writeFromFloatArrays (ch, 2, stems[i]->size());
    }
    // Written last, so a half-written cache is never picked up.
    dir.getChildFile ("stems.txt").replaceWithText (names.joinIntoString ("\n"));
}

bool writeWav (const juce::File& file, const Stereo& s, double rate, int start, int length)
{
    if (length < 0)
        length = s.size() - start;
    start = juce::jlimit (0, s.size(), start);
    length = juce::jlimit (0, s.size() - start, length);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, writerOptions (rate));
    if (w == nullptr)
        return false;
    const float* ch[2] = { s.l.data() + start, s.r.data() + start };
    return w->writeFromFloatArrays (ch, 2, length);
}
} // namespace chop
