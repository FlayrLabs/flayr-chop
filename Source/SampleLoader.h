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
    juce::File file;
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
