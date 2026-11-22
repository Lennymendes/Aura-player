// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
// AURA DSP — Aura Precision Audio Control: the audio chain in one panel. Left, one row per stage (source, headroom,
// sample rate conversion, crossfeed, headphone EQ, parametric EQ, polarity) with its switch and its LED; right, the
// view of the selected equaliser:
//   Égaliseur casque       searchable, scrolling list of headphone profiles and studio curves
//   Égaliseur paramétrique 7 bands (Bass, Band 1-5, Treble): knobs (drag, wheel, typed value) and numbered handles on
//                          the response curve (drag = frequency / gain, wheel = Q); reset, copy the headphone curve,
//                          user presets.
// Both equalisers can run together (in series, 64-bit); the curve shown is the combined one, and the headroom
// follows its peak.
#pragma once

#include "Widgets.h"
#include "../DSP/HeadphoneEq.h"

namespace nx
{

/** Response curve; in parametric mode, 7 numbered handles edit the bands. */
class EqGraph : public juce::Component
{
public:
    explicit EqGraph (AppContext& c) : ctx (c) { setRepaintsOnMouseActivity (true); }
    bool editable = false;
    std::function<void (const hpeq::ParametricBands&, bool final)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    juce::Rectangle<float> plotArea() const;
    float xOf (double freq) const;
    float yOf (double db) const;
    double freqAt (float x) const;
    double dbAt (float y) const;
    juce::Point<float> handlePosition (int band) const;
    int handleAt (juce::Point<float> p) const;
    /** Moves one handle as a drag would (tests use it too). */
    void moveHandle (int band, double freq, double gain, bool final);

private:
    AppContext& ctx;
    int dragging = -1;
};

/** One band of the parametric EQ: name, filter type (Band 1 and 5), gain / frequency / Q knobs. */
class BandStrip : public juce::Component
{
public:
    BandStrip (AppContext& c, int index);
    void resized() override;
    void paint (juce::Graphics&) override;
    void setBand (const hpeq::Band& b);
    void lookAndFeelChanged() override;
    std::function<void (int index, const hpeq::Band&, bool final)> onChange;
    juce::Slider gain, freq, q;
    juce::ComboBox type;
    static juce::String bandName (int index);
private:
    void changed (bool final);
    AppContext& ctx;
    int index;
    bool updating = false;
    hpeq::BandType fixedType;
};

class AuraDspPanel : public juce::Component, private juce::Timer, private juce::ListBoxModel
{
public:
    explicit AuraDspPanel (AppContext& c);
    void open();
    void close();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { close(); return true; } return false; }

    enum class View { Headphone, Parametric, E9ds };
    void setView (View v);
    View getView() const noexcept { return view; }

    /** Tests: what the user does with the mouse / keyboard. */
    void choosePreset (int index);
    void chooseMonitor (int index);
    void searchFor (const juce::String& text) { search.setText (text, false); refilter(); }
    int visiblePresetCount() const;
    juce::String visiblePresetName (int row) const;
    BandStrip& getBand (int i) { return *bands[i]; }
    EqGraph& getGraph() { return graph; }
    juce::TextButton resetButton, copyButton, saveButton;
    /** Headphone view, once a profile is loaded: adds the parametric EQ on top (its own bands, the profile untouched). */
    juce::TextButton addPeqButton;
    juce::ComboBox userPresets;
    juce::ToggleButton headroom, upsample, crossfeed, hpEq, peq, phase, e9ds;
    /** E9DS view: Focus 4-12 kHz (presets 6.5 / 9 kHz), Amount 0 to -6 dB, Split-Band / Dynamic HF. */
    juce::Slider e9Focus, e9Amount;
    juce::TextButton e9Split, e9Hf, e9Sib, e9Bril;

    static constexpr int rowH = 54;

private:
    void timerCallback() override;
    void syncControls();
    void refilter();
    void bandsChanged (const hpeq::ParametricBands& b, bool final);
    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> rowBounds (int row) const;

    // ListBoxModel
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override { chooseRow (row); }
    void returnKeyPressed (int row) override { chooseRow (row); }
    void chooseRow (int row);

    AppContext& ctx;
    IconButton closeButton;
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<int> rows;                 // preset index, or -1 / -2 for the two category headers
    EqGraph graph;
    juce::OwnedArray<BandStrip> bands;
    View view = View::Headphone;
    std::vector<std::pair<juce::Rectangle<int>, int>> monitorSegs;
    bool saveQueued = false;
    int e9Ticks = 0;
    juce::Rectangle<int> left, right;
    juce::Rectangle<int> e9Meter;
    std::vector<std::pair<int, std::unique_ptr<InfoButton>>> infos;   // row, its "+"
public:
    InfoButton* infoFor (const juce::String& id) { for (auto& [r, b] : infos) if (b->id == id) return b.get(); return nullptr; }
private:
    void e9Changed (bool persist);
};

} // namespace nx
