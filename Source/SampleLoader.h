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

// Music/Flayr Chop/Samples. Sandboxed hosts (Logic runs AUs out of process) can read here on every
// launch, unlike most places a sample is dropped from, so loaded samples get a copy here.
juce::File sampleLibraryFolder();
bool isInLibrary (const juce::File&); // anywhere under Music/Flayr Chop
void setSampleLibraryFolderForTests (const juce::File&); // tests only, before any processor exists

// The library copy of a file: named by its SHA-256 plus its extension, so the same audio is stored
// once. Cloned where the file system can (no extra disk space), copied otherwise. Returns the
// existing copy if there is one, or an empty File on failure. Slow: worker thread only.
juce::File libraryCopyOf (const juce::File&);

// High-quality sample-rate conversion (windowed sinc).
StereoPtr resample (const Stereo& in, double fromRate, double toRate);

std::vector<float> monoMix (const Stereo&);

// Stems are cached per file (path + size + date) so a project reopens without separating again.
juce::File stemCacheFolder (const juce::File& audioFile);
bool loadCachedStems (const juce::File& audioFile, int expectedLength, std::vector<StereoPtr>& stems,
                      juce::StringArray& names);
void saveCachedStems (const juce::File& audioFile, double rate, const std::vector<StereoPtr>& stems,
                      const juce::StringArray& names);

// Writes a stereo buffer as 24-bit WAV.
bool writeWav (const juce::File&, const Stereo&, double rate, int start = 0, int length = -1);
} // namespace chop
