// Fuzz target: a saved project through setStateInformation and the whole restore: parseState's
// pre-scan and limits, JUCE's XML parser, parameters (NaN/inf clamping), UI values, the Sample
// element (pads, tempo, key) and its paths (absolute, relative, network-looking, library copies),
// the sample load that a valid path starts, and playback of the restored pads.
//
// Input: byte 0 holds flags, the rest is the state.
//   bit 0  the rest is the raw blob (envelope included); otherwise it is the XML text and the harness
//          wraps it in the copyXmlToBinary envelope with the right length
//   bit 1  restore from another thread (the host-thread path that queues the state)
//   bit 2  play the restored pads and check the output stays finite
//
// Invariant: a project the plug-in accepted must save back into a state it accepts again.

#include "FuzzCommon.h"
#include "PluginProcessor.h"

#include <cmath>
#include <thread>

namespace
{
juce::File songFile, libraryCopy;

// Runs the message loop until a load the restore started has been applied (its result arrives on
// the processor's 30 Hz timer and clears Busy on the message thread). A restore that starts no load
// (refused, missing or network path) costs one short loop for queued async work.
void pumpUntilIdle (FlayrChopProcessor& p, double maxSeconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + maxSeconds * 1000.0;
    auto* mm = juce::MessageManager::getInstance();
    mm->runDispatchLoopUntil (1);
    while (p.getBusy() != FlayrChopProcessor::Busy::None && juce::Time::getMillisecondCounterHiRes() < end)
        mm->runDispatchLoopUntil (2);
}

juce::MemoryBlock envelope (const void* text, size_t bytes)
{
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int) bytes);
    out.write (text, bytes);
    out.writeByte (0);
    return out.getMemoryBlock();
}

// XML text of a state blob (what the harness wraps again), with a flags byte in front.
fuzz::Bytes asInput (uint8_t flags, const juce::MemoryBlock& state, bool raw)
{
    fuzz::Bytes b { flags };
    if (raw)
    {
        const auto* p = static_cast<const uint8_t*> (state.getData());
        b.insert (b.end(), p, p + state.getSize());
        return b;
    }
    if (const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
    {
        const auto text = xml->toString();
        b.insert (b.end(), text.toRawUTF8(), text.toRawUTF8() + text.getNumBytesAsUTF8());
    }
    return b;
}

juce::MemoryBlock stateOf (const juce::String& xml)
{
    return envelope (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

std::vector<fuzz::Bytes> makeSeeds()
{
    std::vector<fuzz::Bytes> seeds;
    juce::MemoryBlock full;
    {
        // A real project: loaded sample, pads with every field set, parameters and UI changed.
        FlayrChopProcessor p;
        p.loadFile (songFile);
        pumpUntilIdle (p, 30.0);
        p.chop (chop::analysis::ChopMode::Even, 8, 0);
        for (int i = 0; i < 4; ++i)
        {
            auto pad = p.getPad (i);
            pad.gainDb = -3.0f;
            pad.pan = 0.25f;
            pad.keyShift = 2.0f;
            pad.stretch = 1.5f;
            pad.filter = -0.5f;
            pad.favorite = true;
            pad.reverse = i % 2 == 1;
            pad.choke = i;
            pad.mode = (chop::PadMode) (i % 3);
            pad.stemMask = 0x5u;
            p.setPad (i, pad);
        }
        p.apvts.getParameter ("volume")->setValueNotifyingHost (0.3f);
        p.bank = 2;
        p.chopModeUI = 3;
        p.getStateInformation (full);
    }
    seeds.push_back (asInput (0, full, false));
    seeds.push_back (asInput (1, full, true));
    seeds.push_back (asInput (2, full, false));
    seeds.push_back (asInput (4, full, false));
    {
        FlayrChopProcessor p;
        juce::MemoryBlock empty;
        p.getStateInformation (empty);
        seeds.push_back (asInput (0, empty, false));
    }
    const auto song = songFile.getFullPathName();
    const auto lib = libraryCopy.getFullPathName();
    for (const auto& xml : {
             // Older versions: no version, no UI, no libraryPath.
             juce::String ("<FlayrChop><FlayrChop><PARAM id=\"volume\" value=\"-6.0\"/><PARAM id=\"keyShift\" value=\"nan\"/></FlayrChop>"
                           "<Sample path=\"") + song + "\" bpm=\"100.0\"><Pad index=\"0\" cue=\"0.05\"/><Pad index=\"12\" cue=\"0.2\" gain=\"inf\"/></Sample></FlayrChop>",
             // Original unreadable / missing: the library copy.
             "<FlayrChop version=\"1.2.0\"><Sample path=\"/nonexistent/song.wav\" libraryPath=\"" + lib + "\"><Pad index=\"63\" cue=\"0.1\" end=\"0.05\"/></Sample></FlayrChop>",
             // Paths that must never be opened.
             juce::String ("<FlayrChop><Sample path=\"\\\\host\\share\\a.wav\" libraryPath=\"//host/share/b.wav\"><Pad index=\"3\" cue=\"1.5\"/></Sample></FlayrChop>"),
             juce::String ("<FlayrChop><Sample path=\"smb://host/a.wav\" libraryPath=\"/net/host/a.wav\"/><UI selected=\"99\" bank=\"-1\" chopMode=\"7\" chopCount=\"1000\" snap=\"0\"/></FlayrChop>"),
             juce::String ("<FlayrChop><Sample path=\"relative/a.wav\" bpm=\"-1e308\" firstBeat=\"nan\" key=\"40\" minor=\"1\"/></FlayrChop>"),
             "<FlayrChop><Sample path=\"" + song + "\" libraryPath=\"" + song + "\"><Pad index=\"0\" cue=\"1e300\" end=\"-1\" stems=\"-1\" mode=\"99\" choke=\"-5\" attack=\"nan\" release=\"inf\" stretch=\"0\" keyShift=\"1e9\"/></Sample></FlayrChop>" })
    {
        seeds.push_back (asInput (0, stateOf (xml), false));
        seeds.push_back (asInput (4, stateOf (xml), false));
    }
    return seeds;
}

bool playFinite (FlayrChopProcessor& p)
{
    p.setRateAndBufferSizeDetails (48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    for (int i = 0; i < 200 && p.isRendering(); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    juce::AudioBuffer<float> buf (2, 256);
    for (int block = 0; block < 24; ++block)
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote + (block % 16), (juce::uint8) 127), 0);
        if (block % 4 == 3)
            midi.addEvent (juce::MidiMessage::noteOff (1, chop::kFirstPadNote + (block % 16)), 128);
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                if (! std::isfinite (buf.getSample (c, i)) || std::abs (buf.getSample (c, i)) > 64.0f)
                    return false;
    }
    return true;
}
} // namespace

extern "C" int LLVMFuzzerInitialize (int* argc, char*** argv)
{
    fuzz::initJuce();
    fuzz::initWorkDir ("state-restore");
    const auto dir = fuzz::workDir();
    chop::setSampleLibraryFolderForTests (dir.getChildFile ("Music/Flayr Chop/Samples"));
    chop::setStemCacheRootForTests (dir.getChildFile ("stems"));

    // The sample every seed points at (fixed path, so saved seeds keep working), and its library copy.
    songFile = dir.getChildFile ("song.wav");
    juce::WavAudioFormat wav;
    // Short (0.3 s at 16 kHz): every input that restores it runs the whole load, analysis included.
    const auto bytes = fuzz::encode (wav, fuzz::testSignal (2, 4800, 16000.0), 16000.0, 16);
    songFile.replaceWithData (bytes.data(), bytes.size());
    libraryCopy = chop::libraryCopyOf (songFile);

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
    const uint8_t flags = data[0];
    const auto blob = (flags & 1) != 0 ? juce::MemoryBlock (data + 1, size - 1) : envelope (data + 1, size - 1);
    const bool accepted = FlayrChopProcessor::parseState (blob.getData(), (int) blob.getSize()) != nullptr;

    // The library copy may have been replaced by an earlier input's load; keep the seeds' copy valid.
    if (! libraryCopy.existsAsFile())
        libraryCopy = chop::libraryCopyOf (songFile);

    auto p = std::make_unique<FlayrChopProcessor>();
    if ((flags & 2) != 0)
        std::thread ([&] { p->setStateInformation (blob.getData(), (int) blob.getSize()); }).join();
    else
        p->setStateInformation (blob.getData(), (int) blob.getSize());
    pumpUntilIdle (*p, 10.0);
    FUZZ_ASSERT (p->getBusy() == FlayrChopProcessor::Busy::None, "still busy 10 s after a restore");

    // Whatever was restored saves back into something this version accepts.
    juce::MemoryBlock saved;
    p->getStateInformation (saved);
    if (accepted)
        FUZZ_ASSERT (FlayrChopProcessor::parseState (saved.getData(), (int) saved.getSize()) != nullptr,
                     "an accepted project saved back into a state that is refused (%d bytes)", (int) saved.getSize());

    for (auto* param : p->getParameters())
        FUZZ_ASSERT (std::isfinite (param->getValue()), "parameter %s is not finite", param->getName (32).toRawUTF8());
    FUZZ_ASSERT (p->selectedPad >= 0 && p->selectedPad < chop::kNumPads && p->bank >= 0 && p->bank < chop::kNumBanks,
                 "UI state out of range: pad %d bank %d", p->selectedPad, p->bank);
    if (p->hasSample())
    {
        for (int i = 0; i < chop::kNumPads; ++i)
        {
            const auto pad = p->getPad (i);
            FUZZ_ASSERT (! pad.hasCue() || (std::isfinite (pad.cue) && pad.cue >= 0.0 && pad.cue <= p->getSample()->lengthSec()),
                         "pad %d cue %f", i, pad.cue);
            FUZZ_ASSERT (std::isfinite (pad.gainDb) && std::isfinite (pad.pan) && std::isfinite (pad.keyShift) && std::isfinite (pad.stretch)
                             && std::isfinite (pad.filter) && std::isfinite (pad.attackMs) && std::isfinite (pad.releaseMs),
                         "pad %d has non-finite settings", i);
        }
        FUZZ_ASSERT (std::isfinite (p->getSampleBpm()) && p->getSampleBpm() > 0.0, "bpm %f", p->getSampleBpm());
        if ((flags & 4) != 0)
            FUZZ_ASSERT (playFinite (*p), "restored project plays non-finite or huge samples");
    }
    p.reset();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    return 0;
}
