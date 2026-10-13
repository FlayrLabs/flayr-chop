// Loads a track into the plug-in without a DAW, renders the editor to a PNG and plays every pad
// of bank A in turn into a WAV, so the whole chain (decode, analysis, chop, HQ Stretch, pads)
// can be checked by eye and ear.
//
//   FlayrChopSnapshot out.png track.mp3 [keyShift] [tempoBpm] [stems]
#include "PluginProcessor.h"
#include "PluginEditor.h"

struct FakeHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (false);
        p.setBpm (bpm);
        return p;
    }
};

static bool waitFor (const std::function<bool()>& done, double seconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (! done())
    {
        if (juce::Time::getMillisecondCounterHiRes() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    return true;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3)
    {
        std::printf ("usage: FlayrChopSnapshot out.png track [keyShift] [tempoBpm] [stems]\n");
        return 1;
    }
    const juce::String outPath (argv[1]);
    const juce::File track (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const int keyShift = argc > 3 ? juce::String (argv[3]).getIntValue() : 0;
    const double tempo = argc > 4 ? juce::String (argv[4]).getDoubleValue() : 0.0;
    const bool doStems = argc > 5 && juce::String (argv[5]) == "stems";

    FlayrChopProcessor proc;
    FakeHead head;
    proc.setPlayHead (&head);
    proc.setRateAndBufferSizeDetails (48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    auto t0 = juce::Time::getMillisecondCounterHiRes();
    proc.loadFile (track);
    if (! waitFor ([&] { return proc.hasSample() && proc.getBusy() == FlayrChopProcessor::Busy::None; }, 120))
    {
        std::printf ("load failed: %s\n", proc.getStatus().toRawUTF8());
        return 1;
    }
    const auto* s = proc.getSample();
    std::printf ("loaded %.1f s in %.0f ms: %.2f BPM (conf %.2f), key %s (conf %.2f), %zu onsets\n", s->lengthSec(),
                 juce::Time::getMillisecondCounterHiRes() - t0, s->analysis.bpm, s->analysis.bpmConfidence,
                 chop::analysis::keyName (s->analysis.key, s->analysis.minor), s->analysis.keyConfidence,
                 s->analysis.onsetsSec.size());

    if (doStems)
    {
        t0 = juce::Time::getMillisecondCounterHiRes();
        proc.separateStems();
        waitFor ([&] { return proc.getBusy() == FlayrChopProcessor::Busy::None; }, 900);
        std::printf ("stems: %s (%d) in %.1f s\n", proc.getStatus().toRawUTF8(), proc.numStems(),
                     (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
    }

    proc.apvts.getParameter ("keyShift")->setValueNotifyingHost (proc.apvts.getParameter ("keyShift")->convertTo0to1 ((float) keyShift));
    if (tempo > 0.0)
    {
        head.bpm = tempo;
        proc.apvts.getParameter ("tempoMode")->setValueNotifyingHost (proc.apvts.getParameter ("tempoMode")->convertTo0to1 (1));
    }
    else
    {
        proc.apvts.getParameter ("tempoMode")->setValueNotifyingHost (0.0f);
    }

    // One block so the processor sees the host tempo, then let the render finish.
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    proc.processBlock (buf, midi);
    t0 = juce::Time::getMillisecondCounterHiRes();
    waitFor ([&] { return false; }, 0.1);
    waitFor ([&] { return ! proc.isRendering() && ! proc.isDraft() && proc.currentRender() != nullptr; }, 120);
    std::printf ("render: speed %.3f, key shift %d, %.0f ms\n", proc.getSpeed(), keyShift, juce::Time::getMillisecondCounterHiRes() - t0);

    // Play each pad of bank A for up to 1.5 s.
    juce::AudioBuffer<float> out (2, (int) (48000 * 1.5 * 16));
    out.clear();
    int pos = 0;
    for (int pad = 0; pad < 16; ++pad)
    {
        if (! proc.getPad (pad).hasCue())
            continue;
        const int len = (int) (48000 * juce::jmin (1.5, proc.padEnd (pad) - proc.getPad (pad).cue + 0.05) / proc.getSpeed());
        for (int done = 0; done < len && pos + 512 <= out.getNumSamples(); done += 512, pos += 512)
        {
            midi.clear();
            if (done == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote + pad, (juce::uint8) 127), 0);
            proc.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, buf, ch, 0, 512);
        }
    }
    const juce::File wav (outPath.upToLastOccurrenceOf (".", false, false) + ".wav");
    chop::Stereo st;
    st.l.assign (out.getReadPointer (0), out.getReadPointer (0) + pos);
    st.r.assign (out.getReadPointer (1), out.getReadPointer (1) + pos);
    chop::writeWav (wav, st, 48000.0);
    std::printf ("pads audio: %s (%.1f s, peak %.2f)\n", wav.getFullPathName().toRawUTF8(), pos / 48000.0,
                 out.getMagnitude (0, pos));

    // Leave a pad sounding so the playhead shows.
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote + 2, (juce::uint8) 127), 0);
    proc.processBlock (buf, midi);
    midi.clear();
    for (int i = 0; i < 20; ++i)
        proc.processBlock (buf, midi);
    proc.selectedPad = 2;

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    if (auto* e = dynamic_cast<FlayrChopEditor*> (ed.get()))
        e->refreshAll();
    waitFor ([&] { return false; }, 0.2);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File f (outPath);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s\n", outPath.toRawUTF8());

    // Project round trip: a second instance restores the first one's state.
    auto p = proc.getPad (5);
    p.gainDb = -4.5f;
    p.reverse = true;
    p.stemMask = 0b1001;
    proc.setPad (5, p);
    proc.setSampleBpm (proc.getSampleBpm() * 2.0);
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    FlayrChopProcessor copy;
    copy.prepareToPlay (48000.0, 512);
    copy.setStateInformation (state.getData(), (int) state.getSize());
    waitFor ([&] { return copy.hasSample() && copy.getBusy() == FlayrChopProcessor::Busy::None; }, 60);
    int same = 0;
    for (int i = 0; i < chop::kNumPads; ++i)
        same += std::abs (copy.getPad (i).cue - proc.getPad (i).cue) < 1e-9 ? 1 : 0;
    const auto q = copy.getPad (5);
    const bool ok = same == chop::kNumPads && std::abs (q.gainDb + 4.5f) < 1e-4f && q.reverse && q.stemMask == 0b1001
                    && std::abs (copy.getSampleBpm() - proc.getSampleBpm()) < 1e-9 && copy.numStems() == proc.numStems();
    std::printf ("state round trip: %s (%d/64 cues, bpm %.2f, %d stems, %zu bytes)\n", ok ? "OK" : "FAILED", same,
                 copy.getSampleBpm(), copy.numStems(), state.getSize());
    if (! ok)
        return 2;

    // The cue workflow: clear, play the track, hit an empty pad over MIDI, move, copy, nudge, end.
    int fails = 0;
    auto expect = [&] (bool cond, const char* what) { if (! cond) { ++fails; std::printf ("WORKFLOW FAIL: %s\n", what); } };
    std::vector<int> all;
    for (int i = 0; i < chop::kNumPads; ++i) all.push_back (i);
    proc.deletePads (all);
    int filled = 0;
    for (int i = 0; i < chop::kNumPads; ++i) filled += proc.getPad (i).hasCue() ? 1 : 0;
    expect (filled == 0, "clear all");

    proc.snapUI = false;
    proc.setPlayhead (10.0);
    proc.play();
    juce::AudioBuffer<float> blk (2, 512);
    juce::MidiBuffer none;
    for (int b = 0; b < 94; ++b) proc.processBlock (blk, none); // ~1 s of playback
    const double heard = proc.getPlayhead();
    juce::MidiBuffer hit;
    hit.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote + 3, (juce::uint8) 100), 0);
    proc.processBlock (blk, hit);
    waitFor ([&] { return proc.getPad (3).hasCue(); }, 2.0);
    expect (proc.getPad (3).hasCue() && std::abs (proc.getPad (3).cue - heard) < 0.03, "MIDI hit on an empty pad drops a cue at the playhead");
    std::printf ("live cue: playhead %.3f s -> pad cue %.3f s\n", heard, proc.getPad (3).cue);
    expect (std::abs (heard - 11.0) < 0.1, "transport plays from the playhead in real time");
    proc.stop();
    for (int b = 0; b < 4; ++b) proc.processBlock (blk, none);
    expect (! proc.isPlaying(), "stop");

    proc.setCueAtPlayhead (0);
    expect (proc.getPad (0).hasCue(), "empty pad click sets a cue");
    const double c0 = proc.getPad (0).cue;
    proc.movePads ({ 0 }, 8, false);
    expect (! proc.getPad (0).hasCue() && std::abs (proc.getPad (8).cue - c0) < 1e-9, "drag pad onto pad moves it");
    proc.movePads ({ 8 }, 9, true);
    expect (proc.getPad (8).hasCue() && std::abs (proc.getPad (9).cue - c0) < 1e-9, "option-drag copies");
    proc.movePads ({ 3, 8, 9 }, 4, true);
    expect (proc.getPad (4).hasCue() && proc.getPad (9).hasCue() && proc.getPad (10).hasCue(), "multi-pad copy keeps the layout");
    proc.nudge ({ 8 }, 0.01);
    expect (std::abs (proc.getPad (8).cue - (c0 + 0.01)) < 1e-9, "nudge");
    proc.setEnd (8, c0 + 0.5);
    expect (std::abs (proc.padEnd (8) - (c0 + 0.5)) < 1e-6, "end handle");
    proc.select (8, false);
    proc.select (9, true);
    expect (proc.getSelection().size() == 2, "shift-click multi-select");
    std::printf ("workflow: %s\n", fails == 0 ? "OK" : "FAILED");
    return fails == 0 ? 0 : 3;
}
