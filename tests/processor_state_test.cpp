// Processor state round trips: a project restored by the host must save back unchanged while its
// sample is missing or still loading, and a normal loaded save keeps the live model.
#include "PluginProcessor.h"

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

static bool near (double a, double b) { return std::abs (a - b) < 1e-6; }

static void pump (double seconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

// Until the worker and the message loop are done (a restore always goes through the worker, and its
// result arrives on the next timer tick: a fixed short pump is a race on a slow CI machine).
static bool waitIdle (FlayrChopProcessor& p)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    pump (0.05);
    while (p.getBusy() != FlayrChopProcessor::Busy::None)
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > 60000)
            return false;
        pump (0.02);
    }
    pump (0.05);
    return true;
}

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

// A few seconds of clicks, enough for the loader and analysis.
static juce::File writeWav (const juce::File& f, double seconds)
{
    const double sr = 48000.0;
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

// A saved project, as a host would hand it back.
static juce::MemoryBlock projectState (const juce::File& path)
{
    juce::ValueTree root ("FlayrChop");
    root.setProperty ("version", "1.2.0", nullptr);
    juce::ValueTree s ("Sample");
    s.setProperty ("path", path.getFullPathName(), nullptr);
    s.setProperty ("bpm", 93.5, nullptr);
    s.setProperty ("firstBeat", 0.25, nullptr);
    s.setProperty ("key", 7, nullptr);
    s.setProperty ("minor", true, nullptr);
    s.setProperty ("bpmFromUser", true, nullptr);
    s.setProperty ("keyFromUser", true, nullptr);
    const int idx[] = { 0, 5, 17 };
    const double cues[] = { 0.5, 1.75, 3.0 };
    for (int k = 0; k < 3; ++k)
    {
        juce::ValueTree t ("Pad");
        t.setProperty ("index", idx[k], nullptr);
        t.setProperty ("cue", cues[k], nullptr);
        t.setProperty ("end", -1.0, nullptr);
        t.setProperty ("gain", -3.0f, nullptr);
        t.setProperty ("favorite", k == 1, nullptr);
        s.appendChild (t, nullptr);
    }
    root.appendChild (s, nullptr);
    juce::MemoryBlock mb;
    if (auto xml = root.createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, mb);
    return mb;
}

static juce::ValueTree sampleOf (FlayrChopProcessor& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    return xml != nullptr ? juce::ValueTree::fromXml (*xml).getChildWithName ("Sample") : juce::ValueTree();
}

// The saved Sample subtree carries the project's path, tempo, key and pads.
static void expectProject (const juce::ValueTree& s, const juce::File& path, const char* what)
{
    CHECK (s.isValid(), "%s: no Sample in saved state", what);
    CHECK (s.getProperty ("path").toString() == path.getFullPathName(), "%s: path '%s'", what, s.getProperty ("path").toString().toRawUTF8());
    CHECK (near (s.getProperty ("bpm", 0.0), 93.5), "%s: bpm %f", what, (double) s.getProperty ("bpm", 0.0));
    CHECK (near (s.getProperty ("firstBeat", 0.0), 0.25), "%s: firstBeat %f", what, (double) s.getProperty ("firstBeat", 0.0));
    CHECK ((int) s.getProperty ("key", -1) == 7, "%s: key %d", what, (int) s.getProperty ("key", -1));
    CHECK ((bool) s.getProperty ("minor", false), "%s: minor lost", what);
    CHECK ((bool) s.getProperty ("bpmFromUser", false) && (bool) s.getProperty ("keyFromUser", false), "%s: user flags lost", what);
    const auto p0 = s.getChildWithProperty ("index", 0), p5 = s.getChildWithProperty ("index", 5), p17 = s.getChildWithProperty ("index", 17);
    CHECK (p0.isValid() && near (p0.getProperty ("cue"), 0.5), "%s: pad 0 lost", what);
    CHECK (p5.isValid() && near (p5.getProperty ("cue"), 1.75) && (bool) p5.getProperty ("favorite"), "%s: pad 5 lost", what);
    CHECK (p17.isValid() && near (p17.getProperty ("cue"), 3.0), "%s: pad 17 lost", what);
    CHECK (p0.isValid() && near (p0.getProperty ("gain"), -3.0), "%s: pad gain lost", what);
}

static void reopen (FlayrChopProcessor& from, FlayrChopProcessor& to)
{
    juce::MemoryBlock mb;
    from.getStateInformation (mb);
    to.setStateInformation (mb.getData(), (int) mb.getSize());
}

int main()
{
    // Unbuffered, so a CI log that ends in a crash still shows the last section reached.
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::setvbuf (stderr, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::createTempFile ("flayr-chop-state");
    dir.createDirectory();
    chop::setSampleLibraryFolderForTests (dir.getChildFile ("Music/Flayr Chop/Samples")); // never the real one
    chop::setStemCacheRootForTests (dir.getChildFile ("stems")); // nor the real stem cache
    const auto wav = writeWav (dir.getChildFile ("loop.wav"), 5.0);
    const auto relinked = writeWav (dir.getChildFile ("moved.wav"), 5.0);
    const auto missing = dir.getChildFile ("gone/loop.wav");

    {
        // Missing sample: save, reopen that save, save again. Nothing may be lost.
        FlayrChopProcessor a;
        const auto st = projectState (missing);
        a.setStateInformation (st.getData(), (int) st.getSize());
        CHECK (waitIdle (a), "missing: still busy");
        CHECK (a.isFileMissing(), "missing: file should be reported missing");
        expectProject (sampleOf (a), missing, "missing save");
        FlayrChopProcessor b;
        reopen (a, b);
        CHECK (waitIdle (b), "missing reopen: still busy");
        expectProject (sampleOf (b), missing, "missing reopen");
    }
    {
        // Saved before the loader finished (the inbox is only drained by the message loop).
        FlayrChopProcessor a;
        const auto st = projectState (wav);
        a.setStateInformation (st.getData(), (int) st.getSize());
        expectProject (sampleOf (a), wav, "before load");
        FlayrChopProcessor b;
        reopen (a, b);
        CHECK (waitLoaded (b), "before load: reopened copy never loaded");
        expectProject (sampleOf (b), wav, "before load reopen, loaded");
        CHECK (waitLoaded (a), "before load: original never loaded");
        expectProject (sampleOf (a), wav, "after load");
        CHECK (near (a.getPad (5).cue, 1.75) && a.getSampleKey() == 7, "after load: project not applied to the model");
    }
    {
        // Relink a missing sample to a new path: the save carries the new path and the old pads.
        FlayrChopProcessor a;
        const auto st = projectState (missing);
        a.setStateInformation (st.getData(), (int) st.getSize());
        CHECK (waitIdle (a) && a.isFileMissing(), "relink: not reported missing first");
        a.loadFile (relinked);
        expectProject (sampleOf (a), relinked, "relink before load");
        CHECK (waitLoaded (a), "relink: never loaded");
        expectProject (sampleOf (a), relinked, "relink after load");
    }
    {
        // Normal loaded save: the live model, including edits made after loading.
        FlayrChopProcessor a;
        a.loadFile (wav);
        CHECK (waitLoaded (a), "live: never loaded");
        a.setSampleBpm (101.0, true);
        a.setSampleKey (3, false, true);
        chop::Pad p;
        p.cue = 2.5;
        p.gainDb = -6.0f;
        a.setPad (40, p);
        const auto s = sampleOf (a);
        CHECK (s.getProperty ("path").toString() == wav.getFullPathName(), "live: path");
        CHECK (near (s.getProperty ("bpm", 0.0), 101.0), "live: bpm %f", (double) s.getProperty ("bpm", 0.0));
        CHECK ((int) s.getProperty ("key", -1) == 3 && ! (bool) s.getProperty ("minor", true), "live: key");
        const auto t = s.getChildWithProperty ("index", 40);
        CHECK (t.isValid() && near (t.getProperty ("cue"), 2.5) && near (t.getProperty ("gain"), -6.0), "live: pad 40");

        // And a project restored into it, once loaded, saves the live edits made afterwards.
        const auto st = projectState (wav);
        a.setStateInformation (st.getData(), (int) st.getSize());
        CHECK (waitLoaded (a), "live restore: never loaded");
        pump (0.2);
        a.setSampleBpm (110.0, true);
        CHECK (near (sampleOf (a).getProperty ("bpm", 0.0), 110.0), "live restore: edit after load not saved");
    }

    dir.deleteRecursively();
    std::printf ("processor_state_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
