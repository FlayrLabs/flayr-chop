#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

namespace ui
{
const juce::Colour bg      { 0xff101318 };
const juce::Colour panel   { 0xff181c24 };
const juce::Colour panel2  { 0xff1f2430 };
const juce::Colour line    { 0xff2a303c };
const juce::Colour text    { 0xffd8dde6 };
const juce::Colour dim     { 0xff7d8696 };
const juce::Colour accent  { 0xff35e0c4 };
const juce::Colour accent2 { 0xffff4f8b };
const juce::Colour warn    { 0xffffb547 };

juce::Colour padColour (int pad);
juce::Colour stemColour (const juce::String& name, int index);
juce::Font font (float size, bool bold = false);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float min, float max,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.0f, true); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
    juce::Font getLabelFont (juce::Label&) override { return font (12.0f); }
};

// Zoomable waveform with cue flags, slice shading, beat grid and playheads.
class Waveform : public juce::Component
{
public:
    explicit Waveform (FlayrChopProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

    void resetView();
    double viewStart = 0.0, viewLen = 0.0;
    std::function<void()> onViewChanged, onSelectionChanged;

private:
    double timeAt (float x) const;
    float xAt (double t) const;
    int flagAt (juce::Point<float>) const;
    void zoomAround (double t, double factor);
    void clampView();

    FlayrChopProcessor& proc;
    int draggingPad = -1;
    double dragTime = 0.0, dragViewStart = 0.0;
    int hoverFlag = -1;
};

class Overview : public juce::Component
{
public:
    Overview (FlayrChopProcessor& p, Waveform& w) : proc (p), wave (w) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { moveTo (e.position.x); }
    void mouseDrag (const juce::MouseEvent& e) override { moveTo (e.position.x); }

private:
    void moveTo (float x);
    FlayrChopProcessor& proc;
    Waveform& wave;
};

class PadButton : public juce::Component
{
public:
    PadButton (FlayrChopProcessor& p, int slotIndex) : proc (p), slot (slotIndex) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int index() const { return proc.bank * chop::kPadsPerBank + slot; }
    std::function<void()> onSelect;
    float level = 0.0f;

private:
    FlayrChopProcessor& proc;
    const int slot;
    int playingNote = -1;
    bool dragging = false;
};
} // namespace ui

class FlayrChopEditor : public juce::AudioProcessorEditor,
                        public juce::FileDragAndDropTarget,
                        private juce::Timer,
                        private juce::ChangeListener
{
public:
    explicit FlayrChopEditor (FlayrChopProcessor&);
    ~FlayrChopEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { fileHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { fileHover = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

    void refreshAll(); // pulls everything from the processor (also used by the snapshot tool)

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct StemStrip
    {
        juce::Label name;
        juce::Slider level { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        juce::TextButton mute { "M" }, solo { "S" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAtt, soloAtt;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshAll(); }
    void setupKnob (Knob&, const juce::String& name);
    void attachKnob (Knob&, const juce::String& paramId);
    void editPad (const std::function<void (chop::Pad&)>&);
    void refreshPadInspector();
    void refreshStems();
    void choosFile();
    void showExportMenu();
    void setTargetKey (int key);
    void showBpmMenu();
    void showKeyMenu();
    void tap();
    void layoutStems (juce::Rectangle<int>);

    FlayrChopProcessor& proc;
    ui::LookAndFeel lnf;

    // Header
    juce::TextButton loadBtn { "LOAD" }, exportBtn { "EXPORT" };
    juce::Label fileLabel, statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::TextButton cancelBtn { "CANCEL" };

    // HQ Stretch
    juce::Label keyFrom, keyArrow, bpmFrom, bpmArrow, bpmTo, renderLabel;
    juce::ComboBox keyTo, tempoMode;
    juce::TextButton keyDown { "-" }, keyUp { "+" }, halfBtn { "/2" }, doubleBtn { "x2" }, tapBtn { "TAP" };
    std::vector<double> taps;
    juce::TextButton pitchTimeBtn { "HQ STRETCH" }, formantBtn { "FORMANT" };
    juce::Slider manualTempo { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> tempoModeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> pitchTimeAtt, formantAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> manualTempoAtt;

    ui::Waveform wave { proc };
    ui::Overview overview { proc, wave };

    // Stems
    juce::Label stemsTitle;
    juce::TextButton separateBtn { "SEPARATE STEMS" }, removeStemsBtn { "x" };
    std::array<StemStrip, chop::kMaxStems> stems;

    // Pads
    std::array<juce::TextButton, chop::kNumBanks> bankBtns;
    std::vector<std::unique_ptr<ui::PadButton>> padButtons;

    // Pad inspector
    juce::Label padTitle, padInfo;
    Knob padGain, padPan, padTape, padAttack, padRelease;
    juce::ComboBox padMode, padChoke;
    juce::TextButton padReverse { "REVERSE" }, padClear { "CLEAR" }, dragHint { "DRAG SLICE" };
    std::array<juce::TextButton, chop::kMaxStems> padStems;

    // Global
    Knob volume, filter, velocity;
    juce::TextButton monoBtn { "MONO" }, keysBtn { "KEYS" }, snapBtn { "SNAP" }, chopBtn { "CHOP" };
    juce::ComboBox quantize, chopMode, chopCount;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> monoAtt, keysAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> quantizeAtt;
    juce::Label quantizeLabel, chopLabel;

    std::unique_ptr<juce::FileChooser> chooser;
    bool fileHover = false;
    bool updatingInspector = false;
    juce::Rectangle<int> headerArea, ptArea, stemsArea, padsArea, inspectorArea, globalArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlayrChopEditor)
};
