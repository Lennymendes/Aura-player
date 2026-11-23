// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "StudioView.h"
#include "Waveform.h"
#include "../Ext/AudioAnalyzer.h"
#include "../DSP/E9ds.h"
#include "../DSP/SmartComfort.h"

namespace nx
{

namespace
{
    juce::String num (double v, int d = 1) { return juce::String (v, d).replaceCharacter ('.', ','); }
    juce::Colour heat (float v)                       // 0..1 -> black, violet, orange, white
    {
        v = juce::jlimit (0.0f, 1.0f, v);
        if (v < 0.4f) return juce::Colour (0xff05040a).interpolatedWith (juce::Colour (0xff5b2a9e), v / 0.4f);
        if (v < 0.75f) return juce::Colour (0xff5b2a9e).interpolatedWith (juce::Colour (0xffff8a3d), (v - 0.4f) / 0.35f);
        return juce::Colour (0xffff8a3d).interpolatedWith (juce::Colours::white, (v - 0.75f) / 0.25f);
    }
    constexpr double fLow = 20.0;
}

StudioView::StudioView (AppContext& c) : closeButton (style::Icon::Close, "Fermer (Échap)"_t, c), ctx (c)
{
    setWantsKeyboardFocus (true);
    setVisible (false);
    closeButton.onClick = [this] { close(); };
    addAndMakeVisible (closeButton);
}

void StudioView::open()
{
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    startTimerHz (30);
    repaint();
}

void StudioView::close()
{
    if (! isVisible()) return;
    stopTimer();
    setVisible (false);
    if (auto* p = getParentComponent()) p->grabKeyboardFocus();
}

void StudioView::resized()
{
    panel = getLocalBounds().withSizeKeepingCentre (juce::jmin (1180, getWidth() - 48), juce::jmin (780, getHeight() - 40));
    closeButton.setBounds (panel.getRight() - 52, panel.getY() + 14, 36, 36);
    auto r = panel.reduced (24, 20);
    r.removeFromTop (54);
    left = r.removeFromLeft (juce::jmin (320, r.getWidth() / 3));
    r.removeFromLeft (20);
    spectrumArea = r.removeFromTop (r.getHeight() * 56 / 100);
    r.removeFromTop (14);
    sonoArea = r;
    if (sono.getWidth() != juce::jmax (1, sonoArea.getWidth()) || sono.getHeight() != juce::jmax (1, sonoArea.getHeight()))
    {
        sono = juce::Image (juce::Image::RGB, juce::jmax (1, sonoArea.getWidth()), juce::jmax (1, sonoArea.getHeight()), true);
        juce::Graphics (sono).fillAll (heat (0.0f));
        columns = 0;
    }
}

float StudioView::xForHz (double hz, juce::Rectangle<float> r) const
{
    const double nyq = juce::jmax (22050.0, ctx.analyzer().getSampleRate() * 0.5);
    return r.getX() + r.getWidth() * (float) (std::log (juce::jlimit (fLow, nyq, hz) / fLow) / std::log (nyq / fLow));
}

void StudioView::timerCallback()
{
    if (! isVisible()) return;                          // (opened, maybe in an off-screen window)
    scrollSpectrogram();
    repaint (panel);
}

void StudioView::scrollSpectrogram()
{
    if (! sono.isValid() || ctx.analyzer().isDsdDensity()) return;
    const auto& fine = ctx.analyzer().getFineSpectrum();
    const int w = sono.getWidth(), h = sono.getHeight();
    sono.moveImageSection (0, 0, 1, 0, w - 1, h);
    const double rate = ctx.analyzer().getSampleRate(), nyq = juce::jmax (22050.0, rate * 0.5);
    const int n = (int) fine.size();
    for (int y = 0; y < h; ++y)
    {
        const double hz = fLow * std::pow (nyq / fLow, 1.0 - (double) y / (double) juce::jmax (1, h - 1));
        const int bin = juce::jlimit (0, juce::jmax (0, n - 1), (int) (hz / rate * AudioAnalyzer::fftSize));
        const float db = n > 0 ? fine[(size_t) bin] : -140.0f;
        sono.setPixelAt (w - 1, y, heat ((db + 120.0f) / 110.0f));
    }
    ++columns;
}

juce::StringArray StudioView::metricLines() const
{
    juce::StringArray out;
    const auto a = ctx.currentAnalysis();
    if (a == nullptr) { out.add ("Analyse|" + juce::String ("Mesure en cours (ou flux non mesurable)"_t)); return out; }
    out.add ("Dynamic Range|DR" + juce::String (a->dr));
    out.add ("True Peak|" + juce::String (a->truePeakDb > 0.0 ? "+" : "") + num (a->truePeakDb) + " dBTP");
    out.add ("Peak|" + num (a->peakDb) + " dBFS");
    out.add ("RMS|" + num (a->rmsDb) + " dBFS");
    const auto r = comfort::classify (a->dr, a->hfShareDb);
    out.add ("Profil de mastering"_t + "|" + comfort::profileName (r.profile));
    out.add ("Aigus 6-10 kHz"_t + "|" + num (a->hfShareDb) + " dB");
    out.add ("Hi-Res|" + hires::describe (a->hiRes));
    return out;
}

double StudioView::spectrumPeakHz() const
{
    const auto& fine = ctx.analyzer().getFineSpectrum();
    int best = 1;
    for (int i = 1; i < (int) fine.size(); ++i) if (fine[(size_t) i] > fine[(size_t) best]) best = i;
    return best * ctx.analyzer().getSampleRate() / AudioAnalyzer::fftSize;
}

bool StudioView::showsHiResMark() const { return ctx.analyzer().getSampleRate() * 0.5 > 22050.0 + 1.0; }

void StudioView::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    g.fillAll (juce::Colours::black.withAlpha (0.6f));
    juce::Path p;
    p.addRoundedRectangle (panel.toFloat(), 14.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 28, { 0, 10 }).drawForPath (g, p);
    g.setColour (t.surface());
    g.fillPath (p);
    g.setColour (t.border());
    g.strokePath (p, juce::PathStrokeType (1.0f));

    const auto* tr = ctx.currentTrack();
    g.setColour (t.textCol());
    g.setFont (style::font (20.0f, true));
    g.drawText ("Studio View", panel.getX() + 24, panel.getY() + 16, 200, 28, juce::Justification::centredLeft);
    g.setColour (t.textDim());
    g.setFont (style::font (13.0f));
    g.drawText (tr != nullptr ? tr->displayArtist() + juce::String::fromUTF8 (" \xe2\x80\x94 ") + tr->title + "  " + juce::String::fromUTF8 ("\xc2\xb7") + "  " + tr->resolutionBadge()
                              : juce::String ("Rien en lecture"_t),
                panel.getX() + 170, panel.getY() + 18, panel.getWidth() - 240, 24, juce::Justification::centredLeft, true);

    // left: the measures of the track
    auto l = left;
    const auto a = ctx.currentAnalysis();
    {
        auto big = l.removeFromTop (92);
        g.setColour (style::neutral().withAlpha (0.05f));
        g.fillRoundedRectangle (big.toFloat(), 12.0f);
        const int dr = a != nullptr ? a->dr : 0;
        g.setColour (dr <= 0 ? t.textDim() : dr >= 12 ? juce::Colour (0xff3ecf8e) : dr >= 8 ? juce::Colour (0xffe6c547) : juce::Colour (0xffff5a5f));
        g.setFont (style::font (44.0f, true));
        g.drawText (dr > 0 ? "DR" + juce::String (dr) : juce::String ("DR --"), big.reduced (16, 0).removeFromLeft (150), juce::Justification::centredLeft);
        g.setColour (t.textDim());
        g.setFont (style::font (12.0f));
        g.drawFittedText ("Dynamic Range\n(méthode TT DR)"_t, big.reduced (16, 0).removeFromRight (120), juce::Justification::centredRight, 2);
    }
    l.removeFromTop (10);
    for (auto& line : metricLines())
    {
        if (line.startsWith ("Dynamic Range")) continue;
        auto row = l.removeFromTop (line.startsWith ("Hi-Res") ? 58 : 30);
        g.setColour (t.textDim());
        g.setFont (style::font (12.5f));
        g.drawText (line.upToFirstOccurrenceOf ("|", false, false), row.removeFromLeft (120), juce::Justification::topLeft);
        const auto value = line.fromFirstOccurrenceOf ("|", false, false);
        g.setColour (line.startsWith ("Hi-Res") && a != nullptr && a->hiRes.verdict == hires::Fake ? juce::Colour (0xffff8a3d)
                     : line.startsWith ("Hi-Res") && a != nullptr && a->hiRes.verdict == hires::Real ? juce::Colour (0xff3ecf8e) : t.textCol());
        g.setFont (style::font (13.0f, true));
        g.drawFittedText (value, row, juce::Justification::topLeft, 3);
    }
    // live meters
    l.removeFromTop (8);
    auto& an = ctx.analyzer();
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText ("EN DIRECT · PEAK / RMS"_t, l.removeFromTop (20), juce::Justification::centredLeft);
    for (int c = 0; c < 2; ++c)
    {
        auto row = l.removeFromTop (22).reduced (0, 4);
        g.setColour (t.textDim());
        g.setFont (style::font (12.0f));
        g.drawText (c == 0 ? "L" : "R", row.removeFromLeft (16), juce::Justification::centredLeft);
        auto bar = row.removeFromLeft (row.getWidth() - 80).toFloat();
        auto pos = [&] (float lin) { return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (lin, -60.0f) + 60.0f) / 60.0f); };
        g.setColour (style::neutral().withAlpha (0.08f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (t.accent().withAlpha (0.55f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * pos (an.getRms (c))), 3.0f);
        const float px = bar.getX() + bar.getWidth() * pos (an.getPeak (c));
        g.setColour (an.getPeak (c) >= 0.999f ? juce::Colour (0xffff5a5f) : t.textCol());
        g.fillRect (px - 1.0f, bar.getY(), 2.0f, bar.getHeight());
        g.setColour (t.textDim());
        g.drawText (num (juce::Decibels::gainToDecibels (an.getPeak (c), -99.0f)) + " dB", row, juce::Justification::centredRight);
    }

    // right: spectrum
    const auto sr = spectrumArea.toFloat();
    g.setColour (style::neutral().withAlpha (0.04f));
    g.fillRoundedRectangle (sr, 10.0f);
    const auto plot = sr.reduced (10.0f, 22.0f).withTrimmedBottom (-8.0f);
    const double rate = an.getSampleRate(), nyq = juce::jmax (22050.0, rate * 0.5);
    g.setFont (style::font (11.0f));
    for (const double f : { 100.0, 1000.0, 10000.0, 20000.0, 40000.0, 80000.0 })
    {
        if (f > nyq) continue;
        const float x = xForHz (f, plot);
        g.setColour (t.border());
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (t.textDim());
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f), (int) x + 3, (int) plot.getBottom() - 14, 40, 14, juce::Justification::left);
    }
    for (int dbLine = -120; dbLine <= 0; dbLine += 20)
    {
        const float y = plot.getY() + plot.getHeight() * (float) (-dbLine) / 130.0f;
        g.setColour (t.border().withAlpha (0.5f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (t.textDim());
        g.drawText (juce::String (dbLine), (int) plot.getX() + 2, (int) y - 13, 40, 12, juce::Justification::left);
    }
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText ("SPECTRE EN DIRECT · 0 - "_t + num (nyq / 1000.0) + " kHz", spectrumArea.reduced (12, 4).removeFromTop (16), juce::Justification::centredLeft);

    // E9DS: where it works and how much it takes now
    const auto& e = ctx.e9ds();
    const double dose = ctx.engine().getComfortDoseDb();
    if (e.enabled.load() || dose > 0.0)
    {
        const bool manual = e.enabled.load();
        const double focus = manual ? e.focusHz.load() : comfort::focusHz;
        const bool shelf = ! manual || e.mode.load() == E9dsState::DynamicHf;
        const float x0 = xForHz (shelf ? focus : focus / 1.41, plot), x1 = shelf ? plot.getRight() : xForHz (focus * 1.41, plot);
        g.setColour (juce::Colour (0xff47b5ff).withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, plot.getHeight()));
        g.setColour (juce::Colour (0xff47b5ff));
        g.setFont (style::font (11.5f, true));
        g.drawText ((manual ? juce::String ("E9DS") : juce::String ("E9DS auto")) + "  " + num (-e.reductionDb.load()) + " dB",
                    (int) x0 + 4, (int) plot.getY() + 2, 180, 14, juce::Justification::left);
    }
    if (an.isDsdDensity())
    {
        g.setColour (t.textDim());
        g.setFont (style::font (13.0f));
        g.drawText ("Flux DSD natif : densité 1 bit, pas de spectre PCM à afficher"_t, spectrumArea, juce::Justification::centred);
    }
    else
    {
        const auto& fine = an.getFineSpectrum();
        juce::Path curve;
        const int n = (int) fine.size();
        bool started = false;
        for (int px = 0; px < (int) plot.getWidth(); px += 2)
        {
            const double hz = fLow * std::pow (nyq / fLow, (double) px / plot.getWidth());
            const int bin = juce::jlimit (0, juce::jmax (0, n - 1), (int) (hz / rate * AudioAnalyzer::fftSize));
            const float db = n > 0 ? fine[(size_t) bin] : -140.0f;
            const float y = plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 130.0f);
            if (! started) { curve.startNewSubPath (plot.getX() + (float) px, y); started = true; }
            else curve.lineTo (plot.getX() + (float) px, y);
        }
        juce::Path fill (curve);
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (t.accent().withAlpha (0.35f), 0, plot.getY(), t.accent().withAlpha (0.02f), 0, plot.getBottom(), false));
        g.fillPath (fill);
        g.setColour (t.accent());
        g.strokePath (curve, juce::PathStrokeType (1.4f));
    }
    if (showsHiResMark())
    {
        const float x = xForHz (22050.0, plot);
        g.setColour (juce::Colour (0xffff8a3d));
        const float dashes[] = { 5.0f, 4.0f };
        g.drawDashedLine (juce::Line<float> (x, plot.getY(), x, plot.getBottom()), dashes, 2, 1.3f);
        g.setFont (style::font (11.5f, true));
        g.drawText ("22,05 kHz", (int) x + 4, (int) plot.getY() + 18, 80, 14, juce::Justification::left);
    }

    // spectrogram
    g.drawImage (sono, sonoArea.toFloat());
    g.setColour (t.border());
    g.drawRect (sonoArea);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.setFont (style::font (11.5f, true));
    g.drawText ("SPECTROGRAMME"_t, sonoArea.reduced (8, 4).removeFromTop (16), juce::Justification::centredLeft);
    if (showsHiResMark())
    {
        const float y = (float) sonoArea.getY() + (float) sonoArea.getHeight() * (float) (1.0 - std::log (22050.0 / fLow) / std::log (nyq / fLow));
        g.setColour (juce::Colour (0xffff8a3d).withAlpha (0.8f));
        g.drawHorizontalLine ((int) y, (float) sonoArea.getX(), (float) sonoArea.getRight());
        g.drawText ("22,05 kHz", sonoArea.getRight() - 84, (int) y - 15, 80, 14, juce::Justification::right);
    }
}

} // namespace nx
