// A minimal plug-in host for UI checks: loads the installed plug-in (VST3 or AU) through its real
// wrapper, plays audio through it on a real-time thread and opens its editor in a real window.
//   FlayrChopMiniHost <plugin path or AU id> [seconds] [track]
// The track is loaded by dropping it on the editor (the same path a user takes).
#include <juce_audio_utils/juce_audio_utils.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

// This process's biggest on-screen window (the plug-in window), from the window server's list.
static CGWindowID ownWindow()
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        CGWindowID best = 0;
        double bestArea = 0;
        if (auto list = CGWindowListCopyWindowInfo (kCGWindowListOptionAll, kCGNullWindowID))
        {
            const auto pid = (long) getpid();
            for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
            {
                auto info = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
                long owner = 0, num = 0;
                CFNumberGetValue ((CFNumberRef) CFDictionaryGetValue (info, kCGWindowOwnerPID), kCFNumberLongType, &owner);
                CFNumberGetValue ((CFNumberRef) CFDictionaryGetValue (info, kCGWindowNumber), kCFNumberLongType, &num);
                CGRect r {};
                CGRectMakeWithDictionaryRepresentation ((CFDictionaryRef) CFDictionaryGetValue (info, kCGWindowBounds), &r);
                if (owner == pid && r.size.width * r.size.height > bestArea)
                {
                    bestArea = r.size.width * r.size.height;
                    best = (CGWindowID) num;
                }
            }
            CFRelease (list);
        }
        if (best != 0 && bestArea > 500 * 400)
            return best;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    return 0;
}

// Grabs exactly what the window server shows for one window (nothing else on screen).
static void grabWindow (CGWindowID wid, const juce::File& out)
{
    CGImageRef img = CGWindowListCreateImage (CGRectNull, kCGWindowListOptionIncludingWindow, wid,
                                              kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
    if (img == nullptr)
        return;
    auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) out.getFullPathName().toRawUTF8(),
                                                        (CFIndex) out.getFullPathName().getNumBytesAsUTF8(), false);
    if (auto dest = CGImageDestinationCreateWithURL (url, CFSTR ("public.png"), 1, nullptr))
    {
        CGImageDestinationAddImage (dest, img, nullptr);
        CGImageDestinationFinalize (dest);
        CFRelease (dest);
    }
    CFRelease (url);
    CGImageRelease (img);
}

struct Head : juce::AudioPlayHead
{
    std::atomic<juce::int64> sample { 0 };
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setBpm (120.0);
        p.setTimeInSamples (sample.load());
        p.setPpqPosition (sample.load() / 48000.0 * 2.0);
        return p;
    }
};

class App : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "FlayrChopMiniHost"; }
    const juce::String getApplicationVersion() override { return "1"; }

    void initialise (const juce::String& cmd) override
    {
        auto args = juce::StringArray::fromTokens (cmd, true);
        args.trim();
        for (auto& a : args) a = a.unquoted();
        const auto target = args[0];
        seconds = args.size() > 1 ? args[1].getDoubleValue() : 5.0;
        track = args.size() > 2 ? juce::File (args[2]) : juce::File();

        juce::addDefaultFormatsToManager (formats);
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : formats.getFormats())
            if (f->fileMightContainThisPluginType (target))
                f->findAllTypesForFile (found, target);
        if (found.isEmpty()) { std::printf ("no plug-in found at %s\n", target.toRawUTF8()); quit(); return; }

        juce::String err;
        plugin = formats.createPluginInstance (*found[0], 48000.0, 512, err);
        if (plugin == nullptr) { std::printf ("load failed: %s\n", err.toRawUTF8()); quit(); return; }
        plugin->setPlayHead (&head);
        plugin->setRateAndBufferSizeDetails (48000.0, 512);
        plugin->prepareToPlay (48000.0, 512);

        audio = std::thread ([this] {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            for (int block = 0; running; ++block)
            {
                midi.clear();
                if (block % 90 == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 36 + (block / 90) % 16, (juce::uint8) 110), 0);
                plugin->processBlock (buf, midi);
                head.sample += 512;
                std::this_thread::sleep_for (std::chrono::microseconds (10667));
            }
        });

        // Load the track while the window is closed, like a saved project.
        if (track.existsAsFile())
        {
            juce::MemoryBlock state;
            plugin->getStateInformation (state);
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            if (xml != nullptr)
            {
                auto* sample = xml->getChildByName ("Sample");
                if (sample == nullptr) sample = xml->createNewChildElement ("Sample");
                sample->setAttribute ("path", track.getFullPathName());
                juce::MemoryBlock mb;
                juce::AudioProcessor::copyXmlToBinary (*xml, mb);
                plugin->setStateInformation (mb.getData(), (int) mb.getSize());
            }
        }

        juce::Timer::callAfterDelay ((int) (openDelay * 1000), [this] {
            std::printf ("OPEN %.3f\n", juce::Time::getMillisecondCounterHiRes() / 1000.0);
            std::fflush (stdout);
            window = std::make_unique<juce::DocumentWindow> ("Flayr Chop", juce::Colours::black, 0);
            window->setUsingNativeTitleBar (true);
            window->setContentOwned (plugin->createEditorAndMakeActive(), true);
            window->setTopLeftPosition (40, 60);
            window->setVisible (true);
            window->toFront (true);
            // Capture the window ~60 times a second from a background thread.
            capture = std::thread ([this] {
                const auto id = ownWindow();
                std::printf ("window id %u\n", id);
                const auto start = juce::Time::getMillisecondCounterHiRes();
                for (int f = 0; id != 0 && capturing && f < 400; ++f)
                {
                    grabWindow (id, outDir.getChildFile ("w_" + juce::String (f).paddedLeft ('0', 3) + ".png"));
                    const auto next = start + (f + 1) * 16.67;
                    while (juce::Time::getMillisecondCounterHiRes() < next)
                        std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }
            });
            juce::Timer::callAfterDelay ((int) (seconds * 1000), [this] { quit(); });
        });
    }

    void shutdown() override
    {
        capturing = false;
        if (capture.joinable()) capture.join();
        window.reset();
        running = false;
        if (audio.joinable()) audio.join();
        plugin.reset();
    }

private:
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::DocumentWindow> window;
    Head head;
    std::thread audio;
    std::atomic<bool> running { true };
    double seconds = 5.0, openDelay = 4.0;
    juce::File track;
    juce::File outDir = juce::File::getCurrentWorkingDirectory();
    std::thread capture;
    std::atomic<bool> capturing { true };
};

START_JUCE_APPLICATION (App)
