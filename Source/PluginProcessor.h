#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <bitset>

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

    // A saved state as XML, or nullptr if it breaks any limit untrusted project data must keep to
    // (size, nesting, no DTDs). Pure; safe on any thread.
    static std::unique_ptr<juce::XmlElement> parseState (const void* data, int sizeInBytes);
    static constexpr int kMaxStateBytes = 256 * 1024; // a full project with long paths is ~30 KB
    static constexpr int kMaxStateElements = 512;    // a full project is ~100
    static constexpr int kMaxStateDepth = 8;         // a full project is 3

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

    // Auto-chop modes beyond chop(): favorites are always left alone.
    void slicerFromPlayhead (int firstPad, int count, double beatsApart);
    void randomCues (int firstPad, int count);
    void keyShiftPad (int sourcePad, int firstPad); // copies one pad across a bank, -7..+8 semitones

    // Transport: plays the whole track from the playhead (independent of the DAW's transport).
    void play();
    void stop();
    bool isPlaying() const { return sampler.isPreviewing(); }
    double getPlayhead() const; // seconds into the source
    void setPlayhead (double seconds);

    // Cues. An empty pad hit (click, key or MIDI) drops a cue at the playhead.
    void setCueAtPlayhead (int pad);
    void nudge (const std::vector<int>& pads, double seconds);
    void setEnd (int pad, double seconds);
    void deletePads (const std::vector<int>& pads);
    void movePads (const std::vector<int>& from, int toFirst, bool copy); // keeps their layout
    int firstEmptyPad (int bankHint) const;

    // Selection (message thread). selectedPad is the primary one the inspector shows.
    std::vector<int> getSelection() const;
    bool isSelected (int pad) const { return selection.test ((size_t) pad); }
    void select (int pad, bool additive);
    void selectAll (int bank);

    bool hasSample() const { return sample != nullptr; }
    const chop::LoadedSample* getSample() const { return sample.get(); }
    juce::String getFileName() const { return sample ? sampleOriginal.getFileNameWithoutExtension() : missingFile.getFileNameWithoutExtension(); }
    juce::File getLibraryCopy() const { return sampleLibrary; }
    bool isFileMissing() const { return sample == nullptr && missingFile != juce::File(); }
    juce::File getMissingFile() const { return missingFile; }
    Busy getBusy() const { return busy; }
    float getProgress() const { return progress.load(); }
    juce::String getStatus() const { return status; }
    void setStatus (const juce::String& s) { status = s; sendChangeMessage(); }
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
    void setSampleBpm (double, bool fromUser = true);
    float getBpmConfidence() const { return sample ? sample->analysis.bpmConfidence : 0.0f; }
    float getKeyConfidence() const { return sample ? sample->analysis.keyConfidence : 0.0f; }
    bool isBpmFromUser() const { return bpmFromUser; }
    bool isKeyFromUser() const { return keyFromUser; }
    bool isRefinedFromStems() const { return refinedFromStems; }
    int getSampleKey() const { return sampleKey; }
    bool isSampleMinor() const { return sampleMinor; }
    void setSampleKey (int key, bool minor, bool fromUser = true);
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
    void applyRefined (const std::shared_ptr<chop::LoadedSample>&, const chop::analysis::Result&);
    void handleMidi (const juce::MidiMessage&, int sampleOffset, double ppqAtBlock, double bpm, bool playing, int blockSize);
    void fireNote (int note, float velocity);
    double quantizeGridBeats() const;

    // Audio data published to the audio thread.
    chop::Published<chop::Rendered> rendered;
    chop::Published<chop::KeysRender> keysRendered;
    chop::Published<chop::PadRenders> padRendered;
    chop::Published<chop::PadTable> padTable;
    chop::Renderer renderer { rendered, keysRendered, padRendered };
    std::bitset<chop::kNumPads> selection;
    double playheadSec = 0.0;

    // Transport commands into the audio thread, and cue hits coming back from MIDI.
    std::atomic<int> transportCmd { 0 };   // 1 play, 2 stop
    std::atomic<double> transportFrom { 0.0 };
    std::atomic<double> idlePlayhead { 0.0 }; // where play starts when stopped
    juce::AbstractFifo cueFifo { 64 };
    std::array<std::pair<int, float>, 64> cueHits {};
    void drainCueHits();

    // Message-thread model.
    std::shared_ptr<chop::LoadedSample> sample;
    chop::SourcePtr source;
    juce::StringArray stemNames;
    std::array<chop::Pad, chop::kNumPads> pads {};
    double sampleBpm = 120.0, sampleFirstBeat = 0.0;
    bool bpmFromUser = false, keyFromUser = false, refinedFromStems = false;
    int sampleKey = 0;
    bool sampleMinor = false;
    juce::File missingFile;
    juce::File sampleOriginal, sampleLibrary; // what the loaded sample saves as "path" and "libraryPath"
    uint64_t loadGeneration = 0;              // only the newest load's result is applied
    void restoreSample (const juce::String& path, const juce::String& libraryPath);
    void loadCandidates (const juce::File& original, std::vector<juce::File> candidates, const juce::File& knownLibraryCopy);
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

    // Project state across threads (see getStateInformation).
    void applyState (const juce::XmlElement&);
    juce::ValueTree buildSampleState() const;
    juce::ValueTree buildUiState() const;
    void refreshStateSnapshot();
    std::mutex stateMutex;
    juce::ValueTree snapshotSample { "Sample" }, snapshotUi { "UI" };
    std::unique_ptr<juce::MemoryBlock> pendingState; // restored off the message thread, not applied yet
    uint64_t stateGeneration = 0;
    int snapshotTicks = 0;
    std::shared_ptr<int> lifetime = std::make_shared<int> (0);

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
