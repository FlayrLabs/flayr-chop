#pragma once

// Decoding, analysis, resampling and the on-disk stem cache. Everything here is slow and runs on
// the processor's background worker, never on the audio thread.

#include <juce_audio_formats/juce_audio_formats.h>

#include "Analysis.h"
#include "Renderer.h"

namespace chop
{
struct LoadedSample
{
    juce::File file;      // the file actually read (the original, or its library copy)
    juce::File cacheFile; // what the stem cache is keyed on: the library copy when there is one
    double fileRate = 44100.0;
    std::shared_ptr<const Stereo> original; // at the file's own rate
    analysis::Result analysis;
    analysis::Overview overview;
    double lengthSec() const { return original ? original->size() / fileRate : 0.0; }
};

juce::StringArray audioFileWildcards(); // "*.wav;*.aif;..." for choosers and drag and drop
bool isAudioFile (const juce::File&);

// Reads and analyses a file. Long files are fine; anything over 20 minutes is refused.
bool loadSample (const juce::File&, LoadedSample&, juce::String& error, const std::function<bool()>& cancelled = {});

// Paths on another machine (\\host\share, //host/share, /net/..., /Network/...). Touching one can
// block, trigger an automount, or (on Windows) send the user's NTLM hash to whoever named it, so
// paths restored from a project are never opened or even checked when they look like this.
bool isNetworkPath (const juce::String& path);

// '.' or '..' components, repeated separators, or a Windows device name (CON, NUL, COM1...): never in a
// path this plug-in saved, and each lets a project disguise where it points (juce::File folds them).
bool hasUnsafeComponents (const juce::String& path);

// A path from a project (or any other untrusted text) that may be checked or opened: absolute, no
// unsafe components, and not on another machine.
bool isSafeProjectPath (const juce::String& path);

// Music/Flayr Chop/Samples. Sandboxed hosts (Logic runs AUs out of process) can read here on every
// launch, unlike most places a sample is dropped from, so loaded samples get a copy here.
juce::File sampleLibraryFolder();
bool isInLibrary (const juce::File&); // anywhere under Music/Flayr Chop
void setSampleLibraryFolderForTests (const juce::File&); // tests only, before any processor exists

// The library copy of a file: named by its SHA-256 plus its extension, so the same audio is stored
// once. Links are resolved all the way and the copy is always a plain file with the data (cloned
// where the file system can, so no extra disk space). An existing copy is reused only if it's still a
// plain file with the same contents. Returns an empty File on failure or when cancelled() returns
// true (hashing and copying check it every megabyte). Slow: worker thread only.
juce::File libraryCopyOf (const juce::File&, const std::function<bool()>& cancelled = {});
bool isPlainFile (const juce::File&); // a regular file, not a link (lstat)

namespace detail
{
// loadSample's first two steps, exposed for tests and fuzzing. openChecked opens a file and applies
// every header check, returning the one checked frame count that readAll then allocates and reads.
std::unique_ptr<juce::AudioFormatReader> openChecked (const juce::File&, int& frames, juce::String& error);
std::shared_ptr<Stereo> readAll (juce::AudioFormatReader&, int frames, const std::function<bool()>& cancelled);
} // namespace detail

// High-quality sample-rate conversion (windowed sinc).
StereoPtr resample (const Stereo& in, double fromRate, double toRate);

std::vector<float> monoMix (const Stereo&);

// The playable source: the sample and its stems, all resampled to `rate`.
SourcePtr buildSource (const LoadedSample&, const std::vector<StereoPtr>& stems, double rate, uint64_t id);

// Stems are cached per file (path + size + date) so a project reopens without separating again.
juce::File stemCacheFolder (const juce::File& audioFile);
void setStemCacheRootForTests (const juce::File&); // tests only: keeps the cache out of the user's folders
void setLoadMemoryBudgetForTests (juce::int64 bytes); // tests only: 0 = half the RAM (the default)
bool loadCachedStems (const juce::File& audioFile, int expectedLength, std::vector<StereoPtr>& stems,
                      juce::StringArray& names);
void saveCachedStems (const juce::File& audioFile, double rate, const std::vector<StereoPtr>& stems,
                      const juce::StringArray& names);

// Deletes prefix*.part files in folder that are more than a day old (temporary files of copies and
// downloads that were interrupted).
void removeStalePartFiles (const juce::File& folder, const juce::String& prefix);

// Writes a stereo buffer as 24-bit WAV.
bool writeWav (const juce::File&, const Stereo&, double rate, int start = 0, int length = -1);
} // namespace chop
