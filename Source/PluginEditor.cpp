#include "PluginEditor.h"

using namespace chop;

namespace ui
{
juce::Font font (float size, bool bold)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Colour padColour (int pad)
{
    // 16 hues per bank, spread so neighbours never look alike.
    static const juce::uint32 hues[16] = { 0xffff4f8b, 0xffffb547, 0xff35e0c4, 0xff6c8cff, 0xffb06cff, 0xff4fd6ff,
                                           0xffff7a4f, 0xff9be35a, 0xffff5fd2, 0xfff2e15a, 0xff3fb8a0, 0xff8f7bff,
                                           0xffff9e9e, 0xff5ae3a0, 0xffe0a35a, 0xff7ab6ff };
    return juce::Colour (hues[pad % 16]);
}

juce::Colour stemColour (const juce::String& name, int index)
{
    const auto n = name.toLowerCase();
    if (n.contains ("vocal")) return juce::Colour (0xffff4f8b);
    if (n.contains ("drum")) return juce::Colour (0xffffb547);
    if (n.contains ("bass")) return juce::Colour (0xff6c8cff);
    if (n.contains ("guitar")) return juce::Colour (0xff9be35a);
    if (n.contains ("piano")) return juce::Colour (0xffb06cff);
    if (n.contains ("other") || n.contains ("melod")) return juce::Colour (0xff35e0c4);
    return padColour (index * 3);
}

static juce::String padLabel (int i)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + i / kPadsPerBank)) + juce::String (i % kPadsPerBank + 1);
}

// Computer keys for the 16 pads of the bank on screen: two piano-style rows.
static const char* const kPadKeys = "awsedftgyhujkol;";

static juce::String timeString (double s)
{
    const int m = (int) (s / 60.0);
    return juce::String (m) + ":" + juce::String (s - m * 60.0, 2).paddedLeft ('0', 5);
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Label::textWhenEditingColourId, text);
    setColour (juce::TextEditor::backgroundColourId, panel2);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.3f));
    setColour (juce::ComboBox::backgroundColourId, panel2);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::TextButton::buttonColourId, panel2);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, dim);
    setColour (juce::TextButton::textColourOnId, bg);
    setColour (juce::ProgressBar::backgroundColourId, panel2);
    setColour (juce::ProgressBar::foregroundColourId, accent);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                    float start, float end, juce::Slider& s)
{
    const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float ang = start + pos * (end - start);
    const float thick = juce::jmax (2.5f, r * 0.1f);
    const bool bipolar = s.getProperties().contains ("bipolar");
    // Bipolar knobs draw from their zero (not always the middle, e.g. -75..+300 %).
    const float zero = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 ? (float) s.valueToProportionOfLength (0.0) : 0.5f;
    const float from = bipolar ? start + zero * (end - start) : start;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, start, end, true);
    g.setColour (line);
    g.strokePath (track, { thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    juce::Path value;
    value.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, juce::jmin (from, ang), juce::jmax (from, ang), true);
    g.setGradientFill (juce::ColourGradient (accent, c.x - r, c.y, accent2, c.x + r, c.y, false));
    g.strokePath (value, { thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    const float kr = r - thick * 2.6f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a4252), c.x, c.y - kr,
                                             juce::Colour (0xff171a21), c.x, c.y + kr, false));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.25f, -kr + 2.5f, 2.5f, kr * 0.45f, 1.25f);
    g.setColour (text);
    g.fillPath (pointer, juce::AffineTransform::rotation (ang).translated (c.x, c.y));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.35f;
    auto on_ = b.getProperties().contains ("colour") ? juce::Colour ((juce::uint32) (juce::int64) b.getProperties()["colour"]) : accent;
    auto fill = on ? on_ : (down ? line.brighter (0.3f) : over ? line.brighter (0.15f) : panel2);
    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour ((on ? on_.brighter (0.3f) : line).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }
    const auto r = juce::Rectangle<float> ((float) x, y + h * 0.5f - 2.5f, (float) w, 5.0f);
    g.setColour (line);
    g.fillRoundedRectangle (r, 2.5f);
    const auto c = s.findColour (juce::Slider::trackColourId);
    g.setColour (c);
    g.fillRoundedRectangle (r.withRight (pos), 2.5f);
    g.setColour (text);
    g.fillEllipse (pos - 5.0f, y + h * 0.5f - 5.0f, 10.0f, 10.0f);
}

//==============================================================================
void Waveform::resetView()
{
    viewStart = 0.0;
    viewLen = proc.getSample() ? proc.getSample()->lengthSec() : 0.0;
    if (onViewChanged) onViewChanged();
    repaint();
}

double Waveform::timeAt (float x) const { return viewStart + (double) x / juce::jmax (1, getWidth()) * viewLen; }
float Waveform::xAt (double t) const { return (float) ((t - viewStart) / juce::jmax (1e-9, viewLen) * getWidth()); }

void Waveform::clampView()
{
    const auto* s = proc.getSample();
    if (s == nullptr)
        return;
    const double len = s->lengthSec();
    viewLen = juce::jlimit (juce::jmin (0.05, len), len, viewLen);
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, len - viewLen), viewStart);
}

void Waveform::zoomAround (double t, double factor)
{
    const double frac = (t - viewStart) / juce::jmax (1e-9, viewLen);
    viewLen *= factor;
    clampView();
    viewStart = t - frac * viewLen;
    clampView();
    if (onViewChanged) onViewChanged();
    repaint();
}

int Waveform::flagAt (juce::Point<float> p) const
{
    // A cue's flag, or its line anywhere down the waveform (within a few pixels).
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto pad = proc.getPad (i);
        if (! pad.hasCue())
            continue;
        const float x = xAt (pad.cue);
        const bool inBank = i / kPadsPerBank == proc.bank;
        float d = 1e9f;
        if (p.y <= 20.0f && p.x >= x - 2.0f && p.x <= x + 28.0f)
            d = std::abs (p.x - (x + 12.0f)) * 0.5f; // the flag
        else if (std::abs (p.x - x) <= 5.0f)
            d = std::abs (p.x - x);                   // the line
        if (! inBank)
            d += 3.0f; // prefer the bank on screen
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return best;
}

int Waveform::endHandleAt (juce::Point<float> p) const
{
    // The end handle of a selected pad: a small tab at the bottom of its slice.
    if (p.y < (float) getHeight() - 18.0f)
        return -1;
    for (int i : proc.getSelection())
    {
        if (! proc.getPad (i).hasCue())
            continue;
        const float x = xAt (proc.padEnd (i));
        if (p.x >= x - 14.0f && p.x <= x + 3.0f)
            return i;
    }
    return -1;
}

void Waveform::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (b, 6.0f);
    const auto* s = proc.getSample();
    if (s == nullptr || viewLen <= 0.0)
    {
        g.setColour (dim);
        g.setFont (font (15.0f));
        g.drawText (proc.isFileMissing() ? "Missing: " + proc.getMissingFile().getFullPathName() + "  (drop the file here to relink)"
                                          : "Drop a track here, or press LOAD",
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    const float h = b.getHeight();
    const float mid = 20.0f + (h - 20.0f) * 0.5f;
    const float amp = (h - 26.0f) * 0.5f;

    // Selected slices.
    const int sel = proc.selectedPad;
    const auto selection = proc.getSelection();
    for (int i : selection)
    {
        const auto p = proc.getPad (i);
        if (! p.hasCue())
            continue;
        const double endT = i == draggingEnd ? dragTime : proc.padEnd (i);
        const float x0 = xAt (i == draggingPad ? dragTime : p.cue), x1 = xAt (endT);
        g.setColour (padColour (i).withAlpha (i == sel ? 0.14f : 0.09f));
        g.fillRect (juce::Rectangle<float> (x0, 20.0f, x1 - x0, h - 20.0f));
    }

    // Beat grid: bars brighter.
    const double bpm = proc.getSampleBpm();
    if (bpm > 0)
    {
        const double beat = 60.0 / bpm;
        const double first = s->analysis.firstBeatSec;
        const double pxPerBeat = beat / viewLen * getWidth();
        if (pxPerBeat > 6.0)
        {
            const int k0 = (int) std::floor ((viewStart - first) / beat);
            for (int k = k0; first + k * beat < viewStart + viewLen; ++k)
            {
                const float x = xAt (first + k * beat);
                const bool bar = (k % 4) == 0;
                g.setColour (bar ? line.brighter (0.35f) : line.withAlpha (0.6f));
                g.drawVerticalLine ((int) x, 20.0f, h);
            }
        }
    }

    // Waveform, coloured by band energy (low = pink, mid = teal, high = white).
    const auto& ov = s->overview;
    const int buckets = (int) ov.mx.size();
    const double len = s->lengthSec();
    const auto& L = s->original->l;
    const auto& R = s->original->r;
    const int total = s->original->size();
    for (int x = 0; x < getWidth(); ++x)
    {
        const double t0 = timeAt ((float) x), t1 = timeAt ((float) x + 1.0f);
        if (t1 < 0 || t0 > len)
            continue;
        const int bk = juce::jlimit (0, buckets - 1, (int) (t0 / len * buckets));
        float mn, mx;
        const double samplesPerPx = (t1 - t0) * s->fileRate;
        if (samplesPerPx * buckets > total * 1.0)
        {
            const int bk1 = juce::jlimit (bk, buckets - 1, (int) (t1 / len * buckets));
            mn = 0, mx = 0;
            for (int k = bk; k <= bk1; ++k)
            {
                mn = juce::jmin (mn, ov.mn[(size_t) k]);
                mx = juce::jmax (mx, ov.mx[(size_t) k]);
            }
        }
        else
        {
            const int a = juce::jlimit (0, total - 1, (int) (t0 * s->fileRate));
            const int e = juce::jlimit (a + 1, total, (int) (t1 * s->fileRate) + 1);
            mn = 1.0f, mx = -1.0f;
            const int step = juce::jmax (1, (e - a) / 64);
            for (int i = a; i < e; i += step)
            {
                const float v = 0.5f * (L[(size_t) i] + R[(size_t) i]);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
        }
        const float lo = ov.low[(size_t) bk], md = ov.mid[(size_t) bk], hi = ov.high[(size_t) bk];
        const float sum = juce::jmax (1e-4f, lo + md + hi);
        auto c = accent2.withMultipliedBrightness (1.0f) .interpolatedWith (accent, md / sum).interpolatedWith (juce::Colours::white, hi / sum * 0.7f);
        g.setColour (c.withAlpha (0.9f));
        const float y0 = mid - juce::jlimit (0.0f, 1.0f, mx) * amp, y1 = mid - juce::jlimit (-1.0f, 0.0f, mn) * amp;
        g.fillRect ((float) x, y0, 1.0f, juce::jmax (1.0f, y1 - y0));
    }

    // Cue flags: the visible bank on top and bright, other banks faint.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < kNumPads; ++i)
        {
            const bool inBank = i / kPadsPerBank == proc.bank;
            if (inBank != (pass == 1))
                continue;
            auto pad = proc.getPad (i);
            if (i == draggingPad)
                pad.cue = dragTime;
            if (! pad.hasCue())
                continue;
            const float x = xAt (pad.cue);
            if (x < -30 || x > getWidth() + 2)
                continue;
            const auto c = padColour (i).withAlpha (inBank ? 1.0f : 0.35f);
            g.setColour (c);
            g.fillRect (x, 0.0f, i == sel ? 2.0f : 1.0f, h);
            const juce::Rectangle<float> flag (x, 2.0f, 26.0f, 16.0f);
            g.fillRoundedRectangle (flag, 3.0f);
            if (i == hoverFlag || i == sel)
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (flag, 3.0f, 1.0f);
            }
            g.setColour (bg);
            g.setFont (font (11.0f, true));
            g.drawText (padLabel (i), flag, juce::Justification::centred);
        }

    // End handles of the selected pads (drag to set where they stop).
    for (int i : selection)
    {
        if (! proc.getPad (i).hasCue())
            continue;
        const float x = xAt (i == draggingEnd ? dragTime : proc.padEnd (i));
        juce::Path tab;
        tab.addTriangle (x, h - 16.0f, x, h, x - 12.0f, h);
        g.setColour (padColour (i).withAlpha (proc.getPad (i).end >= 0.0 ? 1.0f : 0.55f));
        g.fillPath (tab);
        g.fillRect (x - 1.0f, 20.0f, 1.0f, h - 20.0f);
    }

    // The track's own playhead (the transport), then the pads' playheads.
    {
        const float x = xAt (proc.getPlayhead());
        g.setColour (warn);
        g.fillRect (x - 1.0f, 0.0f, 2.0f, h);
        juce::Path head;
        head.addTriangle (x - 6.0f, 0.0f, x + 6.0f, 0.0f, x, 8.0f);
        g.fillPath (head);
    }
    for (const auto& v : proc.sampler.views)
    {
        if (v.pad.load() < 0)
            continue;
        const float x = xAt (v.sourceSec.load());
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillRect (x - 0.5f, 20.0f, 1.5f, h - 20.0f);
    }

    g.setColour (dim);
    g.setFont (font (11.0f));
    g.drawText (timeString (viewStart) + " - " + timeString (viewStart + viewLen),
                getLocalBounds().removeFromBottom (16).reduced (6, 0), juce::Justification::bottomRight);
}

void Waveform::mouseMove (const juce::MouseEvent& e)
{
    const int f = flagAt (e.position);
    const int h = endHandleAt (e.position);
    if (f != hoverFlag)
    {
        hoverFlag = f;
        repaint();
    }
    setMouseCursor (f >= 0 || h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void Waveform::mouseDown (const juce::MouseEvent& e)
{
    if (proc.getSample() == nullptr)
        return;
    const double t = timeAt (e.position.x);
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        const int sel = proc.selectedPad;
        const int empty = proc.firstEmptyPad (proc.bank);
        m.addItem (1, empty >= 0 ? "Add cue here (" + padLabel (empty) + ")" : "Add cue (all pads used)", empty >= 0);
        m.addItem (2, "Move " + padLabel (sel) + " cue here");
        m.addItem (3, "Set " + padLabel (sel) + " end here", proc.getPad (sel).hasCue() && t > proc.getPad (sel).cue);
        m.addItem (4, "Clear " + padLabel (sel) + " end", proc.getPad (sel).end >= 0.0);
        m.addItem (6, "Delete " + padLabel (sel), proc.getPad (sel).hasCue());
        m.addSeparator();
        m.addItem (5, "Zoom to fit");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this, t, sel, empty] (int r) {
            if (r == 1 && empty >= 0) { proc.setCue (empty, t); proc.select (empty, false); }
            if (r == 2) proc.setCue (sel, t);
            if (r == 3) proc.setEnd (sel, t);
            if (r == 4) proc.setEnd (sel, -1.0);
            if (r == 5) resetView();
            if (r == 6) proc.deletePads ({ sel });
            if (onSelectionChanged) onSelectionChanged();
            repaint();
        });
        return;
    }

    dragViewStart = viewStart;
    downTime = t;
    movedSinceDown = false;
    creating = false;
    draggingEnd = endHandleAt (e.position);
    draggingPad = draggingEnd >= 0 ? -1 : flagAt (e.position);
    if (draggingEnd >= 0)
    {
        dragTime = proc.padEnd (draggingEnd);
        return;
    }
    if (draggingPad >= 0)
    {
        dragTime = proc.getPad (draggingPad).cue;
        if (! proc.isSelected (draggingPad) || ! e.mods.isShiftDown())
            proc.select (draggingPad, e.mods.isShiftDown());
        proc.bank = draggingPad / kPadsPerBank;
        if (onSelectionChanged) onSelectionChanged();
    }
}

void Waveform::mouseDrag (const juce::MouseEvent& e)
{
    if (proc.getSample() == nullptr || e.mods.isPopupMenu())
        return;
    if (e.getDistanceFromDragStart() > 3)
        movedSinceDown = true;
    const double t = juce::jlimit (0.0, proc.getSample()->lengthSec(), timeAt (e.position.x));

    if (draggingEnd >= 0 || draggingPad >= 0)
    {
        dragTime = t;
        repaint();
        return;
    }
    // Dragging empty waveform pulls out a new cue on the next free pad.
    if (movedSinceDown && ! creating)
    {
        const int empty = proc.firstEmptyPad (proc.bank);
        if (empty >= 0)
        {
            creating = true;
            draggingPad = empty;
            proc.setCue (empty, downTime);
            proc.select (empty, false);
            proc.bank = empty / kPadsPerBank;
            dragTime = t;
            if (onSelectionChanged) onSelectionChanged();
        }
    }
}

void Waveform::mouseUp (const juce::MouseEvent& e)
{
    if (draggingEnd >= 0)
    {
        proc.setEnd (draggingEnd, dragTime);
        draggingEnd = -1;
    }
    else if (draggingPad >= 0)
    {
        if (movedSinceDown || creating)
            proc.setCue (draggingPad, dragTime);
        draggingPad = -1;
    }
    else if (! movedSinceDown && proc.getSample() != nullptr && ! e.mods.isPopupMenu())
    {
        // A plain click moves the playhead there (and the music, if it's playing).
        proc.setPlayhead (downTime);
        const int p = proc.padAtTime (downTime);
        if (p >= 0 && ! e.mods.isShiftDown())
        {
            proc.select (p, false);
            proc.bank = p / kPadsPerBank;
        }
        if (onSelectionChanged) onSelectionChanged();
    }
    creating = false;
    repaint();
}

void Waveform::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click: a new cue on the next free pad.
    if (proc.getSample() == nullptr || flagAt (e.position) >= 0)
        return;
    const int empty = proc.firstEmptyPad (proc.bank);
    if (empty < 0)
        return;
    proc.setCue (empty, timeAt (e.position.x));
    proc.select (empty, false);
    proc.bank = empty / kPadsPerBank;
    if (onSelectionChanged) onSelectionChanged();
}

void Waveform::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (proc.getSample() == nullptr)
        return;
    if (std::abs (w.deltaX) > std::abs (w.deltaY) || e.mods.isShiftDown())
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        viewStart -= d * viewLen * 0.5;
        clampView();
        if (onViewChanged) onViewChanged();
        repaint();
        return;
    }
    zoomAround (timeAt (e.position.x), std::pow (0.5, w.deltaY * 2.0));
}

void Waveform::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    if (proc.getSample() != nullptr && scale > 0.0f)
        zoomAround (timeAt (e.position.x), 1.0 / scale);
}

//==============================================================================
void Overview::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (b, 4.0f);
    const auto* s = proc.getSample();
    if (s == nullptr)
        return;
    const auto& ov = s->overview;
    const int n = (int) ov.mx.size();
    const float mid = b.getCentreY(), amp = b.getHeight() * 0.45f;
    g.setColour (dim.withAlpha (0.8f));
    for (int x = 0; x < getWidth(); ++x)
    {
        const int a = x * n / getWidth(), e = juce::jmax (a + 1, (x + 1) * n / getWidth());
        float mn = 0, mx = 0;
        for (int k = a; k < e && k < n; ++k)
        {
            mn = juce::jmin (mn, ov.mn[(size_t) k]);
            mx = juce::jmax (mx, ov.mx[(size_t) k]);
        }
        g.fillRect ((float) x, mid - mx * amp, 1.0f, juce::jmax (1.0f, (mx - mn) * amp));
    }
    const double len = s->lengthSec();
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto p = proc.getPad (i);
        if (! p.hasCue())
            continue;
        g.setColour (padColour (i).withAlpha (i / kPadsPerBank == proc.bank ? 1.0f : 0.3f));
        g.fillRect ((float) (p.cue / len * getWidth()), 0.0f, 1.0f, b.getHeight());
    }
    const float x0 = (float) (wave.viewStart / len * getWidth()), w = (float) (wave.viewLen / len * getWidth());
    g.setColour (accent.withAlpha (0.12f));
    g.fillRect (x0, 0.0f, w, b.getHeight());
    g.setColour (accent.withAlpha (0.7f));
    g.drawRect (x0, 0.0f, juce::jmax (2.0f, w), b.getHeight(), 1.0f);
    for (const auto& v : proc.sampler.views)
        if (v.pad.load() >= 0)
        {
            g.setColour (juce::Colours::white);
            g.fillRect ((float) (v.sourceSec.load() / len * getWidth()), 0.0f, 1.0f, b.getHeight());
        }
}

void Overview::moveTo (float x)
{
    const auto* s = proc.getSample();
    if (s == nullptr)
        return;
    wave.viewStart = x / juce::jmax (1, getWidth()) * s->lengthSec() - wave.viewLen * 0.5;
    wave.viewStart = juce::jlimit (0.0, juce::jmax (0.0, s->lengthSec() - wave.viewLen), wave.viewStart);
    wave.repaint();
    repaint();
}

//==============================================================================
void PadButton::paint (juce::Graphics& g)
{
    const int i = index();
    const auto p = proc.getPad (i);
    const auto b = getLocalBounds().toFloat().reduced (3.0f);
    const auto c = padColour (i);
    const bool primary = proc.selectedPad == i;
    const bool selected = proc.isSelected (i) || primary;

    if (p.hasCue())
    {
        g.setGradientFill (juce::ColourGradient (c.withAlpha (0.28f + 0.6f * level), b.getX(), b.getY(),
                                                 c.withAlpha (0.12f + 0.5f * level), b.getRight(), b.getBottom(), false));
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (c.withAlpha (0.9f));
        g.fillRoundedRectangle (b.withHeight (4.0f), 2.0f);
    }
    else
    {
        g.setColour (panel2);
        g.fillRoundedRectangle (b, 8.0f);
    }
    if (dropHover == i)
    {
        g.setColour (accent.withAlpha (0.25f));
        g.fillRoundedRectangle (b, 8.0f);
    }
    g.setColour (selected ? juce::Colours::white.withAlpha (primary ? 1.0f : 0.7f) : (p.hasCue() ? c.withAlpha (0.5f) : line));
    g.drawRoundedRectangle (b, 8.0f, selected ? 2.0f : 1.0f);

    auto inner = b.reduced (10, 8);
    g.setColour (p.hasCue() ? text : dim);
    g.setFont (font (15.0f, true));
    g.drawText (padLabel (i), inner, juce::Justification::topLeft);
    g.setFont (font (11.0f, true));
    g.setColour (dim);
    const juce::String keyName = juce::String::charToString ((juce::juce_wchar) kPadKeys[i % kPadsPerBank]).toUpperCase();
    g.drawText (keyName + "  " + juce::MidiMessage::getMidiNoteName (kFirstPadNote + i, true, true, 3), inner,
                juce::Justification::topRight);
    if (p.favorite)
    {
        g.setColour (warn);
        g.drawText (juce::String::fromUTF8 ("\xe2\x98\x85"), inner.withTrimmedTop (16), juce::Justification::topRight);
    }
    g.setFont (font (11.0f));
    g.setColour (dim);
    if (p.hasCue())
    {
        const double len = proc.padEnd (i) - p.cue;
        juce::String info = juce::String (len, 2) + "s";
        if (std::abs (p.keyShift) > 1e-3f) info << "  " << (p.keyShift > 0 ? "+" : "") << juce::String (p.keyShift, 0) << "st";
        if (std::abs (p.stretch) > 1e-3f) info << "  " << (p.stretch > 0 ? "+" : "") << juce::String (p.stretch, 0) << "%";
        if (p.mode == PadMode::Gate) info << "  GATE";
        if (p.mode == PadMode::Loop) info << "  LOOP";
        if (p.reverse) info << "  REV";
        if (p.stemMask != 0xffffffffu && proc.numStems() > 0) info << "  STEMS";
        g.drawText (info, inner, juce::Justification::bottomLeft);
    }
    else
    {
        g.drawText ("set cue", inner, juce::Justification::bottomLeft);
    }
}

void PadButton::mouseDown (const juce::MouseEvent& e)
{
    const int i = index();
    dragging = false;
    dropHover = -1;
    if (e.mods.isPopupMenu())
    {
        if (! proc.isSelected (i))
            proc.select (i, false);
        if (onSelect) onSelect();
        juce::PopupMenu m;
        const auto sel = proc.getSelection();
        const juce::String what = sel.size() > 1 ? juce::String ((int) sel.size()) + " pads" : padLabel (i);
        m.addItem (1, "Delete " + what);
        m.addItem (2, proc.getPad (i).favorite ? "Remove favorite" : "Make favorite");
        m.addItem (3, "Copy " + what + " to the next free pads");
        m.addItem (4, "Spread " + padLabel (i) + " across the bank in key (-7..+8)", proc.getPad (i).hasCue());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this, i, sel] (int r) {
            if (r == 1) proc.deletePads (sel);
            if (r == 2)
                for (int k : sel) { auto p = proc.getPad (k); p.favorite = ! proc.getPad (i).favorite; proc.setPad (k, p); }
            if (r == 3)
            {
                const int to = proc.firstEmptyPad (proc.bank);
                if (to >= 0) proc.movePads (sel, to, true);
            }
            if (r == 4) proc.keyShiftPad (i, proc.bank * kPadsPerBank);
            if (onSelect) onSelect();
        });
        return;
    }
    if (e.mods.isShiftDown() || e.mods.isCommandDown())
    {
        proc.select (i, true); // add to / remove from the selection, no sound
        if (onSelect) onSelect();
        return;
    }
    // Keep a multi-selection when grabbing one of its pads (to drag them all); a click without
    // a drag selects just this pad on release.
    keepSelection = proc.isSelected (i) && proc.getSelection().size() > 1;
    if (! keepSelection)
        proc.select (i, false);
    if (onSelect) onSelect();

    if (! proc.getPad (i).hasCue())
    {
        proc.setCueAtPlayhead (i); // empty pad: drop a cue where the playhead is
        if (onSelect) onSelect();
        return;
    }
    const bool keys = proc.apvts.getRawParameterValue ("keys")->load() > 0.5f;
    playingNote = keys ? kKeysRootNote : kFirstPadNote + i;
    proc.triggerFromUI (playingNote, true);
}

void PadButton::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.mods.isPopupMenu() || ! proc.getPad (index()).hasCue() || e.getDistanceFromDragStart() < 5)
        return;
    if (playingNote >= 0)
    {
        proc.triggerFromUI (playingNote, false);
        playingNote = -1;
    }
    const int over = padAtScreen ? padAtScreen (e.getScreenPosition()) : -1;
    if (over < 0)
    {
        // Out of the pad grid: hand the slices to the DAW (or Finder).
        dragging = true;
        dropHover = -1;
        if (onDragOut) onDragOut (e);
        return;
    }
    if (over != dropHover)
    {
        dropHover = over;
        if (auto* parent = getParentComponent())
            parent->repaint();
    }
}

void PadButton::mouseUp (const juce::MouseEvent& e)
{
    if (playingNote >= 0)
        proc.triggerFromUI (playingNote, false);
    playingNote = -1;
    const int i = index();
    if (! dragging && dropHover >= 0 && dropHover != i && onDropOnPad)
        onDropOnPad (dropHover, e.mods.isAltDown()); // Option-drag copies, a plain drag moves
    else if (! dragging && keepSelection && e.getDistanceFromDragStart() < 5)
        proc.select (i, false);
    keepSelection = false;
    dropHover = -1;
    dragging = false;
    if (onSelect) onSelect();
}

} // namespace ui

//==============================================================================
FlayrChopEditor::FlayrChopEditor (FlayrChopProcessor& owner) : AudioProcessorEditor (owner), proc (owner)
{
    setLookAndFeel (&lnf);
    auto& s = proc.apvts;

    auto styleLabel = [] (juce::Label& l, float size, bool bold, juce::Colour c, juce::Justification j = juce::Justification::centredLeft) {
        l.setFont (ui::font (size, bold));
        l.setColour (juce::Label::textColourId, c);
        l.setJustificationType (j);
    };

    // Header
    addAndMakeVisible (loadBtn);
    loadBtn.onClick = [this] { choosFile(); };
    addAndMakeVisible (exportBtn);
    exportBtn.onClick = [this] { showExportMenu(); };
    addAndMakeVisible (fileLabel);
    styleLabel (fileLabel, 16.0f, true, ui::text);
    addAndMakeVisible (statusLabel);
    styleLabel (statusLabel, 12.0f, false, ui::dim, juce::Justification::centredRight);
    addChildComponent (progressBar);
    addChildComponent (cancelBtn);
    cancelBtn.onClick = [this] { proc.cancelStems(); };

    // HQ Stretch
    for (auto* l : { &keyFrom, &keyArrow, &bpmFrom, &bpmArrow, &bpmTo, &renderLabel })
        addAndMakeVisible (*l);
    styleLabel (keyFrom, 18.0f, true, ui::text, juce::Justification::centred);
    styleLabel (bpmFrom, 18.0f, true, ui::text, juce::Justification::centred);
    styleLabel (bpmTo, 18.0f, true, ui::accent, juce::Justification::centred);
    styleLabel (keyArrow, 16.0f, false, ui::dim, juce::Justification::centred);
    styleLabel (bpmArrow, 16.0f, false, ui::dim, juce::Justification::centred);
    styleLabel (renderLabel, 11.0f, false, ui::warn, juce::Justification::centredLeft);
    keyArrow.setText (juce::String::fromUTF8 ("\xe2\x86\x92"), juce::dontSendNotification);
    bpmArrow.setText (juce::String::fromUTF8 ("\xe2\x86\x92"), juce::dontSendNotification);
    bpmFrom.setEditable (false, false); // "Type a value..." in its menu opens the editor
    bpmFrom.setTooltip ("The sample's tempo. Click for alternatives or to type it.");
    bpmFrom.addMouseListener (this, false);
    bpmFrom.onTextChange = [this] {
        const double v = bpmFrom.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (v > 20.0) proc.setSampleBpm (v);
        refreshAll();
    };
    keyFrom.setTooltip ("The sample's key. Click to correct it.");
    keyFrom.addMouseListener (this, false);

    addAndMakeVisible (keyTo);
    keyTo.onChange = [this] { if (keyTo.getSelectedId() > 0) setTargetKey (keyTo.getSelectedId() - 1); };
    for (auto* b : { &keyDown, &keyUp, &halfBtn, &doubleBtn, &tapBtn })
        addAndMakeVisible (*b);
    keyDown.onClick = [this] {
        auto* prm = proc.apvts.getParameter ("keyShift");
        prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jmax (-12.0f, prm->convertFrom0to1 (prm->getValue()) - 1.0f)));
        refreshAll();
    };
    keyUp.onClick = [this] {
        auto* prm = proc.apvts.getParameter ("keyShift");
        prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jmin (12.0f, prm->convertFrom0to1 (prm->getValue()) + 1.0f)));
        refreshAll();
    };
    halfBtn.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 0.5); refreshAll(); };
    doubleBtn.onClick = [this] { proc.setSampleBpm (proc.getSampleBpm() * 2.0); refreshAll(); };
    tapBtn.setTooltip ("Tap along to the sample's beat to set its tempo");
    tapBtn.onClick = [this] { tap(); };
    halfBtn.setTooltip ("The sample's BPM was detected double: halve it");
    doubleBtn.setTooltip ("The sample's BPM was detected half: double it");

    addAndMakeVisible (pitchTimeBtn);
    pitchTimeBtn.setClickingTogglesState (true);
    pitchTimeBtn.setTooltip ("On: key and tempo change independently with high-quality stretching. Off: tape style (speed and pitch move together).");
    pitchTimeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, "stretch", pitchTimeBtn);
    addAndMakeVisible (formantBtn);
    formantBtn.setClickingTogglesState (true);
    formantBtn.setTooltip ("Keep vocal character when shifting key (no chipmunk effect)");
    formantAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, "formant", formantBtn);

    addAndMakeVisible (tempoMode);
    tempoMode.addItemList ({ "Original", "Host Sync", "Manual" }, 1);
    tempoModeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (s, "tempoMode", tempoMode);
    tempoMode.onChange = [this] { refreshAll(); };
    addChildComponent (manualTempo);
    manualTempo.setColour (juce::Slider::textBoxTextColourId, ui::accent);
    manualTempoAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, "tempo", manualTempo);

    // Transport and cue tools above the waveform.
    for (auto* b : { &playBtn, &zoomOutBtn, &zoomInBtn, &nudgeLeftBtn, &nudgeRightBtn, &selectAllBtn, &deleteBtn, &clearAllBtn })
        addAndMakeVisible (*b);
    playBtn.setTooltip ("Play the track from the playhead (space). Hit an empty pad while it plays to drop a cue there.");
    playBtn.onClick = [this] { togglePlay(); };
    zoomInBtn.onClick = [this] { wave.zoomAround (proc.getPlayhead(), 0.5); };
    zoomOutBtn.onClick = [this] { wave.zoomAround (proc.getPlayhead(), 2.0); };
    nudgeLeftBtn.setTooltip ("Nudge the selected cues earlier (shift: by a 16th note; arrow keys work too)");
    nudgeRightBtn.setTooltip ("Nudge the selected cues later (shift: by a 16th note; arrow keys work too)");
    nudgeLeftBtn.onClick = [this] { nudgeSelection (-1, juce::ModifierKeys::currentModifiers.isShiftDown()); };
    nudgeRightBtn.onClick = [this] { nudgeSelection (1, juce::ModifierKeys::currentModifiers.isShiftDown()); };
    nudgeLeftBtn.setRepeatSpeed (350, 60);
    nudgeRightBtn.setRepeatSpeed (350, 60);
    selectAllBtn.setTooltip ("Select every pad in this bank (shift-click pads to pick several)");
    selectAllBtn.onClick = [this] { proc.selectAll (proc.bank); refreshAll(); };
    deleteBtn.setTooltip ("Delete the selected cues (backspace)");
    deleteBtn.onClick = [this] { deleteSelection(); };
    clearAllBtn.setTooltip ("Clear every pad");
    clearAllBtn.onClick = [this] {
        juce::PopupMenu m;
        m.addItem (1, "Clear this bank");
        m.addItem (2, "Clear all 4 banks");
        m.addItem (3, "Clear all, except favorites");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (clearAllBtn), [this] (int r) {
            std::vector<int> which;
            for (int i = 0; i < kNumPads; ++i)
                if ((r == 1 && i / kPadsPerBank == proc.bank) || r == 2 || (r == 3 && ! proc.getPad (i).favorite))
                    which.push_back (i);
            if (r > 0)
                proc.deletePads (which);
            refreshAll();
        });
    };
    addAndMakeVisible (timeLabel);
    timeLabel.setFont (ui::font (12.0f, true));
    timeLabel.setColour (juce::Label::textColourId, ui::warn);

    addAndMakeVisible (wave);
    addAndMakeVisible (overview);
    wave.onViewChanged = [this] { overview.repaint(); };
    wave.onSelectionChanged = [this] { refreshAll(); };

    // Stems
    addAndMakeVisible (stemsTitle);
    styleLabel (stemsTitle, 12.0f, true, ui::dim);
    stemsTitle.setText ("STEMS", juce::dontSendNotification);
    addAndMakeVisible (separateBtn);
    separateBtn.onClick = [this] { proc.separateStems(); };
    separateBtn.setTooltip ("Split into vocals, drums, bass and more, on this computer (nothing is uploaded)");
    addChildComponent (removeStemsBtn);
    removeStemsBtn.setTooltip ("Remove the stems for this track");
    removeStemsBtn.onClick = [this] { proc.removeStems(); };
    for (int i = 0; i < kMaxStems; ++i)
    {
        auto& st = stems[(size_t) i];
        addChildComponent (st.name);
        styleLabel (st.name, 12.0f, true, ui::text);
        addChildComponent (st.level);
        addChildComponent (st.mute);
        addChildComponent (st.solo);
        st.mute.setClickingTogglesState (true);
        st.solo.setClickingTogglesState (true);
        st.mute.getProperties().set ("colour", (juce::int64) ui::accent2.getARGB());
        st.solo.getProperties().set ("colour", (juce::int64) ui::warn.getARGB());
        st.levelAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, FlayrChopProcessor::stemLevelId (i), st.level);
        st.muteAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, FlayrChopProcessor::stemMuteId (i), st.mute);
        st.soloAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, FlayrChopProcessor::stemSoloId (i), st.solo);
    }

    // Pads
    for (int b = 0; b < kNumBanks; ++b)
    {
        auto& btn = bankBtns[(size_t) b];
        btn.setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + b)));
        btn.setRadioGroupId (101);
        btn.setClickingTogglesState (true);
        btn.onClick = [this, b] { if (bankBtns[(size_t) b].getToggleState()) { proc.bank = b; refreshAll(); } };
        addAndMakeVisible (btn);
    }
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        auto pb = std::make_unique<ui::PadButton> (proc, i);
        pb->onSelect = [this] { refreshAll(); };
        pb->padAtScreen = [this] (juce::Point<int> screen) {
            const auto local = getLocalPoint (nullptr, screen);
            for (auto& other : padButtons)
                if (other->getBounds().contains (local))
                    return other->index();
            return -1;
        };
        pb->onDropOnPad = [this, raw = pb.get()] (int to, bool copy) {
            const int from = raw->index();
            auto sel = proc.getSelection();
            if (std::find (sel.begin(), sel.end(), from) == sel.end())
                sel = { from };
            // The grabbed pad lands on the target; the rest keep their places relative to it.
            proc.movePads (sel, to - (from - sel.front()), copy);
            refreshAll();
        };
        pb->onDragOut = [this] (const juce::MouseEvent& e) { exportDrag (e); };
        addAndMakeVisible (*pb);
        padButtons.push_back (std::move (pb));
    }

    // Pad inspector
    addAndMakeVisible (padTitle);
    styleLabel (padTitle, 18.0f, true, ui::text);
    addAndMakeVisible (padInfo);
    styleLabel (padInfo, 12.0f, false, ui::dim);
    setupKnob (padGain, "LEVEL");
    setupKnob (padPan, "PAN");
    setupKnob (padFilter, "FILTER");
    setupKnob (padAttack, "ATTACK");
    setupKnob (padRelease, "RELEASE");
    setupKnob (padKey, "KEY SHIFT");
    setupKnob (padStretch, "STRETCH");
    padFilter.slider.setRange (-1.0, 1.0, 0.01);
    padFilter.slider.getProperties().set ("bipolar", true);
    padFilter.slider.setTooltip ("Left: low-pass. Right: high-pass.");
    padFilter.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.filter = (float) padFilter.slider.getValue(); }); };
    padKey.slider.setRange (-24.0, 24.0, 1.0);
    padKey.slider.setTextValueSuffix (" st");
    padKey.slider.getProperties().set ("bipolar", true);
    padKey.slider.setTooltip ("Change this pad's key, keeping its length (HQ Stretch)");
    padKey.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.keyShift = (float) padKey.slider.getValue(); }); };
    padStretch.slider.setRange (-75.0, 300.0, 1.0);
    padStretch.slider.setTextValueSuffix (" %");
    padStretch.slider.getProperties().set ("bipolar", true);
    padStretch.slider.setTooltip ("Make this pad longer or shorter, keeping its pitch");
    padStretch.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.stretch = (float) padStretch.slider.getValue(); }); };
    for (auto* k : { &padGain, &padPan, &padFilter, &padKey, &padStretch })
        k->slider.setDoubleClickReturnValue (true, 0.0);
    padGain.slider.setRange (-24.0, 12.0, 0.1);
    padGain.slider.setTextValueSuffix (" dB");
    padPan.slider.setRange (-1.0, 1.0, 0.01);
    padPan.slider.getProperties().set ("bipolar", true);
    padAttack.slider.setRange (0.0, 2000.0, 0.1);
    padAttack.slider.setSkewFactorFromMidPoint (60.0);
    padAttack.slider.setTextValueSuffix (" ms");
    padRelease.slider.setRange (1.0, 4000.0, 0.1);
    padRelease.slider.setSkewFactorFromMidPoint (150.0);
    padRelease.slider.setTextValueSuffix (" ms");
    padGain.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.gainDb = (float) padGain.slider.getValue(); }); };
    padPan.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.pan = (float) padPan.slider.getValue(); }); };
    padAttack.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.attackMs = (float) padAttack.slider.getValue(); }); };
    padRelease.slider.onValueChange = [this] { editPad ([this] (Pad& p) { p.releaseMs = (float) padRelease.slider.getValue(); }); };

    addAndMakeVisible (padMode);
    padMode.addItemList ({ "Trigger", "Gate", "Loop" }, 1);
    padMode.onChange = [this] { editPad ([this] (Pad& p) { p.mode = (PadMode) (padMode.getSelectedId() - 1); }); };
    addAndMakeVisible (padChoke);
    padChoke.addItem ("No choke", 1);
    for (int c = 1; c <= 8; ++c)
        padChoke.addItem ("Choke " + juce::String (c), c + 1);
    padChoke.onChange = [this] { editPad ([this] (Pad& p) { p.choke = padChoke.getSelectedId() - 1; }); };
    addAndMakeVisible (padReverse);
    padReverse.setClickingTogglesState (true);
    padReverse.onClick = [this] { editPad ([this] (Pad& p) { p.reverse = padReverse.getToggleState(); }); };
    addAndMakeVisible (padClear);
    padClear.onClick = [this] { editPad ([] (Pad& p) { p = Pad {}; }); };
    addAndMakeVisible (padFavorite);
    padFavorite.setClickingTogglesState (true);
    padFavorite.getProperties().set ("colour", (juce::int64) ui::warn.getARGB());
    padFavorite.setTooltip ("Favorites are kept when you auto-chop again");
    padFavorite.onClick = [this] { editPad ([this] (Pad& p) { p.favorite = padFavorite.getToggleState(); }); };
    addAndMakeVisible (dragHint);
    dragHint.setEnabled (false);
    dragHint.setButtonText ("DRAG A PAD OUT TO EXPORT IT");
    for (int i = 0; i < kMaxStems; ++i)
    {
        auto& b = padStems[(size_t) i];
        b.setClickingTogglesState (true);
        b.onClick = [this, i] {
            editPad ([this, i] (Pad& p) {
                if (padStems[(size_t) i].getToggleState()) p.stemMask |= (1u << i);
                else p.stemMask &= ~(1u << i);
            });
        };
        addChildComponent (b);
    }

    // Global
    setupKnob (volume, "VOLUME");
    attachKnob (volume, "volume");
    setupKnob (filter, "FILTER");
    attachKnob (filter, "filter");
    filter.slider.getProperties().set ("bipolar", true);
    filter.slider.setTooltip ("Left: low-pass. Right: high-pass.");
    setupKnob (velocity, "VELOCITY");
    attachKnob (velocity, "velocity");
    addAndMakeVisible (monoBtn);
    monoBtn.setClickingTogglesState (true);
    monoBtn.setTooltip ("One pad at a time");
    monoAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, "mono", monoBtn);
    addAndMakeVisible (keysBtn);
    keysBtn.setClickingTogglesState (true);
    keysBtn.setTooltip ("Play the selected pad across the keyboard (C3 = original). HQ Stretch keeps every note the same length.");
    keysAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (s, "keys", keysBtn);
    addAndMakeVisible (quantize);
    quantize.addItemList ({ "Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar" }, 1);
    quantizeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (s, "quantize", quantize);
    addAndMakeVisible (quantizeLabel);
    styleLabel (quantizeLabel, 11.0f, true, ui::dim);
    quantizeLabel.setText ("QUANTIZE", juce::dontSendNotification);

    addAndMakeVisible (chopLabel);
    styleLabel (chopLabel, 11.0f, true, ui::dim);
    chopLabel.setText ("AUTO-CHOP", juce::dontSendNotification);
    addAndMakeVisible (chopMode);
    chopMode.addItemList ({ "Find samples", "Every beat", "Every 2 beats", "Every bar", "Even", "Slicer from playhead",
                            "Random", "Key shift pad" }, 1);
    chopMode.onChange = [this] { proc.chopModeUI = chopMode.getSelectedId() - 1; resized(); };
    addChildComponent (chopSpacing);
    for (auto [name, beats] : std::initializer_list<std::pair<const char*, int>> { { "1/16", 1 }, { "1/8", 2 }, { "1/4 (beat)", 4 },
                                                                                   { "1/2 bar", 8 }, { "1 bar", 16 }, { "2 bars", 32 }, { "4 bars", 64 } })
        chopSpacing.addItem (juce::String ("Every ") + name, beats); // ids are in 16th notes
    chopSpacing.setSelectedId (4, juce::dontSendNotification);
    addAndMakeVisible (chopCount);
    for (int c : { 4, 8, 16, 32, 64 })
        chopCount.addItem (juce::String (c) + " pads", c);
    chopCount.onChange = [this] { proc.chopCountUI = chopCount.getSelectedId(); };
    addAndMakeVisible (chopBtn);
    chopBtn.setTooltip ("Re-chop starting at the current bank");
    chopBtn.onClick = [this] { runChop(); };
    addAndMakeVisible (snapBtn);
    snapBtn.setClickingTogglesState (true);
    snapBtn.setTooltip ("Moved cues snap to the nearest transient");
    snapBtn.onClick = [this] { proc.snapUI = snapBtn.getToggleState(); };

    proc.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (1000, 700, 2000, 1400);
    setWantsKeyboardFocus (true); // pads on the computer keyboard, space to play, arrows to nudge
    setSize (1180, 780);
    refreshAll();
    wave.resetView();
    startTimerHz (30);
}

FlayrChopEditor::~FlayrChopEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void FlayrChopEditor::setupKnob (Knob& k, const juce::String& name)
{
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    k.label.setText (name, juce::dontSendNotification);
    k.label.setFont (ui::font (10.5f, true));
    k.label.setColour (juce::Label::textColourId, ui::dim);
    k.label.setJustificationType (juce::Justification::centred);
}

void FlayrChopEditor::attachKnob (Knob& k, const juce::String& id)
{
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
}

void FlayrChopEditor::editPad (const std::function<void (Pad&)>& fn)
{
    if (updatingInspector)
        return;
    // Every selected pad gets the same change (multi-edit).
    for (int i : proc.getSelection())
    {
        auto p = proc.getPad (i);
        if (! p.hasCue() && i != proc.selectedPad)
            continue;
        fn (p);
        proc.setPad (i, p);
    }
}

void FlayrChopEditor::setTargetKey (int key)
{
    // Smallest shift that lands on the key (up to +6, otherwise down).
    int shift = ((key - proc.getSampleKey()) % 12 + 12) % 12;
    if (shift > 6)
        shift -= 12;
    auto* prm = proc.apvts.getParameter ("keyShift");
    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) shift));
    refreshAll();
}

void FlayrChopEditor::refreshPadInspector()
{
    const juce::ScopedValueSetter<bool> guard (updatingInspector, true);
    const int i = proc.selectedPad;
    const auto p = proc.getPad (i);
    const auto sel = proc.getSelection();
    if (sel.size() > 1)
    {
        padTitle.setText (juce::String ((int) sel.size()) + " PADS", juce::dontSendNotification);
        padTitle.setColour (juce::Label::textColourId, ui::text);
        padInfo.setText ("Changes apply to every selected pad. Showing " + ui::padLabel (i) + ".", juce::dontSendNotification);
    }
    else
    {
        padTitle.setText ("PAD " + ui::padLabel (i), juce::dontSendNotification);
        padTitle.setColour (juce::Label::textColourId, p.hasCue() ? ui::padColour (i) : ui::dim);
        if (p.hasCue())
            padInfo.setText (ui::timeString (p.cue) + "  " + juce::String (proc.padEnd (i) - p.cue, 2) + " s   key "
                                 + juce::String::charToString ((juce::juce_wchar) ui::kPadKeys[i % kPadsPerBank]).toUpperCase()
                                 + "   note " + juce::MidiMessage::getMidiNoteName (kFirstPadNote + i, true, true, 3),
                             juce::dontSendNotification);
        else
            padInfo.setText ("Empty. Play the track and hit this pad, or drag on the waveform.", juce::dontSendNotification);
    }
    padGain.slider.setValue (p.gainDb, juce::dontSendNotification);
    padPan.slider.setValue (p.pan, juce::dontSendNotification);
    padFilter.slider.setValue (p.filter, juce::dontSendNotification);
    padAttack.slider.setValue (p.attackMs, juce::dontSendNotification);
    padRelease.slider.setValue (p.releaseMs, juce::dontSendNotification);
    padKey.slider.setValue (p.keyShift, juce::dontSendNotification);
    padStretch.slider.setValue (p.stretch, juce::dontSendNotification);
    padMode.setSelectedId ((int) p.mode + 1, juce::dontSendNotification);
    padChoke.setSelectedId (p.choke + 1, juce::dontSendNotification);
    padReverse.setToggleState (p.reverse, juce::dontSendNotification);
    padFavorite.setToggleState (p.favorite, juce::dontSendNotification);
    padFavorite.setButtonText (juce::String::fromUTF8 (p.favorite ? "\xe2\x98\x85 FAVORITE" : "\xe2\x98\x86 FAVORITE"));
    const auto& names = proc.getStemNames();
    for (int st = 0; st < kMaxStems; ++st)
    {
        auto& b = padStems[(size_t) st];
        b.setVisible (st < names.size());
        if (st < names.size())
        {
            b.setButtonText (names[st].toUpperCase());
            b.getProperties().set ("colour", (juce::int64) ui::stemColour (names[st], st).getARGB());
            b.setToggleState ((p.stemMask >> st) & 1u, juce::dontSendNotification);
        }
    }
}

void FlayrChopEditor::refreshStems()
{
    const auto& names = proc.getStemNames();
    for (int i = 0; i < kMaxStems; ++i)
    {
        auto& st = stems[(size_t) i];
        const bool on = i < names.size();
        for (juce::Component* c : { (juce::Component*) &st.name, (juce::Component*) &st.level, (juce::Component*) &st.mute, (juce::Component*) &st.solo })
            c->setVisible (on);
        if (on)
        {
            st.name.setText (names[i].toUpperCase(), juce::dontSendNotification);
            st.name.setColour (juce::Label::textColourId, ui::stemColour (names[i], i));
            st.level.setColour (juce::Slider::trackColourId, ui::stemColour (names[i], i));
        }
    }
    const bool hasStems = ! names.isEmpty();
    removeStemsBtn.setVisible (hasStems);
    separateBtn.setVisible (! hasStems && proc.getBusy() != FlayrChopProcessor::Busy::Separating);
    separateBtn.setEnabled (proc.hasSample() && proc.getBusy() == FlayrChopProcessor::Busy::None);
    layoutStems (stemsArea);
}

void FlayrChopEditor::refreshAll()
{
    const auto* s = proc.getSample();
    fileLabel.setText (proc.hasSample() || proc.isFileMissing() ? proc.getFileName() : "FLAYR CHOP", juce::dontSendNotification);

    const bool minor = proc.isSampleMinor();
    keyFrom.setText (s ? juce::String (analysis::keyName (proc.getSampleKey(), minor)) : "-", juce::dontSendNotification);
    keyTo.clear (juce::dontSendNotification);
    for (int k = 0; k < 12; ++k)
    {
        int shift = ((k - proc.getSampleKey()) % 12 + 12) % 12;
        if (shift > 6) shift -= 12;
        keyTo.addItem (juce::String (analysis::keyName (k, minor)) + "   " + analysis::wheelCode (k, minor) + "   "
                           + (shift > 0 ? "+" : "") + juce::String (shift),
                       k + 1);
    }
    keyTo.setSelectedId (proc.getTargetKey() + 1, juce::dontSendNotification);
    keyTo.setEnabled (s != nullptr);

    // Low-confidence detections are marked so they get checked, not trusted.
    const bool bpmUnsure = s && ! proc.isBpmFromUser() && proc.getBpmConfidence() < 0.4f;
    const bool keyUnsure = s && ! proc.isKeyFromUser() && proc.getKeyConfidence() < 0.4f;
    bpmFrom.setText (s ? juce::String (proc.getSampleBpm(), 1) + (bpmUnsure ? "?" : "") : "-", juce::dontSendNotification);
    bpmFrom.setColour (juce::Label::textColourId, bpmUnsure ? ui::warn : ui::text);
    if (keyUnsure)
        keyFrom.setText (keyFrom.getText() + "?", juce::dontSendNotification);
    keyFrom.setColour (juce::Label::textColourId, keyUnsure ? ui::warn : ui::text);
    const int mode = (int) proc.apvts.getRawParameterValue ("tempoMode")->load();
    bpmTo.setText (s ? juce::String (proc.getTargetBpm(), 1) : "-", juce::dontSendNotification);
    bpmTo.setVisible (mode != 2);
    manualTempo.setVisible (mode == 2);

    for (int b = 0; b < kNumBanks; ++b)
        bankBtns[(size_t) b].setToggleState (b == proc.bank, juce::dontSendNotification);
    chopMode.setSelectedId (proc.chopModeUI + 1, juce::dontSendNotification);
    chopCount.setSelectedId (proc.chopCountUI, juce::dontSendNotification);
    snapBtn.setToggleState (proc.snapUI, juce::dontSendNotification);

    if (s != nullptr && wave.viewLen <= 0.0)
        wave.resetView();
    if (s == nullptr)
        wave.viewLen = 0.0;

    refreshPadInspector();
    refreshStems();
    for (auto& pb : padButtons)
        pb->repaint();
    wave.repaint();
    overview.repaint();
}

void FlayrChopEditor::timerCallback()
{
    // Pad glow follows the voices.
    std::array<float, kNumPads> levels {};
    for (const auto& v : proc.sampler.views)
    {
        const int p = v.pad.load();
        if (p >= 0)
            levels[(size_t) p] = juce::jmax (levels[(size_t) p], v.level.load());
    }
    for (auto& pb : padButtons)
    {
        const float target = levels[(size_t) pb->index()];
        const float next = target > pb->level ? target : pb->level * 0.82f;
        if (std::abs (next - pb->level) > 0.005f)
        {
            pb->level = next;
            pb->repaint();
        }
    }
    wave.repaint();
    overview.repaint();

    const auto busy = proc.getBusy();
    const bool separating = busy == FlayrChopProcessor::Busy::Separating;
    progressValue = separating ? proc.getProgress() : -1.0;
    progressBar.setVisible (busy != FlayrChopProcessor::Busy::None);
    cancelBtn.setVisible (separating);
    separateBtn.setVisible (proc.getStemNames().isEmpty() && ! separating);
    separateBtn.setEnabled (proc.hasSample() && busy == FlayrChopProcessor::Busy::None);
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);

    const int mode = (int) proc.apvts.getRawParameterValue ("tempoMode")->load();
    if (proc.hasSample())
        bpmTo.setText (juce::String (proc.getTargetBpm(), 1), juce::dontSendNotification);
    juce::String r;
    if (proc.isRendering() || proc.isDraft())
        r = "rendering...";
    else if (mode == 1 && proc.getHostBpm() <= 0.0)
        r = "no host tempo";
    renderLabel.setText (r, juce::dontSendNotification);
    keyTo.setSelectedId (proc.getTargetKey() + 1, juce::dontSendNotification);

    // Transport readout, and the waveform pages along with the music while zoomed in.
    const bool playing = proc.isPlaying();
    playBtn.setButtonText (playing ? "STOP" : "PLAY");
    playBtn.setToggleState (playing, juce::dontSendNotification);
    if (const auto* s = proc.getSample())
    {
        const double ph = proc.getPlayhead();
        timeLabel.setText (ui::timeString (ph) + " / " + ui::timeString (s->lengthSec()), juce::dontSendNotification);
        if (playing && wave.viewLen < s->lengthSec() - 1e-6 && (ph > wave.viewStart + wave.viewLen * 0.92 || ph < wave.viewStart))
        {
            wave.viewStart = ph - wave.viewLen * 0.08;
            wave.clampView();
            overview.repaint();
        }
    }
}

//==============================================================================
void FlayrChopEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);

    g.setColour (ui::panel);
    for (auto r : { ptArea, stemsArea, inspectorArea, globalArea })
        g.fillRoundedRectangle (r.toFloat(), 8.0f);

    // Wordmark
    g.setFont (ui::font (13.0f, true));
    g.setColour (ui::accent);
    g.drawText ("FLAYR", headerArea.getX(), headerArea.getY(), 60, headerArea.getHeight(), juce::Justification::centredLeft);
    g.setColour (ui::accent2);
    g.drawText ("CHOP", headerArea.getX() + 48, headerArea.getY(), 60, headerArea.getHeight(), juce::Justification::centredLeft);

    // Section captions
    g.setFont (ui::font (10.5f, true));
    g.setColour (ui::dim);
    g.drawText ("KEY", ptArea.getX() + 14, ptArea.getY() + 6, 60, 14, juce::Justification::left);
    g.drawText ("TEMPO", ptArea.getX() + 420, ptArea.getY() + 6, 60, 14, juce::Justification::left);
    g.drawText ("PADS", padsArea.getX() + 2, padsArea.getY() + 4, 60, 18, juce::Justification::left);
    g.drawText ("PAD SETTINGS", inspectorArea.getX() + 14, inspectorArea.getY() + 8, 200, 14, juce::Justification::left);
    g.drawText ("GLOBAL", globalArea.getX() + 14, globalArea.getY() + 8, 200, 14, juce::Justification::left);
    if (padStems[0].isVisible())
        g.drawText ("PLAYS STEMS", inspectorArea.getX() + 14, padStems[0].getY() - 16, 200, 14, juce::Justification::left);
}

void FlayrChopEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! proc.hasSample())
        return;
    if (e.eventComponent == &keyFrom)
        showKeyMenu();
    else if (e.eventComponent == &bpmFrom && e.getNumberOfClicks() == 1)
        showBpmMenu();
}

void FlayrChopEditor::showBpmMenu()
{
    const auto* s = proc.getSample();
    juce::PopupMenu m;
    m.addSectionHeader (proc.isRefinedFromStems() ? "Detected (from stems)" : "Detected");
    const double cur = proc.getSampleBpm();
    std::vector<double> values;
    for (const auto& c : s->analysis.tempoCandidates)
    {
        values.push_back (c.bpm);
        m.addItem ((int) values.size(), juce::String (c.bpm, 1) + " BPM" + (values.size() == 1 ? "  (best)" : "  (" + juce::String (juce::roundToInt (c.score * 100.0f)) + "%)"),
                   true, std::abs (c.bpm - cur) < 0.05);
    }
    m.addSeparator();
    for (double f : { 0.5, 2.0, 2.0 / 3.0, 1.5 })
    {
        values.push_back (cur * f);
        const juce::String label = f == 0.5 ? "Half" : f == 2.0 ? "Double" : f < 1.0 ? "Two thirds" : "Three halves";
        m.addItem ((int) values.size(), label + " (" + juce::String (cur * f, 1) + ")");
    }
    m.addSeparator();
    m.addItem (1000, "Type a value...");
    m.addItem (1001, "Tap tempo (use the TAP button)", false);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (bpmFrom), [this, values] (int r) {
        if (r == 1000)
            bpmFrom.showEditor();
        else if (r > 0 && r <= (int) values.size())
            proc.setSampleBpm (values[(size_t) r - 1]);
        refreshAll();
    });
}

void FlayrChopEditor::showKeyMenu()
{
    const auto* s = proc.getSample();
    juce::PopupMenu m;
    m.addSectionHeader (proc.isRefinedFromStems() ? "Detected (from stems)" : "Detected");
    std::vector<std::pair<int, bool>> keys;
    for (const auto& c : s->analysis.keyCandidates)
    {
        keys.push_back ({ c.key, c.minor });
        m.addItem ((int) keys.size(), juce::String (analysis::keyName (c.key, c.minor)) + "  " + analysis::wheelCode (c.key, c.minor)
                                          + (keys.size() == 1 ? "  (best)" : "  (" + juce::String (juce::roundToInt (c.score * 100.0f)) + "%)"),
                   true, proc.getSampleKey() == c.key && proc.isSampleMinor() == c.minor);
    }
    // Relative major/minor is the most common mix-up, so it's always one click away.
    const int rel = proc.isSampleMinor() ? (proc.getSampleKey() + 3) % 12 : (proc.getSampleKey() + 9) % 12;
    keys.push_back ({ rel, ! proc.isSampleMinor() });
    m.addSeparator();
    m.addItem ((int) keys.size(), juce::String ("Relative ") + (proc.isSampleMinor() ? "major: " : "minor: ") + analysis::keyName (rel, ! proc.isSampleMinor()));
    for (int minor = 0; minor < 2; ++minor)
    {
        juce::PopupMenu sub;
        for (int k = 0; k < 12; ++k)
            sub.addItem (500 + k + 12 * minor, analysis::keyName (k, minor == 1), true,
                         proc.getSampleKey() == k && proc.isSampleMinor() == (minor == 1));
        m.addSubMenu (minor ? "All minor keys" : "All major keys", sub);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (keyFrom), [this, keys] (int r) {
        if (r >= 500)
            proc.setSampleKey ((r - 500) % 12, r - 500 >= 12);
        else if (r > 0 && r <= (int) keys.size())
            proc.setSampleKey (keys[(size_t) r - 1].first, keys[(size_t) r - 1].second);
        refreshAll();
    });
}

void FlayrChopEditor::tap()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (! taps.empty() && now - taps.back() > 2.0)
        taps.clear(); // a pause starts a new count
    taps.push_back (now);
    if (taps.size() > 9)
        taps.erase (taps.begin());
    if (taps.size() >= 4 && proc.hasSample())
    {
        // Least-squares slope through the taps: steadier than averaging intervals.
        const double n = (double) taps.size();
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < taps.size(); ++i)
        {
            sx += (double) i; sy += taps[i]; sxx += (double) i * i; sxy += (double) i * taps[i];
        }
        const double period = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        if (period > 0.15 && period < 3.0)
        {
            double bpm = 60.0 / period;
            // If a detected candidate is within 3%, take its precise value.
            for (const auto& c : proc.getSample()->analysis.tempoCandidates)
                if (std::abs (c.bpm - bpm) / bpm < 0.03)
                    bpm = c.bpm;
            proc.setSampleBpm (bpm);
            refreshAll();
        }
    }
    tapBtn.setButtonText ("TAP " + juce::String ((int) taps.size()));
}

void FlayrChopEditor::togglePlay()
{
    if (! proc.hasSample())
        return;
    if (proc.isPlaying())
        proc.stop();
    else
        proc.play();
}

void FlayrChopEditor::nudgeSelection (int direction, bool big)
{
    // Small steps scale with the zoom (about 2 px); shift moves by a 16th note.
    const double small = juce::jmax (0.0005, wave.viewLen / juce::jmax (1, wave.getWidth()) * 2.0);
    const double step = big ? 15.0 / juce::jmax (20.0, proc.getSampleBpm()) : small;
    proc.nudge (proc.getSelection(), direction * step);
    refreshAll();
}

void FlayrChopEditor::deleteSelection()
{
    proc.deletePads (proc.getSelection());
    refreshAll();
}

juce::File FlayrChopEditor::slicesFolder()
{
    // A permanent folder: DAWs reference dropped files where they are, so never use a temp folder.
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Flayr Chop").getChildFile ("Slices");
}

void FlayrChopEditor::exportDrag (const juce::MouseEvent&)
{
    juce::StringArray files;
    for (int i : proc.getSelection())
        if (proc.getPad (i).hasCue())
        {
            const auto f = proc.exportSlice (i, slicesFolder());
            if (f.existsAsFile())
                files.add (f.getFullPathName());
        }
    if (! files.isEmpty())
        juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

void FlayrChopEditor::runChop()
{
    const int m = proc.chopModeUI;
    const int first = proc.bank * kPadsPerBank;
    const int count = juce::jmin (proc.chopCountUI, kNumPads - first);
    if (m == 5)
        proc.slicerFromPlayhead (first, count, chopSpacing.getSelectedId() / 4.0);
    else if (m == 6)
        proc.randomCues (first, count);
    else if (m == 7)
        proc.keyShiftPad (proc.selectedPad, first);
    else
    {
        const auto mode = m == 0 ? analysis::ChopMode::Auto : m == 4 ? analysis::ChopMode::Even : analysis::ChopMode::Beats;
        const double beats = m == 2 ? 2.0 : m == 3 ? 4.0 : 1.0;
        proc.chop (mode, count, first, beats);
    }
    refreshAll();
}

bool FlayrChopEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey)
    {
        nudgeSelection (k.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1, k.getModifiers().isShiftDown());
        return true;
    }
    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        deleteSelection();
        return true;
    }
    // Pad keys are handled in keyStateChanged (they need key-up too).
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    for (int i = 0; i < kPadsPerBank; ++i)
        if (c == (juce::juce_wchar) ui::kPadKeys[i] && ! k.getModifiers().isCommandDown())
            return true;
    return false;
}

bool FlayrChopEditor::keyStateChanged (bool)
{
    bool used = false;
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown ((int) ui::kPadKeys[i])
                          || juce::KeyPress::isKeyCurrentlyDown ((int) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) ui::kPadKeys[i]));
        if (down == keyHeld[(size_t) i])
            continue;
        keyHeld[(size_t) i] = down;
        used = true;
        const int pad = proc.bank * kPadsPerBank + i;
        const bool keysMode = proc.apvts.getRawParameterValue ("keys")->load() > 0.5f;
        if (down)
        {
            if (! keysMode && ! proc.getPad (pad).hasCue())
            {
                proc.setCueAtPlayhead (pad); // empty pad: drop a cue at the playhead
                refreshAll();
                continue;
            }
            proc.select (pad, false);
            refreshPadInspector();
        }
        proc.triggerFromUI (keysMode ? kKeysRootNote + i - 7 : kFirstPadNote + pad, down);
    }
    return used;
}

void FlayrChopEditor::paintOverChildren (juce::Graphics& g)
{
    if (! fileHover)
        return;
    g.setColour (ui::accent.withAlpha (0.12f));
    g.fillRect (getLocalBounds());
    g.setColour (ui::accent);
    g.drawRect (getLocalBounds(), 3);
    g.setFont (ui::font (22.0f, true));
    g.drawText ("Drop to load", getLocalBounds(), juce::Justification::centred);
}

void FlayrChopEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    headerArea = r.removeFromTop (34);
    {
        auto h = headerArea;
        h.removeFromLeft (100);
        exportBtn.setBounds (h.removeFromRight (84).reduced (0, 3));
        h.removeFromRight (6);
        loadBtn.setBounds (h.removeFromRight (70).reduced (0, 3));
        h.removeFromRight (12);
        cancelBtn.setBounds (h.removeFromRight (70).reduced (0, 5));
        progressBar.setBounds (h.removeFromRight (160).reduced (4, 9));
        statusLabel.setBounds (h.removeFromRight (300));
        fileLabel.setBounds (h);
    }
    r.removeFromTop (8);

    ptArea = r.removeFromTop (64);
    {
        auto p = ptArea.reduced (12, 6);
        p.removeFromTop (14);
        auto keyArea = p.removeFromLeft (400);
        keyFrom.setBounds (keyArea.removeFromLeft (60));
        keyArrow.setBounds (keyArea.removeFromLeft (24));
        keyTo.setBounds (keyArea.removeFromLeft (150).reduced (0, 4));
        keyArea.removeFromLeft (6);
        keyDown.setBounds (keyArea.removeFromLeft (30).reduced (0, 4));
        keyUp.setBounds (keyArea.removeFromLeft (30).reduced (2, 4));
        keyArea.removeFromLeft (6);
        formantBtn.setBounds (keyArea.removeFromLeft (90).reduced (0, 4));

        auto t = p;
        tempoMode.setBounds (t.removeFromLeft (110).reduced (0, 4));
        t.removeFromLeft (8);
        bpmFrom.setBounds (t.removeFromLeft (70));
        halfBtn.setBounds (t.removeFromLeft (32).reduced (1, 6));
        doubleBtn.setBounds (t.removeFromLeft (32).reduced (1, 6));
        tapBtn.setBounds (t.removeFromLeft (46).reduced (1, 6));
        bpmArrow.setBounds (t.removeFromLeft (24));
        auto to = t.removeFromLeft (120);
        bpmTo.setBounds (to);
        manualTempo.setBounds (to.reduced (0, 6));
        t.removeFromLeft (8);
        pitchTimeBtn.setBounds (t.removeFromRight (120).reduced (0, 4));
        renderLabel.setBounds (t);
    }
    r.removeFromTop (8);

    toolbarArea = r.removeFromTop (28);
    {
        auto t = toolbarArea;
        playBtn.setBounds (t.removeFromLeft (80).reduced (0, 1));
        t.removeFromLeft (8);
        timeLabel.setBounds (t.removeFromLeft (140));
        zoomOutBtn.setBounds (t.removeFromLeft (30).reduced (1, 1));
        zoomInBtn.setBounds (t.removeFromLeft (30).reduced (1, 1));
        t.removeFromLeft (14);
        nudgeLeftBtn.setBounds (t.removeFromLeft (30).reduced (1, 1));
        nudgeRightBtn.setBounds (t.removeFromLeft (30).reduced (1, 1));
        clearAllBtn.setBounds (t.removeFromRight (96).reduced (1, 1));
        deleteBtn.setBounds (t.removeFromRight (80).reduced (1, 1));
        selectAllBtn.setBounds (t.removeFromRight (100).reduced (1, 1));
    }
    r.removeFromTop (6);
    auto waveArea = r.removeFromTop (juce::jmax (150, r.getHeight() - 520));
    wave.setBounds (waveArea);
    r.removeFromTop (4);
    overview.setBounds (r.removeFromTop (34));
    r.removeFromTop (8);

    stemsArea = r.removeFromTop (44);
    layoutStems (stemsArea);
    r.removeFromTop (8);

    // Bottom: pads | inspector | global
    padsArea = r.removeFromLeft (juce::jmin (r.getHeight() + 40, (int) (r.getWidth() * 0.42)));
    {
        auto pa = padsArea;
        auto banks = pa.removeFromTop (26);
        banks.removeFromLeft (52);
        for (auto& b : bankBtns)
            b.setBounds (banks.removeFromLeft (40).reduced (2, 1));
        pa.removeFromTop (4);
        const int cw = pa.getWidth() / 4, ch = pa.getHeight() / 4;
        // Bottom row is pad 1, like an MPC.
        for (int i = 0; i < kPadsPerBank; ++i)
        {
            const int col = i % 4, row = 3 - i / 4;
            padButtons[(size_t) i]->setBounds (pa.getX() + col * cw, pa.getY() + row * ch, cw, ch);
        }
    }
    r.removeFromLeft (10);
    globalArea = r.removeFromRight (250);
    r.removeFromRight (10);
    inspectorArea = r;

    {
        auto a = inspectorArea.reduced (14, 10);
        a.removeFromTop (16);
        auto top = a.removeFromTop (26);
        padTitle.setBounds (top.removeFromLeft (100));
        padClear.setBounds (top.removeFromRight (70).reduced (0, 2));
        padFavorite.setBounds (top.removeFromRight (110).reduced (4, 2));
        padInfo.setBounds (a.removeFromTop (18));
        a.removeFromTop (6);
        auto knobs = a.removeFromTop (84);
        const int kw = knobs.getWidth() / 7;
        for (auto* k : { &padGain, &padPan, &padFilter, &padAttack, &padRelease, &padKey, &padStretch })
        {
            auto c = knobs.removeFromLeft (kw);
            k->label.setBounds (c.removeFromTop (14));
            k->slider.setBounds (c);
        }
        a.removeFromTop (8);
        auto row = a.removeFromTop (28);
        const int w3 = row.getWidth() / 3;
        padMode.setBounds (row.removeFromLeft (w3).reduced (2, 2));
        padChoke.setBounds (row.removeFromLeft (w3).reduced (2, 2));
        padReverse.setBounds (row.reduced (2, 2));
        a.removeFromTop (22);
        auto stemRow = a.removeFromTop (28);
        const int sw = stemRow.getWidth() / juce::jmax (1, juce::jmin (kMaxStems, juce::jmax (4, proc.numStems())));
        for (auto& b : padStems)
            b.setBounds (stemRow.removeFromLeft (sw).reduced (2, 2));
        dragHint.setBounds (a.removeFromBottom (26).reduced (2, 0));
    }

    {
        auto g = globalArea.reduced (14, 10);
        g.removeFromTop (16);
        auto knobs = g.removeFromTop (84);
        const int kw = knobs.getWidth() / 3;
        for (auto* k : { &volume, &filter, &velocity })
        {
            auto c = knobs.removeFromLeft (kw);
            k->label.setBounds (c.removeFromTop (14));
            k->slider.setBounds (c);
        }
        g.removeFromTop (8);
        auto row = g.removeFromTop (28);
        monoBtn.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 2));
        keysBtn.setBounds (row.reduced (2, 2));
        g.removeFromTop (6);
        auto q = g.removeFromTop (26);
        quantizeLabel.setBounds (q.removeFromLeft (80));
        quantize.setBounds (q.reduced (2, 1));
        g.removeFromTop (12);
        chopLabel.setBounds (g.removeFromTop (16));
        auto c1 = g.removeFromTop (28);
        chopMode.setBounds (c1.removeFromLeft (c1.getWidth() * 3 / 5).reduced (2, 2));
        // The slicer's spacing takes the pad-count slot (it always fills the bank).
        const bool slicer = proc.chopModeUI == 5;
        chopSpacing.setVisible (slicer);
        chopCount.setVisible (! slicer && proc.chopModeUI != 7);
        chopCount.setBounds (c1.reduced (2, 2));
        chopSpacing.setBounds (c1.reduced (2, 2));
        auto c2 = g.removeFromTop (30);
        snapBtn.setBounds (c2.removeFromLeft (c2.getWidth() / 2).reduced (2, 3));
        chopBtn.setBounds (c2.reduced (2, 3));
    }
}

void FlayrChopEditor::layoutStems (juce::Rectangle<int> area)
{
    auto a = area.reduced (12, 6);
    stemsTitle.setBounds (a.removeFromLeft (56));
    const int n = proc.numStems();
    if (n == 0)
    {
        separateBtn.setBounds (a.removeFromLeft (160).reduced (0, 2));
        return;
    }
    removeStemsBtn.setBounds (a.removeFromRight (28).reduced (0, 4));
    const int w = a.getWidth() / n;
    for (int i = 0; i < n && i < kMaxStems; ++i)
    {
        auto c = a.removeFromLeft (w).reduced (6, 0);
        auto& st = stems[(size_t) i];
        st.name.setBounds (c.removeFromLeft (64));
        st.solo.setBounds (c.removeFromRight (26).reduced (0, 3));
        st.mute.setBounds (c.removeFromRight (28).reduced (2, 3));
        st.level.setBounds (c.reduced (2, 0));
    }
}

//==============================================================================
// Drops can carry several items (some hosts add companion files); the first audio file wins.
static juce::File firstAudioFile (const juce::StringArray& items)
{
    for (auto item : items)
    {
        item = item.trim().unquoted();
        if (item.startsWithIgnoreCase ("file://"))
            item = juce::URL (item).getLocalFile().getFullPathName();
        const juce::File f (item);
        if (juce::File::isAbsolutePath (item) && f.existsAsFile() && chop::isAudioFile (f))
            return f;
    }
    return {};
}

bool FlayrChopEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return firstAudioFile (files) != juce::File();
}

void FlayrChopEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileHover = false;
    loadDropped (firstAudioFile (files));
    repaint();
}

bool FlayrChopEditor::isInterestedInTextDrag (const juce::String& text)
{
    return firstAudioFile (juce::StringArray::fromLines (text)) != juce::File();
}

void FlayrChopEditor::textDropped (const juce::String& text, int, int)
{
    fileHover = false;
    loadDropped (firstAudioFile (juce::StringArray::fromLines (text)));
    repaint();
}

void FlayrChopEditor::loadDropped (const juce::File& f)
{
    if (! f.existsAsFile())
        return;
    wave.viewLen = 0.0;
    proc.loadFile (f);
}

void FlayrChopEditor::choosFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File(), chop::audioFileWildcards().joinIntoString (";"));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                              {
                                  wave.viewLen = 0.0;
                                  proc.loadFile (f);
                              }
                          });
}

void FlayrChopEditor::showExportMenu()
{
    juce::PopupMenu m;
    const bool ready = proc.currentRender() != nullptr;
    m.addItem (1, "Export track at current key and tempo...", ready);
    m.addItem (2, "Export track + stems...", ready && proc.numStems() > 0);
    m.addItem (3, "Export this bank's slices...", ready);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (exportBtn), [this] (int r) {
        if (r == 0)
            return;
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, r] (const juce::FileChooser& fc) {
                                  const auto dir = fc.getResult();
                                  if (dir == juce::File())
                                      return;
                                  bool ok = true;
                                  if (r == 1 || r == 2)
                                      ok = proc.exportAll (dir, r == 2);
                                  else
                                      for (int i = proc.bank * kPadsPerBank; i < (proc.bank + 1) * kPadsPerBank; ++i)
                                          if (proc.getPad (i).hasCue())
                                              ok = proc.exportSlice (i, dir).existsAsFile() && ok;
                                  statusLabel.setText (ok ? "Exported to " + dir.getFullPathName() : "Export failed", juce::dontSendNotification);
                                  if (ok)
                                      dir.revealToUser();
                              });
    });
}
