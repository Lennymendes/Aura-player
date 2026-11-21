// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#pragma once

#include <map>
#include "Shortcuts.h"

#include "Background.h"
#include "Sidebar.h"
#include "PlayerBar.h"
#include "NowPlaying.h"
#include "Pages.h"
#include "Marketplace.h"
#include "SettingsWindow.h"
#include "Onboarding.h"
#include "Immersive.h"
#include "AuraDsp.h"

namespace nx
{

class QueuePanel : public juce::Component
{
public:
    explicit QueuePanel (AppContext& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { scroll = juce::jmax (0.0f, scroll - w.deltaY * 200.0f); repaint(); }
private:
    AppContext& ctx;
    float scroll = 0.0f;
};

/** Header: audio state at a glance. "● Bit-Perfect" (lit when the stream is untouched) opens AURA DSP;
    "● MOMENTUM M2 CL" shows the headphone EQ profile, a click bypasses it or turns it back on. */
class AudioStatusChip : public juce::Component, private juce::Timer
{
public:
    explicit AudioStatusChip (AppContext& c) : ctx (c) { setRepaintsOnMouseActivity (true); startTimerHz (4); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int idealWidth() const;
    juce::Rectangle<int> eqArea() const;
    bool isBitPerfect() const;
    juce::String eqLabel() const;
    bool eqOn() const;
private:
    void timerCallback() override;
    AppContext& ctx;
    juce::String shown;
};

class Toast : public juce::Component, private juce::Timer
{
public:
    explicit Toast (AppContext& c) : ctx (c) { setInterceptsMouseClicks (false, false); }
    void show (const juce::String& text);
    void paint (juce::Graphics&) override;
    juce::String message;
private:
    void timerCallback() override;
    AppContext& ctx;
    float alpha = 0.0f;
    double shownAt = 0.0;
};

class MainComponent : public juce::Component, public juce::DragAndDropContainer, private juce::Timer, private juce::FocusChangeListener
{
public:
    explicit MainComponent (AppContext& ctx);
    /** ‹ › enabled state and tooltips (with the current Page précédente / suivante shortcuts). */
    void updateNavButtons();
    IconButton& getBackButton() noexcept { return backButton; }
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override {}
    bool keyPressed (const juce::KeyPress&) override;
    void performShortcut (shortcuts::Action a);

    /** Opens a page and records it in the history (‹ / › buttons). */
    void navigate (const PageRequest& r);
    void goBack();
    void goForward();
    bool canGoBack() const    { return historyPos > 0; }
    bool canGoForward() const { return historyPos + 1 < (int) history.size(); }
    void setSidebarVisible (bool visible, bool animate = true);
    bool isSidebarVisible() const { return sidebarVisible; }
    void refreshCurrentPage();
    void trackChanged();
    void stateChanged();
    void themeChanged();
    void rebuildPagesLater();
    void pluginsChanged();
    void showMessage (const juce::String& text) { placeToast(); toast.show (text); }
    void setLyrics (const Lyrics& l, const juce::String& status) { nowPlaying.setLyrics (l, status); immersive.setLyrics (l, status); }
    void showSignalPath();
    void showRemote();
    void openSettings (const juce::String& tab);
    void showOnboarding();
    void setImmersive (bool on);
    bool isImmersive() const { return immersive.isVisible(); }
    void languageChanged();
    SettingsWindow& getSettingsWindow() { return settingsWindow; }
    OnboardingWizard& getOnboarding() { return onboarding; }
    ImmersiveView& getImmersive() { return immersive; }
    ListingPage& getListing() { return listing; }
    int rememberedScroll (const PageRequest& r) const { auto it = scrollMemory.find (viewKey (r)); return it == scrollMemory.end() ? 0 : it->second; }
    PageRequest getCurrentPage() const { return current; }
    MarketplacePage& getMarketplace() { return marketplace; }

    BackgroundComponent background;
    Sidebar sidebar;
    PlayerBar player;

    IconButton sidebarButton, backButton, forwardButton, remoteButton, settingsButton, streamerButton;
    AudioStatusChip audioStatus;
    AuraDspPanel auraDsp;
    int playerHeight() const;
    void showAuraDsp() { auraDsp.open(); auraDsp.setBounds (getLocalBounds()); auraDsp.grabKeyboardFocus(); }

    /** Side mouse buttons (4 = back, 5 = forward) anywhere in the window. */
    struct SideButtons : juce::MouseListener
    {
        std::function<void (bool)> go;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isBackButtonDown()) go (false);
            else if (e.mods.isForwardButtonDown()) go (true);
        }
    } sideButtons;
private:
    void open (const PageRequest& r);
    static bool isLocalPage (PageId p) noexcept;
    void rememberLocalView();
    void placeToast();
    void showPage (juce::Component* page);
    void timerCallback() override;
    void globalFocusChanged (juce::Component* focused) override;
    struct Animator : juce::Timer { std::function<void()> tick; void timerCallback() override { tick(); } };
    Animator sidebarAnim;
    float sidebarAmount = 1.0f;     // 0 hidden .. 1 shown (animated)
    bool sidebarVisible = true;
    std::vector<PageRequest> history;
    int historyPos = -1;
    float fadeProgress = 1.0f;
    AppContext& ctx;
    ListingPage listing;
    NowPlayingPage nowPlaying;
    SourcePage sourcePage;
    MarketplacePage marketplace;
    QueuePanel queue;
    Toast toast;
    std::unique_ptr<PluginSlot> homeSlot;
    SettingsWindow settingsWindow;
    ImmersiveView immersive;
    OnboardingWizard onboarding;
    juce::TooltipWindow tooltips { this, 650 };
    juce::Component* visiblePage = nullptr;
    PageRequest current;
    PageRequest localView;            // last page of the Local family (album, artist, grid...) and its scroll
    int localScroll = 0, pendingScroll = -1;
    std::map<juce::String, int> scrollMemory;   // vertical scroll of each listing page, by page + arguments
    bool restoreScroll = false;                 // the next page opened (Back, Forward, Local) gets its remembered scroll back
    static juce::String viewKey (const PageRequest& r) { return juce::String ((int) r.page) + "|" + r.a + "|" + r.b; }
    bool queueVisible = false;
    bool immersiveMadeFullScreen = false;
};

} // namespace nx
