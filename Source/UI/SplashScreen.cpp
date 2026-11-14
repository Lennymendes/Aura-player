// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "SplashScreen.h"
#include "../GUI/Logo.h"

namespace nx
{

namespace
{
    float easeOut (float x) { x = juce::jlimit (0.0f, 1.0f, x); return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
}

SplashScreen::SplashScreen (juce::Component& parent, std::function<void()> done) : onFinished (std::move (done))
{
    setOpaque (false);
    parent.addAndMakeVisible (this);
    setBounds (parent.getLocalBounds());
    toFront (false);
    startMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

SplashScreen::~SplashScreen() { stopTimer(); }

void SplashScreen::timerCallback()
{
    elapsed = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    const float p = (float) progress();
    setInterceptsMouseClicks (p < 0.7f, false);   // the interface is usable while the splash fades out
    setAlpha (p < 0.7f ? 1.0f : 1.0f - easeOut ((p - 0.7f) / 0.3f));
    repaint();
    if (p >= 1.0f && ! finished)
    {
        finished = true;
        stopTimer();
        setVisible (false);
        if (onFinished) onFinished();
    }
}

void SplashScreen::paint (juce::Graphics& g)
{
    const auto bg = juce::Colour (0xff07050a), blue = juce::Colour (0xff2563eb);
    const float p = (float) progress();
    const float in = easeOut (p / 0.45f);                 // opening fade + slight zoom of the logo and the name
    auto r = getLocalBounds().toFloat();

    g.fillAll (bg);
    // soft Aura blue glow behind the logo
    const auto c = r.getCentre().translated (0.0f, -24.0f);
    g.setGradientFill (juce::ColourGradient (blue.withAlpha (0.16f * in), c.x, c.y, blue.withAlpha (0.0f), c.x + 320.0f, c.y, true));
    g.fillEllipse (juce::Rectangle<float> (640.0f, 640.0f).withCentre (c));

    const float size = 120.0f * (0.92f + 0.08f * in);
    g.setOpacity (in);
    logo::draw (g, juce::Rectangle<float> (size, size).withCentre (c));

    // "AURA": letter-spaced, rising slightly into place
    const juce::String name ("AURA");
    const auto font = juce::Font (juce::FontOptions (28.0f, juce::Font::bold)).withExtraKerningFactor (0.32f);
    g.setFont (font);
    g.setColour (juce::Colours::white.withAlpha (0.94f * in));
    const float textY = c.y + 92.0f + 10.0f * (1.0f - in);
    g.drawText (name, juce::Rectangle<float> (r.getWidth(), 40.0f).withPosition (r.getX(), textY), juce::Justification::centred);

    // thin progress line in the Aura blue
    const float w = 160.0f, y = textY + 58.0f;
    g.setColour (juce::Colours::white.withAlpha (0.08f * in));
    g.fillRoundedRectangle (c.x - w * 0.5f, y, w, 2.0f, 1.0f);
    g.setColour (blue.withAlpha (in));
    g.fillRoundedRectangle (c.x - w * 0.5f, y, w * juce::jmin (1.0f, p / 0.7f), 2.0f, 1.0f);
}

} // namespace nx
