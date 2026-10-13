#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Renderer.h"
#include "SampleLoader.h"

class FlayrChopProcessor : public juce::AudioProcessor,
                           public juce::ChangeBroadcaster,
                           private juce::Timer
{
public:
    FlayrChopProcessor();
    ~FlayrChopProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::String stemLevelId (int s) { return "stemLevel" + juce::String (s); }
    static juce::String stemMuteId (int s) { return "stemMute" + juce::String (s); }
    static juce::String stemSoloId (int s) { return "stemSolo" + juce::String (s); }

    //==============================================================================
    // Message thread API for the editor.
    enum class Busy { None, Loading, Separating };

    void loadFile (const juce::File&);
    void separateStems();
    void cancelStems();
    void removeStems();
    void chop (chop::analysis::ChopMode, int count, int firstPad, double beatsPerCue = 1.0);
    void clearPads (int firstPad, int count);

    bool hasSample() const { return sample != nullptr; }
    const chop::LoadedSample* getSample() const { return sample.get(); }
    juce::String getFileName() const { return sample ? sample->file.getFileNameWithoutExtension() : missingFile.getFileNameWithoutExtension(); }
    bool isFileMissing() const { return sample == nullptr && missingFile != juce::File(); }
    juce::File getMissingFile() const { return missingFile; }
    Busy getBusy() const { return busy; }
    float getProgress() const { return progress.load(); }
    juce::String getStatus() const { return status; }
    bool isRendering() const { return renderer.busy.load(); }
    bool isDraft() const { const auto* r = rendered.get(); return r != nullptr && r->draft; }

    const juce::StringArray& getStemNames() const { return stemNames; }
    int numStems() const { return stemNames.size(); }

    chop::Pad getPad (int i) const { return pads[(size_t) i]; }
    void setPad (int i, const chop::Pad&);
    void setCue (int pad, double seconds); // snaps to the nearest transient when snap is on
    double padEnd (int pad) const;          // resolved end in seconds
    int padAtTime (double seconds) const;   // which pad's slice contains this time (-1 none)

    double getSampleBpm() const { return sampleBpm; }
    void setSampleBpm (double);
    int getSampleKey() const { return sampleKey; }
    bool isSampleMinor() const { return sampleMinor; }
    void setSampleKey (int key, bool minor);
    int getTargetKey() const; // sample key + key shift
    double getTargetBpm() const;
    double getSpeed() const;
    double getHostBpm() const { return hostBpm.load(); }

    void triggerFromUI (int note, bool on); // pads clicked or keys played in the editor

    // Current audio for exports and drag-out (message thread).
    const chop::Rendered* currentRender() const { return rendered.get(); }
    juce::File exportSlice (int pad, const juce::File& folder);
    bool exportAll (const juce::File& folder, bool stems);

    // UI state, saved with the project.
    int selectedPad = 0;
    int bank = 0;
    int chopModeUI = 0, chopCountUI = 16;
    bool snapUI = true;

    juce::AudioProcessorValueTreeState apvts;
    chop::Sampler sampler;
    std::atomic<double> hostBpm { 0.0 };
    std::atomic<bool> hostPlaying { false };

private:
    void timerCallback() override;
    void publishPads();
    void publishSource (std::vector<chop::StereoPtr> stemsAtFileRate, juce::StringArray names);
    chop::RenderSettings currentRenderSettings() const;
    chop::Globals readGlobals() const;
    void runJob (std::function<void()> job);
    void applyLoaded (std::shared_ptr<chop::LoadedSample>, bool keepPads);
    void handleMidi (const juce::MidiMessage&, int sampleOffset, double ppqAtBlock, double bpm, bool playing, int blockSize);
    void fireNote (int note, float velocity);
    double quantizeGridBeats() const;

    // Audio data published to the audio thread.
    chop::Published<chop::Rendered> rendered;
    chop::Published<chop::KeysRender> keysRendered;
    chop::Published<chop::PadTable> padTable;
    chop::Renderer renderer { rendered, keysRendered };

    // Message-thread model.
    std::shared_ptr<chop::LoadedSample> sample;
    chop::SourcePtr source;
    juce::StringArray stemNames;
    std::array<chop::Pad, chop::kNumPads> pads {};
    double sampleBpm = 120.0, sampleFirstBeat = 0.0;
    int sampleKey = 0;
    bool sampleMinor = false;
    juce::File missingFile;
    Busy busy = Busy::None;
    std::atomic<float> progress { 0.0f };
    juce::String status;
    uint64_t sourceCounter = 0;
    std::atomic<double> hostRate { 0.0 };
    double sourceRate = 0.0;

    // Background work: one job at a time; results come back through the inbox on the timer.
    std::thread worker;
    std::atomic<bool> cancelJob { false };
    std::mutex inboxMutex;
    std::vector<std::function<void()>> inbox;
    std::unique_ptr<juce::ValueTree> pendingPadState; // pads restored with a project, applied after load

    // UI note queue into the audio thread.
    juce::AbstractFifo uiFifo { 256 };
    std::array<std::pair<int, bool>, 256> uiNotes {};

    // Quantized hits waiting for the grid.
    struct Pending { int note = -1; float velocity = 0; int64_t samplesLeft = 0; bool releaseAfter = false; };
    std::array<Pending, 64> pending {};
    std::array<bool, 128> held {};

    std::atomic<float>* rawVolume = nullptr;
    std::atomic<float>* rawFilter = nullptr;
    std::atomic<float>* rawVelocity = nullptr;
    std::atomic<float>* rawMono = nullptr;
    std::atomic<float>* rawKeys = nullptr;
    std::atomic<float>* rawQuantize = nullptr;
    std::array<std::atomic<float>*, chop::kMaxStems> rawStemLevel {}, rawStemMute {}, rawStemSolo {};
    std::atomic<int> keysPadAudio { 0 };
    std::atomic<int> numStemsAudio { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlayrChopProcessor)
};
