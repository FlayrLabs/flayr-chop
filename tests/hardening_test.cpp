// Untrusted input through the real processor: crafted audio files, project states and paths must be
// refused cleanly (an error message, never a crash, an abort or a huge allocation).
#include "PluginProcessor.h"
#include "stems/Resampler.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstdio>
#include <thread>
#include <sys/stat.h>

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

//==============================================================================
// State blobs: the copyXmlToBinary envelope around any text, or around raw bytes with any length.
static juce::MemoryBlock rawBlob (const void* text, size_t bytes, juce::int64 declaredLength = -1)
{
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int) (declaredLength >= 0 ? declaredLength : (juce::int64) bytes));
    out.write (text, bytes);
    out.writeByte (0);
    return out.getMemoryBlock();
}

static juce::MemoryBlock blob (const juce::String& xml)
{
    return rawBlob (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

static bool stateRefused (const juce::MemoryBlock& mb)
{
    return FlayrChopProcessor::parseState (mb.getData(), (int) mb.getSize()) == nullptr;
}

static juce::String repeat (const juce::String& s, int n)
{
    juce::String r;
    r.preallocateBytes ((size_t) (s.getNumBytesAsUTF8() * (size_t) n + 1));
    for (int i = 0; i < n; ++i)
        r << s;
    return r;
}

static juce::String nested (int levels) // <a><a>...</a></a>
{
    return repeat ("<a>", levels) + repeat ("</a>", levels);
}

static void stateXml()
{
    const juce::String decl ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    const auto ok = decl + "<FlayrChop version=\"1.2.0\"><Sample path=\"/x.wav\"><Pad index=\"0\" cue=\"1\"/></Sample></FlayrChop>";
    CHECK (! stateRefused (blob (ok)), "plain state refused");
    CHECK (! stateRefused (blob ("<FlayrChop/>")), "state without a declaration refused");

    // Entity expansion (a stack overflow in JUCE's XmlDocument) and every other '<!'.
    CHECK (stateRefused (blob (decl + "<!DOCTYPE a [<!ENTITY a \"&b;\"><!ENTITY b \"&a;\">]><FlayrChop x=\"&a;\"/>")), "DOCTYPE entities");
    CHECK (stateRefused (blob ("<FlayrChop><!-- hi --></FlayrChop>")), "comment");
    CHECK (stateRefused (blob ("<FlayrChop><![CDATA[x]]></FlayrChop>")), "CDATA");
    CHECK (stateRefused (blob ("<FlayrChop/><?pi x?>")), "processing instruction after the start");
    CHECK (stateRefused (blob (" " + decl + "<FlayrChop/>")), "declaration not at the very start");

    // Nesting: deep, wide, and hidden from parsers that track quotes.
    CHECK (stateRefused (blob (nested (100000))), "100000 levels");
    CHECK (stateRefused (blob (nested (9))), "9 levels (depth limit)");
    CHECK (! stateRefused (blob (nested (8))), "8 levels refused");
    CHECK (stateRefused (blob ("<r>" + repeat ("<p/>", 600) + "</r>")), "600 elements");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("<a>", 600) + "\"/>")), "'<' inside an attribute value is not skipped");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("/>", 600) + "\">" + nested (600) + "</r>")), "'/>' inside a value");
    CHECK (stateRefused (blob ("<r x=\"" + repeat ("</a>", 600) + "\">" + nested (600) + "</r>")), "'</' inside a value");
    CHECK (stateRefused (blob ("<r x='>\" y=\">" + nested (600) + "</r>")), "'>' and mixed quotes inside values");
    CHECK (stateRefused (blob ("<r x=\"" + nested (600))), "unterminated quote");
    CHECK (stateRefused (blob ("<r>" + repeat ("&lt;", 10) + nested (600) + "</r>")), "escaped text then nesting");

    // Bytes JUCE would read differently from a naive scan.
    {
        const auto deep = nested (100000);
        juce::MemoryOutputStream text;
        text << "<FlayrChop/>";
        text.writeByte (0); // getXmlFromBinary's String stops at a NUL
        text << deep;
        CHECK (stateRefused (rawBlob (text.getData(), text.getDataSize())), "deep nesting after a NUL inside the length");
    }
    {
        // Past the declared length: JUCE never reads it, so neither do we (and the state is fine).
        const juce::String head ("<FlayrChop/>");
        const auto all = head + nested (100000);
        const auto mb = rawBlob (all.toRawUTF8(), all.getNumBytesAsUTF8(), head.getNumBytesAsUTF8());
        CHECK (mb.getSize() > (size_t) FlayrChopProcessor::kMaxStateBytes || ! stateRefused (mb), "text past the declared length was read");
        const auto small = head + nested (20);
        const auto mb2 = rawBlob (small.toRawUTF8(), small.getNumBytesAsUTF8(), head.getNumBytesAsUTF8());
        CHECK (! stateRefused (mb2), "only the declared length is parsed");
    }
    {
        // Overlong UTF-8 for '<' (C0 BC): refused as invalid, or counted after decoding.
        juce::MemoryOutputStream text;
        for (int i = 0; i < 600; ++i)
            text << "\xc0\xbc" "a>";
        CHECK (stateRefused (rawBlob (text.getData(), text.getDataSize())), "overlong '<'");
        juce::MemoryOutputStream bad;
        bad << "<FlayrChop x=\"\xff\xfe\"/>";
        CHECK (stateRefused (rawBlob (bad.getData(), bad.getDataSize())), "invalid UTF-8");
    }
    {
        // UTF-16 (with and without a BOM) has NULs; a UTF-8 BOM must not hide a declaration.
        const auto u16 = juce::String ("<FlayrChop/>");
        juce::MemoryOutputStream le, bom;
        bom.writeByte ((char) 0xff);
        bom.writeByte ((char) 0xfe);
        for (auto p = u16.getCharPointer(); ! p.isEmpty(); ++p)
        {
            le.writeShort ((short) *p);
            bom.writeShort ((short) *p);
        }
        CHECK (stateRefused (rawBlob (le.getData(), le.getDataSize())), "UTF-16LE");
        CHECK (stateRefused (rawBlob (bom.getData(), bom.getDataSize())), "UTF-16 with BOM");
        CHECK (stateRefused (blob (juce::String::fromUTF8 ("\xef\xbb\xbf") + decl + nested (600))), "UTF-8 BOM + declaration + nesting");
        FlayrChopProcessor::parseState (blob (juce::String::fromUTF8 ("\xef\xbb\xbf") + "<FlayrChop/>").getData(), 30); // must not crash
    }

    // Envelope.
    {
        auto mb = blob (ok);
        CHECK (FlayrChopProcessor::parseState (mb.getData(), 8) == nullptr, "8-byte blob");
        static_cast<char*> (mb.getData())[0] ^= 1;
        CHECK (stateRefused (mb), "bad magic");
        CHECK (stateRefused (rawBlob (ok.toRawUTF8(), ok.getNumBytesAsUTF8(), 1 << 30)), "declared length beyond the blob");
        CHECK (stateRefused (rawBlob (ok.toRawUTF8(), ok.getNumBytesAsUTF8(), 0)), "zero length");
        const auto big = "<FlayrChop x=\"" + juce::String::repeatedString ("a", FlayrChopProcessor::kMaxStateBytes) + "\"/>";
        CHECK (stateRefused (blob (big)), "oversize state");
    }

    // A real, full project (every pad, every parameter, long paths) fits with lots of headroom.
    {
        const auto longDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (repeat ("long folder name ", 40).trim());
        FlayrChopProcessor p;
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", longDir.getChildFile ("song.wav").getFullPathName(), nullptr);
        s.setProperty ("libraryPath", longDir.getChildFile ("library.wav").getFullPathName(), nullptr);
        for (int i = 0; i < chop::kNumPads; ++i)
        {
            juce::ValueTree t ("Pad");
            for (auto* name : { "index", "cue", "end", "gain", "pan", "keyShift", "stretch", "filter", "favorite", "attack", "release",
                                "mode", "reverse", "choke", "stems" })
                t.setProperty (name, juce::String (i) + ".123456789012", nullptr);
            s.appendChild (t, nullptr);
        }
        root.appendChild (p.apvts.copyState(), nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        std::printf ("full project state: %d bytes (limit %d)\n", (int) mb.getSize(), FlayrChopProcessor::kMaxStateBytes);
        CHECK ((int) mb.getSize() * 4 < FlayrChopProcessor::kMaxStateBytes, "full state %d bytes is too close to the limit", (int) mb.getSize());
        CHECK (! stateRefused (mb), "full project state refused");

        // And a refused blob changes nothing.
        const auto before = p.getStatus();
        const auto evil = blob (decl + "<!DOCTYPE a [<!ENTITY a \"&a;&a;\">]><FlayrChop><Sample path=\"/evil.wav\" x=\"&a;\"/></FlayrChop>");
        p.setStateInformation (evil.getData(), (int) evil.getSize());
        p.setStateInformation (blob (nested (100000)).getData(), (int) blob (nested (100000)).getSize());
        CHECK (! p.isFileMissing() && p.getStatus() == before, "refused state was applied");
    }
}

//==============================================================================
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

static juce::MemoryBlock projectState (FlayrChopProcessor& p, const juce::File& path, const char* paramValue = nullptr,
                                       const juce::String& libraryPath = {})
{
    juce::ValueTree root ("FlayrChop");
    root.setProperty ("version", "1.2.0", nullptr);
    auto params = p.apvts.copyState();
    if (paramValue != nullptr)
        for (auto child : params)
            child.setProperty ("value", paramValue, nullptr);
    root.appendChild (params, nullptr);
    juce::ValueTree s ("Sample");
    s.setProperty ("path", path.getFullPathName(), nullptr);
    if (libraryPath.isNotEmpty())
        s.setProperty ("libraryPath", libraryPath, nullptr);
    s.setProperty ("bpm", 120.0, nullptr);
    juce::ValueTree t ("Pad");
    t.setProperty ("index", 0, nullptr);
    t.setProperty ("cue", 0.25, nullptr);
    t.setProperty ("filter", "nan", nullptr);
    t.setProperty ("gain", "inf", nullptr);
    s.appendChild (t, nullptr);
    root.appendChild (s, nullptr);
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
    return mb;
}

// Plays pad A1 for a while; true if every output sample was finite and within +-4.
static bool playIsFinite (FlayrChopProcessor& p, float& peak)
{
    p.setRateAndBufferSizeDetails (48000.0, 512);
    p.prepareToPlay (48000.0, 512);
    for (int i = 0; i < 400 && p.isRendering(); ++i)
        pump (0.05);
    juce::AudioBuffer<float> buf (2, 512);
    bool finite = true;
    peak = 0.0f;
    for (int block = 0; block < 200; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote, (juce::uint8) 127), 0);
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const float x = buf.getSample (c, i);
                finite = finite && std::isfinite (x) && std::abs (x) <= 4.0f;
                if (std::isfinite (x))
                    peak = juce::jmax (peak, std::abs (x));
            }
    }
    return finite;
}

static void nanState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("nan.wav"), 3.0);
    FlayrChopProcessor p;
    const auto st = projectState (p, wav, "nan");
    p.setStateInformation (st.getData(), (int) st.getSize());
    CHECK (waitLoaded (p), "nan state: never loaded");
    bool allFinite = true;
    for (auto* param : p.getParameters())
        allFinite = allFinite && std::isfinite (param->getValue());
    CHECK (allFinite, "nan state: a parameter stayed non-finite");
    CHECK (std::isfinite (p.getPad (0).gainDb) && std::isfinite (p.getPad (0).filter), "nan state: pad values not finite");
    float peak = 0.0f;
    CHECK (playIsFinite (p, peak), "nan state: non-finite output");
    CHECK (peak > 0.01f, "nan state: pad A1 silent (peak %f)", peak);

    // A host (or automation) pushing NaN straight into a parameter: output stays finite.
    p.apvts.getParameter ("filter")->setValue (std::numeric_limits<float>::quiet_NaN());
    p.apvts.getParameter ("volume")->setValue (std::numeric_limits<float>::infinity());
    CHECK (playIsFinite (p, peak), "NaN parameter: non-finite output");
}

//==============================================================================
static juce::MemoryBlock stateFrom (FlayrChopProcessor& p, bool otherThread)
{
    juce::MemoryBlock mb;
    if (otherThread)
        std::thread ([&] { p.getStateInformation (mb); }).join();
    else
        p.getStateInformation (mb);
    return mb;
}

static juce::ValueTree sampleIn (const juce::MemoryBlock& mb)
{
    const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    return xml != nullptr ? juce::ValueTree::fromXml (*xml).getChildWithName ("Sample") : juce::ValueTree();
}

// Hosts that save and restore off the message thread.
static void threadedState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("threads.wav"), 3.0);
    {
        FlayrChopProcessor p;
        const auto st = projectState (p, wav);
        std::thread ([&] { p.setStateInformation (st.getData(), (int) st.getSize()); }).join();
        // Not applied yet: a save from any thread returns the restored project unchanged (as PR #5 does).
        CHECK (stateFrom (p, true) == st, "threaded restore: off-thread save before apply differs");
        CHECK (stateFrom (p, false) == st, "threaded restore: message-thread save before apply differs");
        CHECK (waitLoaded (p), "threaded restore: never applied");
        CHECK (p.getPad (0).hasCue() && std::abs (p.getPad (0).cue - 0.25) < 1e-6, "threaded restore: pad not restored");
        pump (0.2);
        const auto a = sampleIn (stateFrom (p, true)), b = sampleIn (stateFrom (p, false));
        CHECK (a.isValid() && a.isEquivalentTo (b), "threaded save differs from a message-thread save");
        CHECK (a.getChildWithProperty ("index", 0).isValid(), "threaded save lost pad A1");

        // A newer message-thread restore wins over an older queued one.
        const auto older = projectState (p, dir.getChildFile ("older.wav"));
        std::thread ([&] { p.setStateInformation (older.getData(), (int) older.getSize()); }).join();
        p.setStateInformation (st.getData(), (int) st.getSize());
        pump (0.3);
        CHECK (! p.getMissingFile().getFileName().contains ("older"), "an older queued restore overrode a newer one");
    }
    {
        // Destroyed with a restore still queued: nothing runs afterwards.
        auto p = std::make_unique<FlayrChopProcessor>();
        const auto st = projectState (*p, wav);
        std::thread ([&] { p->setStateInformation (st.getData(), (int) st.getSize()); }).join();
        p.reset();
        pump (0.2);
        CHECK (true, "survived");
    }
}

//==============================================================================
// Project paths on other machines are never opened (or checked) on restore.
static void networkPaths()
{
    for (auto* path : { "\\\\attacker\\share\\loop.wav", "//attacker/share/loop.wav", "/net/attacker/loop.wav",
                        "/Network/Servers/attacker/loop.wav", "\\\\?\\UNC\\attacker\\share\\loop.wav", "smb://attacker/loop.wav" })
    {
        CHECK (chop::isNetworkPath (path), "not a network path: %s", path);
        FlayrChopProcessor p;
        juce::ValueTree root ("FlayrChop");
        juce::ValueTree s ("Sample");
        s.setProperty ("path", path, nullptr);
        s.setProperty ("libraryPath", path, nullptr);
        juce::ValueTree t ("Pad");
        t.setProperty ("index", 3, nullptr);
        t.setProperty ("cue", 1.5, nullptr);
        s.appendChild (t, nullptr);
        root.appendChild (s, nullptr);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*root.createXml(), mb);
        p.setStateInformation (mb.getData(), (int) mb.getSize());
        // Decided without starting a load: no worker ever saw the path.
        CHECK (p.getBusy() == FlayrChopProcessor::Busy::None && ! p.hasSample(), "%s: a load was started", path);
        CHECK (p.isFileMissing() || ! juce::File::isAbsolutePath (path), "%s: not reported missing", path);
        CHECK (p.getStatus().contains ("Missing"), "%s: status '%s'", path, p.getStatus().toRawUTF8());
        const auto saved = sampleIn (stateFrom (p, false));
        CHECK (saved.getProperty ("path").toString() == path && saved.getChildWithProperty ("index", 3).isValid(),
               "%s: the project didn't save back unchanged", path);
    }
    for (auto* path : { "/Users/me/Music/loop.wav", "C:\\Music\\loop.wav", "/Volumes/Drive/loop.wav", "/network-music/loop.wav" })
        CHECK (! chop::isNetworkPath (path), "local path taken for a network one: %s", path);
}

// The library copy (fix for sandboxed hosts): a project whose original can't be read loads from it.
static juce::File noiseWav (const juce::File& f, double seconds)
{
    f.getParentDirectory().createDirectory();
    juce::Random rng (juce::Time::currentTimeMillis()); // unique audio, so the copy is ours alone
    const double sr = 48000.0;
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            buf.setSample (c, i, (i % 24000 < 300 ? 0.6f : 0.05f) * (rng.nextFloat() * 2.0f - 1.0f));
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
    auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return f;
}

static void libraryCopies (const juce::File& dir)
{
    const auto original = noiseWav (dir.getChildFile ("sandboxed/My Song.wav"), 3.0);
    juce::File copy;
    juce::MemoryBlock saved;
    {
        FlayrChopProcessor p;
        p.loadFile (original);
        CHECK (waitLoaded (p), "library: never loaded");
        copy = p.getLibraryCopy();
        CHECK (copy.existsAsFile() && copy.isAChildOf (chop::sampleLibraryFolder()), "library: no copy (%s)", copy.getFullPathName().toRawUTF8());
        CHECK (copy.getFileName() == juce::SHA256 (original).toHexString() + ".wav", "library: copy named %s", copy.getFileName().toRawUTF8());
        CHECK (copy.getSize() == original.getSize(), "library: copy size");
        CHECK (p.getFileName() == "My Song", "library: shows '%s' instead of the original name", p.getFileName().toRawUTF8());
        chop::Pad pad;
        pad.cue = 1.0;
        p.setPad (9, pad);
        saved = stateFrom (p, false);
        const auto s = sampleIn (saved);
        CHECK (s.getProperty ("path").toString() == original.getFullPathName(), "library: path is not the original");
        CHECK (s.getProperty ("libraryPath").toString() == copy.getFullPathName(), "library: libraryPath not saved");

        // The same audio from somewhere else shares the copy.
        const auto twin = dir.getChildFile ("twin.wav");
        original.copyFileTo (twin);
        CHECK (chop::libraryCopyOf (twin) == copy, "library: same audio, second copy");
    }
    {
        // The original exists but can't be read (what Logic's AU sandbox does to ~/Music/Logic and
        // ~/Downloads): the project loads from the library copy and still saves the original path.
        original.setReadOnly (true);
        ::chmod (original.getFullPathName().toRawUTF8(), 0);
        FlayrChopProcessor p;
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        CHECK (waitLoaded (p), "unreadable original: library copy not used (status '%s')", p.getStatus().toRawUTF8());
        CHECK (p.getPad (9).hasCue() && std::abs (p.getPad (9).cue - 1.0) < 1e-6, "unreadable original: pads not restored");
        const auto s = sampleIn (stateFrom (p, false));
        CHECK (s.getProperty ("path").toString() == original.getFullPathName(), "unreadable original: path changed");
        CHECK (s.getProperty ("libraryPath").toString() == copy.getFullPathName(), "unreadable original: libraryPath lost");
        ::chmod (original.getFullPathName().toRawUTF8(), 0644);
    }
    {
        // A libraryPath outside the library folder is ignored (a project can't point the loader anywhere).
        FlayrChopProcessor p;
        const auto st = projectState (p, dir.getChildFile ("gone.wav"), nullptr, original.getFullPathName());
        p.setStateInformation (st.getData(), (int) st.getSize());
        pump (0.5);
        waitIdle (p);
        CHECK (! p.hasSample() && p.isFileMissing(), "libraryPath outside the library was loaded");
    }
    {
        // Both gone: missing, and the project saves back as it came (relink still possible).
        copy.deleteFile();
        original.deleteFile();
        FlayrChopProcessor p;
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        waitIdle (p);
        CHECK (! p.hasSample() && p.isFileMissing(), "both gone: not missing");
        const auto s = sampleIn (stateFrom (p, false));
        CHECK (s.getChildWithProperty ("index", 9).isValid() && s.getProperty ("libraryPath").toString() == copy.getFullPathName(),
               "both gone: project not saved back unchanged");
    }
    dir.getChildFile ("twin.wav").deleteFile();
}

// The state format older versions (and hand-made Logic projects) carry: no version, no UI, no
// libraryPath, a parameters child and a Sample with only a path and pads.
static void legacyState (const juce::File& dir)
{
    const auto wav = goodWav (dir.getChildFile ("legacy.wav"), 3.0);
    const juce::String xml = "<FlayrChop><FlayrChop><PARAM id=\"volume\" value=\"-6.0\"/><PARAM id=\"keyShift\" value=\"2.0\"/></FlayrChop>"
                             "<Sample path=\"" + wav.getFullPathName() + "\" bpm=\"100.0\">"
                             "<Pad index=\"0\" cue=\"0.5\"/><Pad index=\"12\" cue=\"2.0\" gain=\"-3.0\"/></Sample></FlayrChop>";
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int) xml.getNumBytesAsUTF8());
    out << xml;
    out.writeByte (0);
    FlayrChopProcessor p;
    p.setStateInformation (out.getData(), (int) out.getDataSize());
    CHECK (waitLoaded (p), "legacy state: never loaded");
    CHECK (p.getPad (12).hasCue() && std::abs (p.getPad (12).cue - 2.0) < 1e-6 && std::abs (p.getSampleBpm() - 100.0) < 1e-6,
           "legacy state: project not applied");
    CHECK (std::abs (p.apvts.getRawParameterValue ("volume")->load() + 6.0f) < 1e-3f, "legacy state: parameters not applied");
    if (const auto copy = p.getLibraryCopy(); copy.existsAsFile())
        copy.deleteFile();

    // The auto-chop menu has 8 modes; the restore used to clamp to the first 5.
    for (auto [saved, expected] : { std::pair<int, int> { 7, 7 }, { 5, 5 }, { 99, 7 }, { -3, 0 } })
    {
        const auto ui = blob ("<FlayrChop><UI chopMode=\"" + juce::String (saved) + "\"/></FlayrChop>");
        p.setStateInformation (ui.getData(), (int) ui.getSize());
        CHECK (p.chopModeUI == expected, "chopMode %d restored as %d", saved, p.chopModeUI);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::createTempFile ("flayr-chop-hardening");
    dir.createDirectory();
    chop::setSampleLibraryFolderForTests (dir.getChildFile ("Music/Flayr Chop/Samples")); // never the real one

    crashFiles (dir);
    stateXml();
    nanState (dir);
    threadedState (dir);
    networkPaths();
    libraryCopies (dir);
    legacyState (dir);

    dir.deleteRecursively();
    std::printf ("hardening_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
