// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "AuraDsp.h"
#include <limits>
#include "../DSP/StudioTools.h"
#include "../DSP/E9ds.h"

namespace nx
{

namespace
{
    constexpr double graphFs = 48000.0;
    constexpr double graphRangeDb = 12.0;

    juce::String hz (double f)
    {
        return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2).replaceCharacter ('.', ',') + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
    }
}

//==============================================================================
// EqGraph
//==============================================================================
juce::Rectangle<float> EqGraph::plotArea() const { return getLocalBounds().toFloat().reduced (36.0f, 16.0f).withTrimmedBottom (6.0f); }
float EqGraph::xOf (double f) const { const auto p = plotArea(); return p.getX() + (float) (std::log10 (juce::jmax (20.0, f) / 20.0) / 3.0) * p.getWidth(); }
float EqGraph::yOf (double db) const { const auto p = plotArea(); return p.getCentreY() - (float) (db / graphRangeDb) * p.getHeight() * 0.5f; }
double EqGraph::freqAt (float x) const { const auto p = plotArea(); return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, (double) ((x - p.getX()) / p.getWidth()))); }
double EqGraph::dbAt (float y) const { const auto p = plotArea(); return (double) ((p.getCentreY() - y) / (p.getHeight() * 0.5f)) * graphRangeDb; }

juce::Point<float> EqGraph::handlePosition (int band) const
{
    const auto b = ctx.studioTools().parametricBands()[(size_t) band];
    return { xOf (b.freq), yOf (b.gain) };
}

int EqGraph::handleAt (juce::Point<float> pt) const
{
    int best = -1;
    float bestD = 14.0f;
    for (int i = 0; i < 7; ++i)
        if (const float d = handlePosition (i).getDistanceFrom (pt); d < bestD) { bestD = d; best = i; }
    return best;
}

void EqGraph::moveHandle (int band, double freq, double gain, bool final)
{
    auto b = ctx.studioTools().parametricBands();
    b[(size_t) band].freq = freq;
    b[(size_t) band].gain = gain;
    b[(size_t) band] = hpeq::clampBand (b[(size_t) band]);
    if (onChange) onChange (b, final);
    repaint();
}

void EqGraph::mouseDown (const juce::MouseEvent& e) { dragging = editable ? handleAt (e.position) : -1; }

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging >= 0) moveHandle (dragging, freqAt (e.position.x), dbAt (e.position.y), false);
}

void EqGraph::mouseUp (const juce::MouseEvent& e)
{
    if (dragging >= 0) moveHandle (dragging, freqAt (e.position.x), dbAt (e.position.y), true);
    dragging = -1;
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int i = editable ? handleAt (e.position) : -1;
    if (i < 0) return;
    auto b = ctx.studioTools().parametricBands();
    b[(size_t) i].q = juce::jlimit (0.5, 10.0, b[(size_t) i].q * std::pow (1.15, w.deltaY * 4.0));
    if (onChange) onChange (b, true);
    repaint();
}

void EqGraph::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    const auto& s = ctx.studioTools();
    auto area = getLocalBounds().toFloat();
    g.setColour (style::neutral().withAlpha (0.04f));
    g.fillRoundedRectangle (area, 12.0f);
    const auto plot = plotArea();
    g.setFont (style::font (10.5f));
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        g.setColour (t.border());
        g.fillRect (xOf (f), plot.getY(), 1.0f, plot.getHeight());
        g.setColour (t.textDim());
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (40, 14).withCentre ({ xOf (f), plot.getBottom() + 9.0f }), juce::Justification::centred);
    }
    for (int db : { -12, -6, 0, 6, 12 })
    {
        g.setColour (db == 0 ? t.textDim().withAlpha (0.6f) : t.border());
        g.fillRect (plot.getX(), yOf (db), plot.getWidth(), 1.0f);
        g.setColour (t.textDim());
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (30, 12).withPosition (area.getX() + 4.0f, yOf (db) - 6.0f), juce::Justification::centredLeft);
    }

    auto curve = [&] (auto&& dbOf, juce::Colour c, float width)
    {
        juce::Path path;
        for (int i = 0; i <= 200; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 200.0);
            const auto pt = juce::Point<float> (xOf (f), yOf (juce::jlimit (-graphRangeDb, graphRangeDb, dbOf (f))));
            if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        g.setColour (c);
        g.strokePath (path, juce::PathStrokeType (width, juce::PathStrokeType::curved));
    };
    const auto* preset = s.eqPresetInfo();
    const auto peqBands = s.parametricBands();
    const double preamp = s.autoHeadroom.load() ? s.eqPreampDb.load() : 0.0;
    // the two equalisers on their own, faint, then the combined curve with the headroom
    if (preset != nullptr && s.eqActive() && s.peqActive())
    {
        curve ([&] (double f) { return hpeq::responseDb (*preset, graphFs, f); }, t.accent2().withAlpha (0.45f), 1.2f);
        curve ([&] (double f) { return hpeq::responseDb (peqBands.data(), 7, graphFs, f); }, t.textDim().withAlpha (0.55f), 1.2f);
    }
    if (s.eqActive() || s.peqActive())
        curve ([&] (double f) { return s.combinedDb (graphFs, f) + preamp; }, t.accent(), 2.4f);
    else if (editable)
        curve ([&] (double f) { return hpeq::responseDb (peqBands.data(), 7, graphFs, f); }, t.textDim(), 1.6f);
    else if (preset != nullptr)
        curve ([&] (double f) { return hpeq::responseDb (*preset, graphFs, f); }, t.textDim(), 1.6f);

    if (! editable) return;
    const auto mouse = getMouseXYRelative().toFloat();
    const int hot = dragging >= 0 ? dragging : handleAt (mouse);
    for (int i = 0; i < 7; ++i)
    {
        const auto c = handlePosition (i);
        const auto r = juce::Rectangle<float> (20.0f, 20.0f).withCentre (c);
        g.setColour (i == hot ? t.accent() : t.surface().withAlpha (0.95f));
        g.fillEllipse (r);
        g.setColour (s.peqActive() ? t.accent() : t.textDim());
        g.drawEllipse (r.reduced (0.5f), 1.6f);
        g.setColour (i == hot ? t.colour ("--nx-on-accent", juce::Colours::black) : t.textCol());
        g.setFont (style::font (10.5f, true));
        g.drawText (i == 0 ? "B" : i == 6 ? "T" : juce::String (i), r, juce::Justification::centred);
    }
    if (hot >= 0)
    {
        const auto b = peqBands[(size_t) hot];
        g.setColour (t.textCol());
        g.setFont (style::font (11.5f, true));
        g.drawText (BandStrip::bandName (hot) + "  " + (b.gain > 0 ? "+" : "") + juce::String (b.gain, 1).replaceCharacter ('.', ',') + " dB  " + hz (b.freq)
                        + "  Q " + juce::String (b.q, 2).replaceCharacter ('.', ','),
                    plot.withHeight (16.0f).translated (0.0f, 2.0f), juce::Justification::centredRight);
    }
}

//==============================================================================
// BandStrip
//==============================================================================
juce::String BandStrip::bandName (int i)
{
    return i == 0 ? juce::String ("Bass") : i == 6 ? juce::String ("Treble") : "Band " + juce::String (i);
}

BandStrip::BandStrip (AppContext& c, int i) : ctx (c), index (i),
    fixedType (i == 0 ? hpeq::BandType::LowShelf : i == 6 ? hpeq::BandType::HighShelf : hpeq::BandType::Peak)
{
    auto setup = [this] (juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix)
    {
        sl.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        sl.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 18);
        sl.setRange (lo, hi, step);
        sl.setTextValueSuffix (suffix);
        sl.setScrollWheelEnabled (true);
        sl.setTextBoxIsEditable (true);           // click the value to type it
        sl.setMouseDragSensitivity (220);
        sl.onValueChange = [this] { changed (false); };
        sl.onDragEnd = [this] { changed (true); };
        addAndMakeVisible (sl);
    };
    setup (gain, -12.0, 12.0, 0.1, " dB");
    setup (freq, 20.0, 20000.0, 1.0, " Hz");
    setup (q, 0.5, 10.0, 0.01, "");
    freq.setSkewFactorFromMidPoint (630.0);       // log-like: 20 Hz ... 630 Hz ... 20 kHz
    freq.textFromValueFunction = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 2) + "k" : juce::String (juce::roundToInt (v)); };
    freq.valueFromTextFunction = [] (const juce::String& s)
    {
        const auto t = s.trim().toLowerCase().replaceCharacter (',', '.');
        return t.containsChar ('k') ? t.upToFirstOccurrenceOf ("k", false, false).getDoubleValue() * 1000.0 : t.getDoubleValue();
    };
    freq.setTextValueSuffix ({});
    gain.setDoubleClickReturnValue (true, 0.0);
    lookAndFeelChanged();
    if (index == 1 || index == 5)
    {
        type.addItem ("Peak", 1);
        type.addItem ("Low Shelf", 2);
        type.addItem ("High Shelf", 3);
        type.onChange = [this] { changed (true); };
        addAndMakeVisible (type);
    }
}

void BandStrip::lookAndFeelChanged()
{
    // the knobs follow the theme (accent arc, neutral track), also when it changes while the panel is open
    const auto& t = ctx.theme();
    for (auto* sl : { &gain, &freq, &q })
    {
        sl->setColour (juce::Slider::rotarySliderFillColourId, t.accent());
        sl->setColour (juce::Slider::rotarySliderOutlineColourId, t.textCol().withAlpha (0.14f));
        sl->setColour (juce::Slider::thumbColourId, t.textCol());
        sl->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        sl->setColour (juce::Slider::textBoxBackgroundColourId, t.textCol().withAlpha (0.06f));
        sl->setColour (juce::Slider::textBoxTextColourId, t.textCol());
    }
}

void BandStrip::setBand (const hpeq::Band& b)
{
    updating = true;
    gain.setValue (b.gain, juce::dontSendNotification);
    freq.setValue (b.freq, juce::dontSendNotification);
    q.setValue (b.q, juce::dontSendNotification);
    if (type.isVisible()) type.setSelectedId (b.type == hpeq::BandType::LowShelf ? 2 : b.type == hpeq::BandType::HighShelf ? 3 : 1, juce::dontSendNotification);
    updating = false;
}

void BandStrip::changed (bool final)
{
    if (updating || ! onChange) return;
    const auto t = type.isVisible() ? (type.getSelectedId() == 2 ? hpeq::BandType::LowShelf : type.getSelectedId() == 3 ? hpeq::BandType::HighShelf : hpeq::BandType::Peak) : fixedType;
    onChange (index, { t, freq.getValue(), gain.getValue(), q.getValue() }, final);
}

void BandStrip::resized()
{
    auto r = getLocalBounds().reduced (2, 0);
    r.removeFromTop (20);
    auto typeRow = r.removeFromTop (24);
    if (type.isVisible()) type.setBounds (typeRow.reduced (2, 1));
    const int k = r.getHeight() / 3;
    for (auto* s : { &gain, &freq, &q }) s->setBounds (r.removeFromTop (k).withTrimmedTop (12).reduced (4, 1));
}

void BandStrip::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    g.setColour (style::neutral().withAlpha (0.035f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    g.setColour (t.textCol());
    g.setFont (style::font (12.5f, true));
    g.drawText (bandName (index), getLocalBounds().removeFromTop (20), juce::Justification::centred);
    if (! type.isVisible())
    {
        g.setColour (t.textDim());
        g.setFont (style::font (10.5f));
        g.drawText (index == 0 ? "Low Shelf" : index == 6 ? "High Shelf" : "Peak", getLocalBounds().withTrimmedTop (20).removeFromTop (24), juce::Justification::centred);
    }
    g.setColour (t.textDim());
    g.setFont (style::font (9.5f, true));
    const juce::String caps[] = { "GAIN", "FRÉQ."_t, "Q" };
    int c = 0;
    for (auto* s : { &gain, &freq, &q }) g.drawText (caps[c++], s->getBounds().withY (s->getY() - 12).withHeight (11), juce::Justification::centred);
}

//==============================================================================
// AuraDspPanel
//==============================================================================
namespace
{
    enum Row { Source, Headroom, TruePeak, Conversion, Crossfeed, HeadphoneEq, ParametricEq, Polarity, E9ds, numRows };
    const char* const rowTitles[] = { "Source", "Gestion du headroom", "Auto-Headroom", "Conversion de fréquence", "Crossfeed", "Égaliseur casque", "Égaliseur paramétrique", "Polarité", "E9DS" };
}

AuraDspPanel::AuraDspPanel (AppContext& c)
    : resetButton ("Réinitialiser (0 dB)"_t), copyButton ("Copier la courbe du casque actif"_t), saveButton ("Enregistrer le preset…"_t),
      addPeqButton ("+ EQ paramétrique"_t),
      ctx (c), closeButton (style::Icon::Close, "Fermer"_t, c), graph (c)
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (closeButton);
    closeButton.onClick = [this] { close(); };
    for (auto* t : { &headroom, &truePeak, &upsample, &crossfeed, &hpEq, &peq, &phase, &e9ds }) addAndMakeVisible (t);
    headroom.onClick = [this]
    {
        auto& s = ctx.studioTools();
        s.autoHeadroom = headroom.getToggleState();
        ctx.setHeadphoneEq (s.eqPreset.load(), s.eqBypass.load());     // preamp worked out again (and saved)
        repaint();
    };
    truePeak.onClick = [this] { ctx.setAutoTruePeak (truePeak.getToggleState(), ctx.engine().getSettings().truePeakCeilingDb); repaint(); };
    tpCeiling.setTooltip ("Plafond de sécurité : -1,0 dBTP (recommandé) ou -0,5 dBTP"_t);
    truePeak.setTooltip ("Auto-Headroom True Peak : baisse seulement les morceaux dont le True Peak dépasse le plafond"_t);
    tpCeiling.onClick = [this]
    {
        const auto st = ctx.engine().getSettings();
        ctx.setAutoTruePeak (st.autoTruePeak, st.truePeakCeilingDb < -0.75 ? -0.5 : -1.0);
        syncControls();
        repaint();
    };
    addAndMakeVisible (tpCeiling);
    upsample.onClick = [this]
    {
        auto st = ctx.engine().getSettings();
        st.upsampleToDxd = upsample.getToggleState();
        ctx.engine().setSettings (st);
        ctx.applyEngineSettings();
    };
    crossfeed.onClick = [this] { ctx.studioTools().crossfeed = crossfeed.getToggleState(); ctx.applyStudioTools(); repaint(); };
    phase.onClick = [this] { ctx.studioTools().invertPhase = phase.getToggleState(); ctx.applyStudioTools(); repaint(); };
    hpEq.onClick = [this]
    {
        auto& s = ctx.studioTools();
        const int preset = s.eqPreset.load() >= 0 ? s.eqPreset.load() : 0;     // first switch-on: Momentum M2
        ctx.setHeadphoneEq (preset, ! hpEq.getToggleState());
        setView (View::Headphone);
    };
    peq.onClick = [this]
    {
        ctx.setParametricEq (ctx.studioTools().parametricBands(), peq.getToggleState(), true);
        setView (View::Parametric);
    };
    e9ds.onClick = [this] { e9Changed (true); setView (View::E9ds); };
    for (auto [row, id] : std::initializer_list<std::pair<int, const char*>> { { Headroom, "headroom" }, { TruePeak, "truepeak" }, { Conversion, "dxd" }, { Crossfeed, "crossfeed" },
                                                                            { HeadphoneEq, "hpeq" }, { E9ds, "e9ds" } })
    {
        infos.emplace_back (row, std::make_unique<InfoButton> (ctx, id));
        addAndMakeVisible (*infos.back().second);
    }
    for (auto* sl : { &e9Focus, &e9Amount })
    {
        sl->setSliderStyle (juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
        sl->setTextBoxIsEditable (true);
        sl->onValueChange = [this] { e9Changed (false); };
        sl->onDragEnd = [this] { e9Changed (true); };
        const auto& t = ctx.theme();
        sl->setColour (juce::Slider::trackColourId, t.accent());
        sl->setColour (juce::Slider::backgroundColourId, t.textCol().withAlpha (0.14f));
        sl->setColour (juce::Slider::thumbColourId, t.textCol());
        sl->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        sl->setColour (juce::Slider::textBoxBackgroundColourId, t.textCol().withAlpha (0.06f));
        sl->setColour (juce::Slider::textBoxTextColourId, t.textCol());
        addChildComponent (sl);
    }
    e9Focus.setRange (E9dsState::minFocus, E9dsState::maxFocus, 10.0);
    e9Focus.textFromValueFunction = [] (double v) { return juce::String (v / 1000.0, 2).replaceCharacter ('.', ',') + " kHz"; };
    e9Focus.valueFromTextFunction = [] (const juce::String& t) { const double v = t.replaceCharacter (',', '.').getDoubleValue(); return v < 100.0 ? v * 1000.0 : v; };
    e9Amount.setRange (-E9dsState::maxAmountDb, 0.0, 0.1);
    e9Amount.textFromValueFunction = [] (double v) { return juce::String (v, 1).replaceCharacter ('.', ',') + " dB"; };
    e9Amount.valueFromTextFunction = [] (const juce::String& t) { return -std::abs (t.replaceCharacter (',', '.').getDoubleValue()); };
    e9Amount.setDoubleClickReturnValue (true, -3.0);
    e9Split.setButtonText ("Split-Band");
    e9Hf.setButtonText ("Dynamic HF");
    e9Sib.setButtonText ("Sibilances 6,5 kHz"_t);
    e9Bril.setButtonText ("Brillance 9 kHz"_t);
    e9Split.setTooltip ("Cloche dynamique sur la bande : seules les sibilances baissent, le reste ne bouge pas."_t);
    e9Hf.setTooltip ("Plateau doux au-dessus de la fréquence, seulement quand les aigus dépassent le seuil."_t);
    e9Split.onClick = [this] { ctx.e9ds().mode = E9dsState::SplitBand; e9Changed (true); };
    e9Hf.onClick = [this] { ctx.e9ds().mode = E9dsState::DynamicHf; e9Changed (true); };
    e9Sib.onClick = [this] { e9Focus.setValue (6500.0, juce::dontSendNotification); e9Changed (true); };
    e9Bril.onClick = [this] { e9Focus.setValue (9000.0, juce::dontSendNotification); e9Changed (true); };
    for (auto* b : { &e9Split, &e9Hf, &e9Sib, &e9Bril }) addChildComponent (b);

    search.setTextToShowWhenEmpty ("Rechercher un casque (ex: HD 650, Momentum M2, Focal...)"_t, ctx.theme().textDim());
    search.setFont (style::font (13.5f));
    search.setIndents (10, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.onTextChange = [this] { refilter(); };
    search.onReturnKey = [this] { for (int r = 0; r < (int) rows.size(); ++r) if (rows[(size_t) r] >= 0) { chooseRow (r); break; } };
    addAndMakeVisible (search);
    list.setModel (this);
    list.setRowHeight (44);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness (0);
    addAndMakeVisible (list);

    addAndMakeVisible (graph);
    graph.onChange = [this] (const hpeq::ParametricBands& b, bool final) { bandsChanged (b, final); };
    for (int i = 0; i < 7; ++i)
    {
        auto* strip = bands.add (new BandStrip (c, i));
        strip->onChange = [this] (int index, const hpeq::Band& band, bool final)
        {
            auto b = ctx.studioTools().parametricBands();
            b[(size_t) index] = band;
            bandsChanged (b, final);
        };
        addChildComponent (strip);
    }
    resetButton.onClick = [this] { bandsChanged (hpeq::flatParametric(), true); syncControls(); };
    addPeqButton.setTooltip ("Un égaliseur paramétrique à vous, en plus du profil casque (en série, 64 bits) : ses bandes ne changent pas quand vous changez de casque."_t);
    addPeqButton.onClick = [this]
    {
        // its own bands (flat the first time, then the user's last ones): never a copy of the headphone curve
        if (! ctx.studioTools().peqActive()) ctx.setParametricEq (ctx.studioTools().parametricBands(), true, true);
        syncControls();
        setView (View::Parametric);
    };
    addChildComponent (addPeqButton);
    copyButton.onClick = [this]
    {
        const auto* p = ctx.studioTools().eqPresetInfo();
        if (p == nullptr) { ctx.showMessage ("Choisissez d'abord un profil casque"_t); return; }
        ctx.setParametricEq (hpeq::toParametric (*p), true, true);
        syncControls();
        ctx.showMessage ("Courbe copiée dans l'égaliseur paramétrique : "_t + p->displayName());
        repaint();
    };
    saveButton.onClick = [this]
    {
        auto* w = new juce::AlertWindow ("Preset de l'égaliseur paramétrique"_t, "Nom du preset :"_t, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", {});
        w->addButton ("Enregistrer"_t, 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Annuler"_t, 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<AuraDspPanel> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            if (safe == nullptr || result != 1) return;
            const auto name = w->getTextEditorContents ("name").trim();
            if (name.isEmpty()) return;
            safe->ctx.saveParametricPreset (name);
            safe->syncControls();
            safe->ctx.showMessage ("Preset enregistré : "_t + name);
        }), true);
    };
    userPresets.setTextWhenNothingSelected ("Presets utilisateur"_t);
    userPresets.onChange = [this]
    {
        if (userPresets.getSelectedId() > 0 && ctx.loadParametricPreset (userPresets.getText())) { syncControls(); repaint(); }
    };
    for (auto* b : std::initializer_list<juce::Component*> { &resetButton, &copyButton, &saveButton, &userPresets }) addChildComponent (b);
    refilter();
    setVisible (false);
}

void AuraDspPanel::open()
{
    syncControls();
    setView (ctx.studioTools().peqActive() && ! ctx.studioTools().eqActive() ? View::Parametric : View::Headphone);
    setVisible (true);
    toFront (true);
    resized();
    startTimerHz (2);           // source / bit-perfect state, and the debounced save while it is open
}

void AuraDspPanel::close()
{
    if (saveQueued) bandsChanged (ctx.studioTools().parametricBands(), true);
    stopTimer();
    setVisible (false);
}

void AuraDspPanel::timerCallback()
{
    if (saveQueued) bandsChanged (ctx.studioTools().parametricBands(), true);
    if (view == View::E9ds && ++e9Ticks % 8 != 0) { repaint (e9Meter); return; }
    syncControls();
    repaint();
}

void AuraDspPanel::bandsChanged (const hpeq::ParametricBands& b, bool final)
{
    // live in the DSP at once; written to settings.json at the end of a gesture (or by the timer)
    ctx.setParametricEq (b, ctx.studioTools().peqActive(), final);
    saveQueued = ! final;
    const auto now = ctx.studioTools().parametricBands();
    for (int i = 0; i < 7; ++i) bands[i]->setBand (now[(size_t) i]);
    graph.repaint();
    repaint (left);
}

void AuraDspPanel::syncControls()
{
    const auto& s = ctx.studioTools();
    headroom.setToggleState (s.autoHeadroom.load(), juce::dontSendNotification);
    upsample.setToggleState (ctx.engine().getSettings().upsampleToDxd, juce::dontSendNotification);
    truePeak.setToggleState (ctx.engine().getSettings().autoTruePeak, juce::dontSendNotification);
    tpCeiling.setButtonText (ctx.engine().getSettings().truePeakCeilingDb < -0.75 ? "-1,0 dBTP" : "-0,5 dBTP");
    crossfeed.setToggleState (s.crossfeed.load(), juce::dontSendNotification);
    hpEq.setToggleState (s.eqActive(), juce::dontSendNotification);
    peq.setToggleState (s.peqActive(), juce::dontSendNotification);
    phase.setToggleState (s.invertPhase.load(), juce::dontSendNotification);
    const auto& e = ctx.e9ds();
    e9ds.setToggleState (e.enabled.load(), juce::dontSendNotification);
    if (! e9Focus.isMouseButtonDown()) e9Focus.setValue (e.focusHz.load(), juce::dontSendNotification);
    if (! e9Amount.isMouseButtonDown()) e9Amount.setValue (-e.amountDb.load(), juce::dontSendNotification);
    e9Split.setToggleState (e.mode.load() == E9dsState::SplitBand, juce::dontSendNotification);
    e9Hf.setToggleState (e.mode.load() == E9dsState::DynamicHf, juce::dontSendNotification);
    addPeqButton.setButtonText (s.peqActive() ? "EQ paramétrique ›"_t : "+ EQ paramétrique"_t);
    if (const bool show = view == View::Headphone && s.eqActive(); show != addPeqButton.isVisible()) { addPeqButton.setVisible (show); resized(); }
    const auto b = s.parametricBands();
    for (int i = 0; i < 7; ++i) bands[i]->setBand (b[(size_t) i]);
    const auto names = ctx.getParametricPresetNames();
    if (names.size() != userPresets.getNumItems())
    {
        userPresets.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i) userPresets.addItem (names[i], i + 1);
    }
}

void AuraDspPanel::e9Changed (bool persist)
{
    auto& e = ctx.e9ds();
    if (persist) ctx.setE9ds (e9ds.getToggleState(), e9Focus.getValue(), -e9Amount.getValue(), e.mode.load());
    else e.set (e9ds.getToggleState(), e9Focus.getValue(), -e9Amount.getValue(), e.mode.load());     // live while dragging
    syncControls();
    repaint();
}

void AuraDspPanel::setView (View v)
{
    view = v;
    const bool p = v == View::Parametric, h = v == View::Headphone, e = v == View::E9ds;
    search.setVisible (h);
    list.setVisible (h);
    graph.setVisible (! e);
    for (auto* c : std::initializer_list<juce::Component*> { &e9Focus, &e9Amount, &e9Split, &e9Hf, &e9Sib, &e9Bril }) c->setVisible (e);
    addPeqButton.setVisible (h && ctx.studioTools().eqActive());
    graph.editable = p;
    for (auto* b : bands) b->setVisible (p);
    for (auto* c : std::initializer_list<juce::Component*> { &resetButton, &copyButton, &saveButton, &userPresets }) c->setVisible (p);
    startTimerHz (e ? 15 : 2);       // E9DS: the reduction meter
    resized();
    repaint();
}

void AuraDspPanel::refilter()
{
    rows.clear();
    const auto q = search.getText().trim();
    for (int studio = 0; studio < 2; ++studio)
    {
        bool header = false;
        for (int i = 0; i < (int) hpeq::presets().size(); ++i)
        {
            const auto& p = hpeq::presets()[(size_t) i];
            if (p.studioCurve != (studio == 1) || ! hpeq::matches (p, q)) continue;
            if (! header) { rows.push_back (studio == 0 ? -1 : -2); header = true; }
            rows.push_back (i);
        }
    }
    list.updateContent();
    list.repaint();
}

int AuraDspPanel::visiblePresetCount() const
{
    return (int) std::count_if (rows.begin(), rows.end(), [] (int r) { return r >= 0; });
}

juce::String AuraDspPanel::visiblePresetName (int n) const
{
    for (int r : rows) if (r >= 0 && n-- == 0) return hpeq::presets()[(size_t) r].displayName();
    return {};
}

void AuraDspPanel::chooseRow (int row)
{
    if (row < 0 || row >= (int) rows.size() || rows[(size_t) row] < 0) return;
    choosePreset (rows[(size_t) row]);
}

void AuraDspPanel::choosePreset (int index)
{
    ctx.setHeadphoneEq (index, false);
    syncControls();
    list.repaint();
    repaint();
}

void AuraDspPanel::chooseMonitor (int index)
{
    using M = StudioToolsState::Monitor;
    const M modes[] = { M::Stereo, M::Mono, M::MidSolo, M::SideSolo };
    ctx.studioTools().monitor = modes[juce::jlimit (0, 3, index)];
    ctx.applyStudioTools();
    repaint();
}

void AuraDspPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool)
{
    if (row < 0 || row >= (int) rows.size()) return;
    const auto& t = ctx.theme();
    const int r = rows[(size_t) row];
    if (r < 0)
    {
        g.setColour (t.textDim());
        g.setFont (style::font (11.5f, true));
        g.drawText (r == -1 ? "CASQUES"_t : "COURBES STUDIO"_t, 6, 0, w - 12, h - 4, juce::Justification::bottomLeft);
        return;
    }
    const auto& p = hpeq::presets()[(size_t) r];
    const bool on = ctx.studioTools().eqPreset.load() == r;
    auto area = juce::Rectangle<float> (0.0f, 2.0f, (float) w, (float) h - 4.0f);
    if (on || list.getRowContainingPosition (list.getMouseXYRelative().x, list.getMouseXYRelative().y) == row)
    {
        g.setColour (on ? t.accent().withAlpha (0.14f) : style::neutral().withAlpha (0.06f));
        g.fillRoundedRectangle (area, 9.0f);
    }
    auto inner = area.reduced (10.0f, 4.0f);
    style::drawIcon (g, p.studioCurve ? style::Icon::Studio : style::Icon::Headphones, inner.removeFromLeft (22.0f).withSizeKeepingCentre (18.0f, 18.0f),
                     on ? t.accent() : t.textDim());
    inner.removeFromLeft (10.0f);
    if (on)
    {
        auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ inner.getRight() - 6.0f, inner.getCentreY() });
        g.setColour (ctx.studioTools().eqActive() ? t.accent() : t.textDim());
        g.fillEllipse (led);
        inner.removeFromRight (18.0f);
    }
    g.setColour (t.textCol());
    g.setFont (style::font (13.5f, true));
    g.drawText (p.studioCurve ? tr (juce::String::fromUTF8 (p.name)) : p.displayName(), inner.removeFromTop (inner.getHeight() * 0.55f), juce::Justification::bottomLeft, true);
    g.setColour (t.textDim());
    g.setFont (style::font (11.5f));
    const juce::String sig = juce::String::fromUTF8 (p.signature);
    g.drawText (sig.isNotEmpty() ? sig : p.studioCurve ? "Courbe studio"_t : juce::String (p.numBands) + " bandes"_t, inner, juce::Justification::topLeft, true);
}

juce::Rectangle<int> AuraDspPanel::panelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (1120, getWidth() - 48), juce::jmin (740, getHeight() - 40));
}

juce::Rectangle<int> AuraDspPanel::rowBounds (int row) const
{
    return { left.getX(), left.getY() + row * rowH, left.getWidth(), rowH };
}

void AuraDspPanel::resized()
{
    auto p = panelBounds().reduced (24, 20);
    closeButton.setBounds (p.removeFromTop (40).removeFromRight (36).withSizeKeepingCentre (32, 32));
    p.removeFromTop (12);
    left = p.removeFromLeft (juce::jmin (340, p.getWidth() / 3));
    juce::ToggleButton* toggles[] = { nullptr, &headroom, &truePeak, &upsample, &crossfeed, &hpEq, &peq, &phase, &e9ds };
    for (int r = 1; r < numRows; ++r) toggles[r]->setBounds (rowBounds (r).removeFromRight (52).withSizeKeepingCentre (48, 28));
    tpCeiling.setBounds (rowBounds (TruePeak).withTrimmedRight (56).removeFromRight (78).withSizeKeepingCentre (78, 26));
    for (auto& [r, b] : infos)
    {
        // "+" right after the row's title (first half of the row, see drawRow)
        auto row = rowBounds (r);
        const int x = row.getX() + 30 + (int) style::textWidth (style::font (14.5f, true), tr (juce::String::fromUTF8 (rowTitles[r]))) + 6;
        b->setBounds (x, row.getY() + row.getHeight() / 2 + 2 - 17, InfoButton::size, InfoButton::size);
    }
    p.removeFromLeft (22);
    right = p;
    auto rr = right;
    rr.removeFromTop (24);                                  // view title
    if (view == View::E9ds)
    {
        rr.removeFromTop (10);
        auto modes = rr.removeFromTop (34).removeFromLeft (320);
        e9Split.setBounds (modes.removeFromLeft (156));
        modes.removeFromLeft (8);
        e9Hf.setBounds (modes);
        rr.removeFromTop (40);                              // "Fréquence" label
        e9Focus.setBounds (rr.removeFromTop (32));
        rr.removeFromTop (22);                              // markers 6.5 / 8-10 kHz
        auto pre = rr.removeFromTop (30).removeFromLeft (360);
        e9Sib.setBounds (pre.removeFromLeft (176));
        pre.removeFromLeft (8);
        e9Bril.setBounds (pre);
        rr.removeFromTop (40);                              // "Quantité" label
        e9Amount.setBounds (rr.removeFromTop (32));
        rr.removeFromTop (44);                              // meter label
        e9Meter = rr.removeFromTop (18);
    }
    else if (view == View::Headphone)
    {
        auto top = rr.removeFromTop (34);
        if (addPeqButton.isVisible()) { addPeqButton.setBounds (top.removeFromRight (190)); top.removeFromRight (8); }
        search.setBounds (top);
        rr.removeFromTop (8);
        graph.setBounds (rr.removeFromBottom (juce::jmin (210, rr.getHeight() / 2)));
        rr.removeFromBottom (8);
        list.setBounds (rr);
    }
    else
    {
        auto buttons = rr.removeFromBottom (32);
        resetButton.setBounds (buttons.removeFromLeft (160));
        buttons.removeFromLeft (8);
        copyButton.setBounds (buttons.removeFromLeft (240));
        buttons.removeFromLeft (8);
        saveButton.setBounds (buttons.removeFromRight (180));
        buttons.removeFromRight (8);
        userPresets.setBounds (buttons);
        rr.removeFromBottom (10);
        auto strips = rr.removeFromBottom (juce::jmin (270, rr.getHeight() / 2 + 30));
        rr.removeFromBottom (10);
        graph.setBounds (rr);
        const int w = (strips.getWidth() - 6 * 6) / 7;
        for (int i = 0; i < 7; ++i) { bands[i]->setBounds (strips.removeFromLeft (w)); strips.removeFromLeft (6); }
    }
}

void AuraDspPanel::mouseUp (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.getPosition())) { close(); return; }
    if (rowBounds (HeadphoneEq).withTrimmedRight (60).contains (e.getPosition())) { setView (View::Headphone); return; }
    if (rowBounds (ParametricEq).withTrimmedRight (60).contains (e.getPosition())) { setView (View::Parametric); return; }
    if (rowBounds (E9ds).withTrimmedRight (60).contains (e.getPosition())) { setView (View::E9ds); return; }
    for (auto& [r, i] : monitorSegs) if (r.contains (e.getPosition())) { chooseMonitor (i); return; }
}

void AuraDspPanel::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    const auto mouse = getMouseXYRelative();
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    auto pf = panelBounds().toFloat();
    juce::Path shape;
    shape.addRoundedRectangle (pf, t.radius() + 4.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.7f), 40, { 0, 16 }).drawForPath (g, shape);
    style::paintGlass (g, *this, ctx, pf, t.radius() + 4.0f, true);

    auto head = panelBounds().reduced (24, 20).removeFromTop (40);
    g.setColour (t.textCol());
    g.setFont (style::font (26.0f, true));
    const auto title = juce::String ("AURA DSP");
    g.drawText (title, head, juce::Justification::centredLeft);
    head.removeFromLeft ((int) style::textWidth (style::font (26.0f, true), title) + 16);
    g.setColour (t.textDim());
    g.setFont (style::font (13.5f));
    g.drawText ("Aura Precision Audio Control · Moteur 64-bit"_t, head, juce::Justification::centredLeft);

    auto& s = ctx.studioTools();
    const auto path = ctx.engine().getSignalPath();
    const bool bitPerfect = ! path.nodes.empty() && path.overall == PathQuality::BitPerfect;
    const double preampDb = s.eqPreampDb.load();
    const auto* preset = s.eqPresetInfo();

    // left: the chain, one row per stage, joined by a line
    auto drawRow = [&] (int i, bool on, const juce::String& detail)
    {
        auto row = rowBounds (i);
        const bool selected = (i == HeadphoneEq && view == View::Headphone) || (i == ParametricEq && view == View::Parametric) || (i == E9ds && view == View::E9ds);
        if (selected || ((i == HeadphoneEq || i == ParametricEq || i == E9ds) && row.contains (mouse)))
        {
            g.setColour (selected ? style::neutral().withAlpha (0.08f) : style::neutral().withAlpha (0.04f));
            g.fillRoundedRectangle (row.toFloat().reduced (0.0f, 3.0f), 10.0f);
        }
        auto dot = juce::Rectangle<float> (10, 10).withCentre ({ (float) row.getX() + 12.0f, (float) row.getCentreY() });
        if (i > 0)
        {
            g.setColour (t.border());
            g.fillRect ((float) row.getX() + 11.5f, (float) row.getY() - (float) rowH / 2.0f + 6.0f, 1.0f, (float) rowH - 12.0f);
        }
        g.setColour (on ? t.accent() : t.textDim().withAlpha (0.5f));
        g.fillEllipse (dot);
        if (on) { g.setColour (t.accent().withAlpha (0.25f)); g.drawEllipse (dot.expanded (3.0f), 2.0f); }
        auto text = row.withTrimmedLeft (30).withTrimmedRight (i == TruePeak ? 140 : 60);
        g.setColour (t.textCol());
        g.setFont (style::font (14.5f, true));
        g.drawText (tr (juce::String::fromUTF8 (rowTitles[i])), text.removeFromTop (text.getHeight() / 2 + 2), juce::Justification::bottomLeft, true);
        g.setColour (t.textDim());
        g.setFont (style::font (12.0f));
        g.drawText (detail, text, juce::Justification::topLeft, true);
    };
    {
        const auto* tr0 = ctx.currentTrack();
        const juce::String src = tr0 != nullptr ? tr0->resolutionBadge() + "  ·  " + ctx.engine().getDeviceName() : juce::String ("Rien en lecture"_t);
        drawRow (Source, bitPerfect, src + (bitPerfect ? "  ·  Bit-perfect"_t : juce::String()));
    }
    const bool anyEq = s.eqActive() || s.peqActive();
    drawRow (Headroom, anyEq && s.autoHeadroom.load(),
             s.autoHeadroom.load() ? (anyEq ? "Automatique : "_t + juce::String (preampDb, 1).replaceCharacter ('.', ',') + " dB, jamais d'écrêtage"_t : juce::String ("Automatique (aucun gain à compenser)"_t))
                                   : juce::String ("Désactivé : attention à l'écrêtage"_t));
    {
        // Auto-Headroom True Peak: the measurement of the track being played and what is applied to it
        const auto st = ctx.engine().getSettings();
        const auto& q = ctx.engine().getQueue();
        const int ci = ctx.engine().getCurrentIndex();
        const double tp = ci >= 0 && ci < (int) q.size() ? q[(size_t) ci].truePeakDb : std::numeric_limits<double>::quiet_NaN();
        const auto num = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (v, 1).replaceCharacter ('.', ','); };
        const bool dsd = ctx.engine().getCurrentPlan().source.isDsd();
        const double att = ctx.engine().getTruePeakAttenuationDb();
        juce::String detail;
        if (! st.autoTruePeak) detail = "True Peak · désactivé"_t;
        else if (ci < 0) detail = "True Peak, plafond "_t + num (st.truePeakCeilingDb);
        else if (! std::isfinite (tp)) detail = "Mesure du True Peak…"_t;
        else if (dsd) detail = "TP " + num (tp) + " dBTP" + " · DSD"_t;
        else detail = "TP " + num (tp) + " dBTP" + juce::String::fromUTF8 ("  \xe2\x86\x92  ") + (att > 0.0 ? num (-att) + " dB" : juce::String ("0 dB"));
        drawRow (TruePeak, st.autoTruePeak && att > 0.0, detail);
    }
    drawRow (Conversion, upsample.getToggleState(), upsample.getToggleState() ? "Sinc polyphase vers DXD 352,8 / 384 kHz"_t : juce::String ("Fréquence d'origine (bit-perfect possible)"_t));
    drawRow (Crossfeed, s.crossfeed.load(), "Bauer bs2b 700 Hz / 4,5 dB, pour le casque"_t);
    drawRow (HeadphoneEq, s.eqActive(), preset != nullptr ? preset->displayName() + (s.eqBypass.load() ? "  ·  contourné"_t : juce::String()) : juce::String ("Choisissez un profil dans la liste"_t));
    {
        int moved = 0;
        for (auto& b : s.parametricBands()) if (b.gain != 0.0) ++moved;
        drawRow (ParametricEq, s.peqActive(), moved > 0 ? juce::String (moved) + " bande(s) réglée(s) sur 7"_t : juce::String ("7 bandes à plat (0 dB)"_t));
    }
    drawRow (Polarity, s.invertPhase.load(), "Inversion de polarité absolue (Ø)"_t);
    {
        const auto& e = ctx.e9ds();
        const juce::String khz = juce::String (e.focusHz.load() / 1000.0, 1).replaceCharacter ('.', ',');
        drawRow (E9ds, e.enabled.load(), e.enabled.load() ? juce::String (e.mode.load() == E9dsState::DynamicHf ? "Dynamic HF" : "Split-Band") + "  ·  " + khz + " kHz  ·  max -"
                                                              + juce::String (e.amountDb.load(), 1).replaceCharacter ('.', ',') + " dB"
                                                        : juce::String ("De-esser transparent, sibilances et aigus durs"_t));
    }

    monitorSegs.clear();
    {
        auto seg = juce::Rectangle<int> (left.getX(), rowBounds (numRows).getY() + 12, left.getWidth(), 32);
        const juce::String names[] = { "Stéréo"_t, "Mono", "Mid", "Side" };
        const int cur = (int) s.monitor.load();
        const int w = seg.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            auto r = seg.removeFromLeft (w).reduced (2, 0);
            const bool on = i == cur;
            g.setColour (on ? t.accent() : style::neutral().withAlpha (r.contains (mouse) ? 0.12f : 0.06f));
            g.fillRoundedRectangle (r.toFloat(), 9.0f);
            g.setColour (on ? t.colour ("--nx-on-accent", juce::Colours::black) : t.textCol());
            g.setFont (style::font (13.0f, true));
            g.drawText (names[i], r, juce::Justification::centred);
            monitorSegs.push_back ({ r, i });
        }
    }

    // right: title of the view
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText (view == View::Headphone ? "PROFILS CASQUE"_t : view == View::E9ds ? "E9DS · DE-ESSER / ADOUCISSEUR D'AIGUS"_t : "ÉGALISEUR PARAMÉTRIQUE · 7 BANDES"_t,
                right.withHeight (20), juce::Justification::centredLeft);
    if (view == View::E9ds)
    {
        const auto& e = ctx.e9ds();
        auto label = [&] (juce::Component& under, const juce::String& text)
        {
            g.setColour (t.textCol());
            g.setFont (style::font (13.5f, true));
            g.drawText (text, under.getBounds().translated (0, -30).withHeight (24), juce::Justification::bottomLeft);
        };
        label (e9Focus, "Fréquence (Focus)"_t);
        label (e9Amount, "Quantité (Amount) : atténuation maximale"_t);
        // markers under the focus slider: sibilance 6.5 kHz, harshness / brilliance 8-10 kHz
        const auto track = e9Focus.getBounds().withTrimmedRight (86).reduced (10, 0);
        auto xOf = [&] (double hz) { return (float) track.getX() + (float) ((hz - E9dsState::minFocus) / (E9dsState::maxFocus - E9dsState::minFocus)) * (float) track.getWidth(); };
        const float y = (float) e9Focus.getBottom() + 2.0f;
        g.setColour (t.accent().withAlpha (0.25f));
        g.fillRoundedRectangle (xOf (8000.0), y, xOf (10000.0) - xOf (8000.0), 5.0f, 2.5f);
        g.setColour (t.accent());
        g.fillRect (xOf (6500.0) - 1.0f, y - 2.0f, 2.0f, 9.0f);
        g.setColour (t.textDim());
        g.setFont (style::font (11.0f));
        g.drawText ("6,5 kHz sibilances"_t, juce::Rectangle<float> (xOf (6500.0) - 80.0f, y + 6.0f, 160.0f, 14.0f), juce::Justification::centred);
        g.drawText ("8-10 kHz brillance"_t, juce::Rectangle<float> (xOf (9000.0) - 80.0f, y + 6.0f, 160.0f, 14.0f), juce::Justification::centred);
        // gain reduction meter (0 ... -6 dB)
        const double gr = e.enabled.load() ? e.reductionDb.load() : 0.0;
        g.setColour (t.textCol());
        g.setFont (style::font (13.5f, true));
        g.drawText ("Réduction : "_t + juce::String (gr >= 0.05 ? -gr : 0.0, 1).replaceCharacter ('.', ',') + " dB", e9Meter.translated (0, -30).withHeight (24), juce::Justification::bottomLeft);
        auto bar = e9Meter.toFloat();
        g.setColour (style::neutral().withAlpha (0.08f));
        g.fillRoundedRectangle (bar, 6.0f);
        g.setColour (t.accent());
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, gr / E9dsState::maxAmountDb)), 6.0f);
        g.setColour (t.textDim());
        g.setFont (style::font (12.0f));
        g.drawFittedText ("Seuil automatique (part des aigus dans le signal). Sous le seuil, la sortie est identique à l'entrée, au bit près."_t,
                          e9Meter.translated (0, 28).withHeight (40), juce::Justification::topLeft, 2);
    }
    if (view == View::Parametric)
        g.drawText ("Poignées : glisser = fréquence / gain, molette = Q"_t, right.withHeight (20), juce::Justification::centredRight);
}

} // namespace nx
