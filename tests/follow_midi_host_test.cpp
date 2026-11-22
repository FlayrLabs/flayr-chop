// Follow MIDI with the editor open (issue #7): MIDI note-ons arrive in processBlock as a host
// such as Ableton Live sends them, the message loop and the processor's timer run, and the
// pad settings panel the editor shows must follow the last pad played. Clicks still select.
// Built with the plug-in sources (see CMakeLists.txt); run by ctest.
#include "PluginEditor.h"
#include "SampleLoader.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

static void pump (double seconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

static juce::String expectedTitle (int pad)
{
    return "PAD " + juce::String::charToString ((juce::juce_wchar) ('A' + pad / chop::kPadsPerBank))
         + juce::String (pad % chop::kPadsPerBank + 1);
}

// The pad settings panel's title, read off the editor's components ("PAD A4", or "N PADS").
static juce::String panelTitle (juce::Component& root)
{
    for (auto* c : root.getChildren())
    {
        if (auto* l = dynamic_cast<juce::Label*> (c))
            if (l->getText().startsWith ("PAD ") || l->getText().endsWith (" PADS"))
                return l->getText();
        if (const auto t = panelTitle (*c); t.isNotEmpty())
            return t;
    }
    return {};
}

static std::vector<ui::PadButton*> padButtons (juce::Component& root)
{
    std::vector<ui::PadButton*> r;
    for (auto* c : root.getChildren())
    {
        if (auto* b = dynamic_cast<ui::PadButton*> (c))
            r.push_back (b);
        const auto sub = padButtons (*c);
        r.insert (r.end(), sub.begin(), sub.end());
    }
    return r;
}

static void click (ui::PadButton& b)
{
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const juce::Point<float> at = b.getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent down (source, at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &b, &b, now, at, now, 1, false);
    const juce::MouseEvent up (source, at, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &b, &b, now, at, now, 1, false);
    b.mouseDown (down);
    b.mouseUp (up);
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = juce::File::createTempFile ("flayr-chop-follow");
    dir.createDirectory();
    chop::setSampleLibraryFolderForTests (dir.getChildFile ("Samples")); // never the user's folders
    chop::setStemCacheRootForTests (dir.getChildFile ("stems"));
    {
        FlayrChopProcessor proc;
        proc.prepareToPlay (44100.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        pump (0.1);

        // Ableton: notes land at sample offsets inside a block, sometimes several per block,
        // the processor runs on its own (here: this) thread between timer ticks.
        juce::AudioBuffer<float> buf (2, 256);
        auto block = [&] (std::vector<std::pair<int, int>> events) { // { pad, velocity } at increasing offsets
            juce::MidiBuffer midi;
            int at = 3;
            for (auto [pad, vel] : events)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, chop::kFirstPadNote + pad, (juce::uint8) vel), at);
                at += 40;
            }
            buf.clear();
            proc.processBlock (buf, midi);
        };

        std::printf ("Follow MIDI default: %d\n", (int) proc.followMidiUI);
        const std::pair<std::vector<std::pair<int, int>>, int> seq[] = {
            { { { 5, 100 } }, 5 },                     // one hit
            { { { 1, 90 }, { 12, 70 } }, 12 },         // two in a block: the last wins
            { { { 18, 127 } }, 18 },                   // a pad on bank B (C1 + 18)
            { { { 7, 0 } }, 18 },                      // velocity-0 note-on is a note-off: ignored
        };
        for (const auto& [events, want] : seq)
        {
            block (events);
            pump (0.1); // message loop + the processor's 30 Hz timer
            const auto title = panelTitle (*ed);
            std::printf ("  after MIDI: selected %d, panel '%s'\n", proc.selectedPad, title.toRawUTF8());
            check (proc.selectedPad == want && proc.getSelection() == std::vector<int> { want }
                       && title == expectedTitle (want),
                   "selection and settings panel follow the last note-on (" + expectedTitle (want).toStdString() + ")");
        }
        // Several blocks between two timer ticks.
        block ({ { 2, 100 } });
        block ({ { 9, 100 } });
        pump (0.1);
        check (proc.selectedPad == 9 && panelTitle (*ed) == expectedTitle (9), "last of several blocks wins");

        // A mouse click on a pad still selects it.
        auto pads = padButtons (*ed);
        std::printf ("  %d pad buttons on screen, bank %d\n", (int) pads.size(), proc.bank);
        if (pads.size() >= 4)
        {
            click (*pads[3]);
            pump (0.1);
            const int want = pads[3]->index();
            std::printf ("  after click: selected %d, panel '%s'\n", proc.selectedPad, panelTitle (*ed).toRawUTF8());
            check (proc.selectedPad == want && panelTitle (*ed) == expectedTitle (want), "click selects and shows the pad");
        }
        else
        {
            check (false, "pad buttons found");
        }

        // Off: MIDI no longer moves the selection.
        proc.followMidiUI = false;
        const int before = proc.selectedPad;
        block ({ { 11, 100 } });
        pump (0.1);
        std::printf ("  follow off: selected %d, panel '%s'\n", proc.selectedPad, panelTitle (*ed).toRawUTF8());
        check (proc.selectedPad == before, "with FOLLOW MIDI off the selection stays");

        // Saved and restored with the project, both ways.
        for (const bool on : { false, true })
        {
            proc.followMidiUI = on;
            juce::MemoryBlock st;
            proc.getStateInformation (st);
            FlayrChopProcessor other;
            other.followMidiUI = ! on;
            other.setStateInformation (st.getData(), (int) st.getSize());
            // A restore goes through the loader worker; its result lands on a timer tick.
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            do
                pump (0.05);
            while (other.getBusy() != FlayrChopProcessor::Busy::None && juce::Time::getMillisecondCounterHiRes() - t0 < 60000);
            pump (0.05);
            check (other.followMidiUI == on, std::string ("FOLLOW MIDI ") + (on ? "on" : "off") + " survives save and restore");
        }
        ed.reset();
        pump (0.05);
    }
    dir.deleteRecursively();
    std::printf ("follow_midi_host_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
