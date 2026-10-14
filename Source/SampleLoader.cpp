#include "SampleLoader.h"
#include "stems/Resampler.h"
#include "stems/Stems.h"

#include <juce_cryptography/juce_cryptography.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <optional>

#if JUCE_MAC
#include <copyfile.h>
#endif
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
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
constexpr unsigned int kMaxChannels = 64;
constexpr juce::int64 kMaxFrames = (juce::int64) (kMaxSeconds * 192000.0); // 20 minutes at 192 kHz
static_assert (kMaxFrames <= std::numeric_limits<int>::max(), "frame counts are stored as int");
constexpr juce::int64 kMaxFramesPerCompressedByte = 4096;
constexpr float kMaxSampleValue = 16.0f;

// True if the file holds a FLAC stream, whatever its name: "fLaC" at the start, or after an ID3v2 tag.
bool hasFlacContent (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return false;
    uint8_t head[10] = {};
    if (in.read (head, 10) != 10)
        return false;
    if (std::memcmp (head, "ID3", 3) == 0)
    {
        // Tag size: 4 x 7 bits, plus the 10-byte header (and a 10-byte footer when flagged).
        const auto size = (juce::int64) ((head[6] & 0x7f) << 21 | (head[7] & 0x7f) << 14 | (head[8] & 0x7f) << 7 | (head[9] & 0x7f));
        if (! in.setPosition (10 + size + ((head[5] & 0x10) != 0 ? 10 : 0)) || in.read (head, 4) != 4)
            return false;
    }
    return std::memcmp (head, "fLaC", 4) == 0;
}

#if JUCE_MAC
// Ogg data reaches Core Audio (it reads .ogg files JUCE's Vorbis reader refuses, and .opus), whose
// comment parser reserves memory for the comment count the file states: fuzzing found a 4.5 KB file
// asking for 31 GB. Only Vorbis or Opus streams whose count fits in the file are handed over.
bool oggCommentsPlausible (const juce::File& file)
{
    juce::MemoryBlock head;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return false;
        in.readIntoMemoryBlock (head, 64 * 1024); // the comment header is in the first pages
    }
    const auto* data = static_cast<const char*> (head.getData());
    const auto size = (juce::int64) head.getSize();
    auto find = [&] (const char* marker, juce::int64 len) -> juce::int64 {
        for (juce::int64 i = 0; i + len <= size; ++i)
            if (std::memcmp (data + i, marker, (size_t) len) == 0)
                return i + len;
        return -1;
    };
    auto pos = find ("\x03vorbis", 7);
    if (pos < 0)
        pos = find ("OpusTags", 8);
    auto le32 = [&] (juce::int64 at) { return (juce::int64) juce::ByteOrder::littleEndianInt (data + at); };
    if (pos < 0 || pos + 4 > size)
        return false;
    pos += 4 + le32 (pos); // vendor string
    if (pos + 4 > size)
        return false;
    // Each comment takes at least its 4-byte length.
    return le32 (pos) <= (file.getSize() - pos - 4) / 4;
}
#endif

// Like AudioFormatManager::createReaderFor (every format that claims the file's extension, in
// registration order), except that FLAC is only ever read by libFLAC: a .flac, or a FLAC stream under
// any other name (which libFLAC then reads instead), is never handed to the operating system's
// decoder. libFLAC covers the format completely, so Core Audio only ever saw FLAC data libFLAC had
// refused, and fuzzing found FLAC comment blocks that made Core Audio's parser ask for 27 to 64 GB
// (named .flac, and renamed .wav). Other formats still fall back: Core Audio reads WAV/AIFF variants
// (IMA ADPCM, u-law) and Ogg codecs that JUCE's readers don't.
std::unique_ptr<juce::AudioFormatReader> createReader (const juce::File& file)
{
    const bool flacContent = hasFlacContent (file);
    const bool flacOnly = flacContent || file.hasFileExtension ("flac");
#if JUCE_MAC
    std::optional<bool> systemDecoderOk; // worked out only if Core Audio's turn comes
#endif
    for (auto* format : formats())
    {
        const bool isFlac = dynamic_cast<juce::FlacAudioFormat*> (format) != nullptr;
        if (flacOnly ? ! isFlac : ! format->canHandleFile (file))
            continue;
#if JUCE_MAC
        if (dynamic_cast<juce::CoreAudioFormat*> (format) != nullptr)
        {
            if (! systemDecoderOk.has_value())
            {
                juce::FileInputStream in (file);
                char magic[4] = {};
                const bool ogg = in.openedOk() && in.read (magic, 4) == 4 && std::memcmp (magic, "OggS", 4) == 0;
                systemDecoderOk = ! ogg || oggCommentsPlausible (file);
            }
            if (! *systemDecoderOk)
                continue;
        }
#endif
        if (auto in = file.createInputStream())
            if (auto* reader = format->createReaderFor (in.release(), true))
                return std::unique_ptr<juce::AudioFormatReader> (reader);
    }
    return nullptr;
}

std::atomic<juce::int64>& loadMemoryBudgetOverride()
{
    static std::atomic<juce::int64> bytes { 0 };
    return bytes;
}

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

namespace
{
bool looksRemote (const juce::String& path)
{
    const auto p = path.trimStart();
    return p.startsWith ("\\\\") || p.startsWith ("//") || p.startsWith ("\\/") || p.startsWith ("/\\")
           || p.startsWithIgnoreCase ("/net/") || p.startsWithIgnoreCase ("/Network/")
           || p.startsWithIgnoreCase ("/System/Volumes/Data/Network/") || p.startsWithIgnoreCase ("/System/Volumes/Data/net/")
           || p.startsWithIgnoreCase ("smb:") || p.startsWithIgnoreCase ("file:");
}
} // namespace

bool isNetworkPath (const juce::String& path)
{
    if (looksRemote (path))
        return true;
    // What juce::File would actually open: it folds '.', '..' and repeated separators, so
    // "C:\..\\\host\share" and "/tmp/../net/host" name network locations too.
    const auto trimmed = path.trim();
    if (! juce::File::isAbsolutePath (trimmed))
        return false;
    const auto full = juce::File (trimmed).getFullPathName();
    if (looksRemote (full))
        return true;
#if JUCE_WINDOWS
    // A drive letter mapped to a network share.
    if (full.length() >= 2 && full[1] == ':')
    {
        const wchar_t root[] = { (wchar_t) full[0], L':', L'\\', 0 };
        if (GetDriveTypeW (root) == DRIVE_REMOTE)
            return true;
    }
#endif
    return false;
}

bool hasUnsafeComponents (const juce::String& path)
{
    juce::StringArray parts;
    parts.addTokens (path.trim().replaceCharacter ('\\', '/'), "/", "");
    for (int i = 0; i < parts.size(); ++i)
    {
        const auto& c = parts[i];
        if (c.isEmpty())
        {
            if (i == 0 || i == parts.size() - 1) // the leading (or a trailing) separator
                continue;
            return true; // repeated separators
        }
        if (c.trimEnd().containsOnly (".")) // ".", "..", and what Windows trims down to them ("...", ". ")
            return true;
        // Windows device names (CON, NUL, COM1.wav, ...) open a device, whatever the folder.
        const auto stem = c.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$"
            || ((stem.startsWith ("COM") || stem.startsWith ("LPT")) && stem.length() == 4 && juce::CharacterFunctions::isDigit (stem[3])))
            return true;
    }
    return false;
}

bool isSafeProjectPath (const juce::String& path)
{
    return path.isNotEmpty() && juce::File::isAbsolutePath (path.trim()) && ! hasUnsafeComponents (path) && ! isNetworkPath (path);
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

bool isPlainFile (const juce::File& f)
{
#if JUCE_WINDOWS
    return f.existsAsFile() && ! f.isSymbolicLink();
#else
    struct stat st {};
    return ::lstat (f.getFullPathName().toRawUTF8(), &st) == 0 && S_ISREG (st.st_mode);
#endif
}

namespace
{
bool sameHash (const juce::File& f, const std::string& hash, const std::function<bool()>& cancelled)
{
    std::string other;
    return stems::sha256File (f.getFullPathName().toStdString(), other, cancelled) && other == hash;
}
} // namespace

juce::File libraryCopyOf (const juce::File& original, const std::function<bool()>& cancelled)
{
    // The real file behind every level of link: the copy holds the audio, never a link back to it.
    std::error_code ec;
    const auto resolved = std::filesystem::canonical (std::filesystem::u8path (original.getFullPathName().toStdString()), ec);
    if (ec)
        return {};
    const juce::File src (juce::String (juce::CharPointer_UTF8 (resolved.u8string().c_str())));
    if (! isPlainFile (src))
        return {};
    std::string hash;
    if (! stems::sha256File (src.getFullPathName().toStdString(), hash, cancelled))
        return {};
    auto ext = src.getFileExtension().toLowerCase();
    if (ext.length() > 8 || ! ext.substring (1).containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789"))
        ext = {};
    const auto folder = sampleLibraryFolder();
    const auto dest = folder.getChildFile (juce::String (hash) + ext);
    // Reused only if it's still a plain file holding exactly this audio (anything else, such as a
    // link or a file changed since, is replaced below).
    auto usable = [&] { return isPlainFile (dest) && dest.getSize() == src.getSize() && sameHash (dest, hash, cancelled); };
    if (dest.exists() && usable())
        return dest;
    if (cancelled && cancelled())
        return {};
    if (! folder.createDirectory())
        return {};

    // Written under a temporary name and renamed, so a half-written copy is never picked up.
    removeStalePartFiles (folder, {});
    const auto tmp = folder.getChildFile (juce::String (hash) + "." + juce::Uuid().toString() + ".part");
    bool copied = false;
#if JUCE_MAC
    // src has no links left in it, so the clone (which never follows one) copies the data.
    copied = copyfile (src.getFullPathName().toRawUTF8(), tmp.getFullPathName().toRawUTF8(), nullptr, COPYFILE_CLONE | COPYFILE_DATA) == 0;
#else
    {
        juce::FileInputStream in (src);
        std::unique_ptr<juce::FileOutputStream> out (tmp.createOutputStream());
        copied = in.openedOk() && out != nullptr && out->openedOk();
        juce::HeapBlock<char> buffer (1 << 20);
        while (copied && ! in.isExhausted())
        {
            if (cancelled && cancelled())
                copied = false;
            else if (const auto n = in.read (buffer, 1 << 20); n < 0 || ! out->write (buffer, (size_t) n))
                copied = false;
        }
        if (out != nullptr)
        {
            out->flush();
            copied = copied && out->getStatus().wasOk();
        }
    }
#endif
    if (! copied || ! isPlainFile (tmp) || tmp.getSize() != src.getSize() || ! tmp.moveFileTo (dest) || ! isPlainFile (dest))
    {
        tmp.deleteFile();
        return dest.exists() && usable() ? dest : juce::File();
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

// Every reader goes through here. Rate, length and format come only from the reader object that will
// then be read from (never from a second look at the header, which could disagree with it), and
// become one checked frame count `frames` that both the allocation and the read loop use.
namespace detail
{
std::unique_ptr<juce::AudioFormatReader> openChecked (const juce::File& file, int& frames, juce::String& error)
{
    frames = 0;
#if ! JUCE_WINDOWS
    // Regular files only: opening a FIFO a project points at would block the worker for good, and a
    // device file is never audio.
    struct stat st {};
    if (::stat (file.getFullPathName().toRawUTF8(), &st) != 0 || ! S_ISREG (st.st_mode))
    {
        error = "Can't read " + file.getFileName();
        return nullptr;
    }
#endif
    auto reader = createReader (file);
    if (reader == nullptr)
    {
        error = "Can't read " + file.getFileName();
        return nullptr;
    }
    const auto rate = reader->sampleRate;
    const auto length = reader->lengthInSamples;
    if (! std::isfinite (rate) || rate < kMinRate || rate > kMaxRate)
    {
        error = file.getFileName() + " has an unsupported sample rate";
        return nullptr;
    }
    if (reader->numChannels < 1 || reader->numChannels > kMaxChannels)
    {
        error = file.getFileName() + " has an unsupported number of channels";
        return nullptr;
    }
    if (length <= 0)
    {
        error = file.getFileName() + " is empty";
        return nullptr;
    }
    if (length > (juce::int64) (kMaxSeconds * rate) || length > kMaxFrames)
    {
        error = file.getFileName() + " is longer than 20 minutes";
        return nullptr;
    }
    // Extra, reject-only: uncompressed audio stores every frame, so a length the file is too small
    // to hold is a lie.
    if (isUncompressed (*reader))
    {
        const auto bytesPerFrame = (juce::int64) reader->numChannels * (juce::int64) ((reader->bitsPerSample + 7) / 8);
        if (bytesPerFrame <= 0 || length > file.getSize() / bytesPerFrame)
        {
            error = file.getFileName() + " is damaged (its header claims more audio than the file holds)";
            return nullptr;
        }
    }
    // Compressed formats can promise any length in a few bytes, and readers pad whatever isn't there
    // with silence: a 43-byte FLAC header claiming 20 minutes at 192 kHz cost 3 GB and minutes of CPU
    // to decode, analyse and resample. Real recordings are around 1 to 10 frames per byte; even digital
    // silence from a standard FLAC encoder (4096-frame blocks) is about 300. Only files of almost
    // nothing but silence in oversized blocks go past this.
    else if (length > file.getSize() * kMaxFramesPerCompressedByte)
    {
        error = file.getFileName() + " is damaged (its header claims more audio than the file holds)";
        return nullptr;
    }
    // And whatever the format, a load must fit in this machine's memory: the decoded buffers, the
    // analysis and a resampled copy come to about 24 bytes per frame plus 8 per frame at the host rate
    // (counted at up to 192 kHz). Refused up front, before any of it is allocated, if that's more than
    // half the RAM (a 20-minute 192 kHz file needs about 6 GB).
    {
        const auto bytes = (double) length * 24.0 + (double) length * juce::jmax (1.0, 192000.0 / rate) * 8.0;
        const auto budget = loadMemoryBudgetOverride() > 0 ? (double) loadMemoryBudgetOverride()
                                                           : (double) juce::SystemStats::getMemorySizeInMegabytes() * 1024.0 * 1024.0 * 0.5;
        if (bytes > budget)
        {
            error = file.getFileName() + " is too long to load in this computer's memory";
            return nullptr;
        }
    }
    frames = (int) length; // <= kMaxFrames <= INT_MAX
    return reader;
}

// Reads exactly `frames` frames (the count openChecked returned) into a new buffer.
std::shared_ptr<Stereo> readAll (juce::AudioFormatReader& reader, int frames, const std::function<bool()>& cancelled)
{
    auto st = std::make_shared<Stereo>();
    st->resize (frames);
    const int rightCh = reader.numChannels > 1 ? 1 : 0;
    // What openChecked validated. A reader can change these while it decodes (found by fuzzing: a
    // FLAC file whose stream info changed the rate from 153 kHz to 545 Hz mid-file), and the caller
    // goes on using the reader's values, so a file that changes them is damaged.
    const auto rate = reader.sampleRate;
    const auto channels = reader.numChannels;
    juce::AudioBuffer<float> tmp (2, 1 << 16);
    for (int pos = 0; pos < frames;)
    {
        if (cancelled && cancelled())
            return nullptr;
        const int n = std::min (tmp.getNumSamples(), frames - pos);
        tmp.clear();
        if (! reader.read (&tmp, 0, n, pos, true, true))
            return nullptr; // short or failed read
        if (! juce::exactlyEqual (reader.sampleRate, rate) || reader.numChannels != channels)
            return nullptr;
        // Float files can hold NaN, infinities or absurd values, and none of that may reach the
        // analysis or the host: anything non-finite becomes silence, the rest is kept within +-16
        // (+24 dBFS, far above any real recording).
        const auto clean = [] (float x) { return std::isfinite (x) ? juce::jlimit (-kMaxSampleValue, kMaxSampleValue, x) : 0.0f; };
        std::transform (tmp.getReadPointer (0), tmp.getReadPointer (0) + n, st->l.begin() + pos, clean);
        std::transform (tmp.getReadPointer (rightCh), tmp.getReadPointer (rightCh) + n, st->r.begin() + pos, clean);
        pos += n;
    }
    return st;
}

} // namespace detail
using detail::openChecked;
using detail::readAll;

bool loadSample (const juce::File& file, LoadedSample& out, juce::String& error, const std::function<bool()>& cancelled)
{
    int frames = 0;
    auto reader = openChecked (file, frames, error);
    if (reader == nullptr)
        return false;
    auto st = readAll (*reader, frames, cancelled);
    if (st == nullptr)
    {
        if (! (cancelled && cancelled()))
            error = "Can't read " + file.getFileName() + " (the file is damaged or cut short)";
        return false;
    }

    out.file = file;
    out.fileRate = reader->sampleRate;
    out.original = st;
    const auto mono = monoMix (*st);
    out.analysis = analysis::analyse (mono.data(), (int) mono.size(), out.fileRate);
    out.overview = analysis::overview (mono.data(), (int) mono.size(), out.fileRate, 4096);
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

//==============================================================================
static juce::File& stemCacheRootOverride()
{
    static juce::File f;
    return f;
}

void setStemCacheRootForTests (const juce::File& folder)
{
    stemCacheRootOverride() = folder;
}

void setLoadMemoryBudgetForTests (juce::int64 bytes)
{
    loadMemoryBudgetOverride() = bytes;
}

juce::File stemCacheFolder (const juce::File& audioFile)
{
    const auto key = audioFile.getFullPathName() + "|" + juce::String (audioFile.getSize()) + "|"
                     + juce::String (audioFile.getLastModificationTime().toMilliseconds());
    const auto root = stemCacheRootOverride() != juce::File() ? stemCacheRootOverride()
                                                              : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                                                                    .getChildFile ("Application Support")
#endif
                                                                    .getChildFile ("Flayr Chop")
                                                                    .getChildFile ("stems");
    return root.getChildFile (juce::String::toHexString (key.hashCode64()));
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
        // Cache files get the same checks as any other audio file.
        int frames = 0;
        juce::String error;
        auto reader = openChecked (dir.getChildFile (juce::String (i) + ".flac"), frames, error);
        if (reader == nullptr || frames != expectedLength)
            return false;
        auto st = readAll (*reader, frames, {});
        if (st == nullptr)
            return false;
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
    // The list goes first and comes back last: a cache interrupted anywhere in between has no list,
    // so it's never picked up. Each stem is written under a temporary name and renamed when complete.
    dir.getChildFile ("stems.txt").deleteFile();
    juce::FlacAudioFormat flac;
    for (size_t i = 0; i < stems.size(); ++i)
    {
        const auto f = dir.getChildFile (juce::String ((int) i) + ".flac");
        const auto tmp = dir.getChildFile (juce::String ((int) i) + "." + juce::Uuid().toString() + ".part");
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tmp);
            auto w = flac.createWriterFor (stream, writerOptions (rate));
            if (w == nullptr || ! w->writeFromFloatArrays (std::array<const float*, 2> { stems[i]->l.data(), stems[i]->r.data() }.data(), 2,
                                                          stems[i]->size()))
            {
                w.reset();
                tmp.deleteFile();
                return;
            }
        } // the writer finishes the file here
        if (! tmp.moveFileTo (f))
        {
            tmp.deleteFile();
            return;
        }
    }
    dir.getChildFile ("stems.txt").replaceWithText (names.joinIntoString ("\n")); // via a temp file and a rename
}

void removeStalePartFiles (const juce::File& folder, const juce::String& prefix)
{
    // Unique names, so instances never share one: what's left from a crash or a killed host is
    // removed once it's a day old (a download in progress elsewhere is younger than that).
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days (1);
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, prefix + "*.part"))
        if (f.getLastModificationTime() < cutoff && isPlainFile (f))
            f.deleteFile();
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
    // Written through a cleaned copy, a block at a time: a non-finite sample would reach the writer's
    // float-to-integer conversion (undefined behaviour) and make a broken file.
    constexpr int block = 1 << 16;
    std::vector<float> l ((size_t) block), r ((size_t) block);
    const auto clean = [] (float x) { return std::isfinite (x) ? juce::jlimit (-1.0f, 1.0f, x) : 0.0f; };
    for (int pos = 0; pos < length;)
    {
        const int n = std::min (block, length - pos);
        std::transform (s.l.begin() + start + pos, s.l.begin() + start + pos + n, l.begin(), clean);
        std::transform (s.r.begin() + start + pos, s.r.begin() + start + pos + n, r.begin(), clean);
        const float* ch[2] = { l.data(), r.data() };
        if (! w->writeFromFloatArrays (ch, 2, n))
            return false;
        pos += n;
    }
    return true;
}
} // namespace chop
