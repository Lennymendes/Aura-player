// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Pages.h"
#include <random>
#include <algorithm>
#include "../Engine/DeviceProfiles.h"
#include "DeviceArt.h"
#include "Logo.h"
#include "Shortcuts.h"
#include "../Core/LicenseManager.h"
#include "../Core/UpdateChecker.h"
#include "../Library/Downloads.h"
#include "../Providers/StreamingProviders.h"
#include "../Library/LibraryScanner.h"
#include "../Ext/PluginManager.h"
#include "../Platform/AuraBridge.h"
#include "../Platform/SystemMonitor.h"
#include "../Remote/RemoteServer.h"
#include "../DSP/StudioTools.h"
#include "LibraryActions.h"
#include "Dashboard.h"
#include "Appearance.h"
#include "NXPlayerData.h"

namespace nx
{

//==============================================================================
// ListingPage
//==============================================================================
ListingPage::ViewBar::ViewBar (ListingPage& p)
    : viewToggle (style::Icon::List, "Vue liste / grille"_t, p.ctx), page (p)
{
    for (auto* b : { &small, &medium, &large })
    {
        b->setClickingTogglesState (false);
        b->setTooltip ("Taille des pochettes"_t);
        addAndMakeVisible (b);
    }
    small.onClick = [this] { page.ctx.settings().set ("albumSize", "S"); page.refresh(); };
    medium.onClick = [this] { page.ctx.settings().set ("albumSize", "M"); page.refresh(); };
    large.onClick = [this] { page.ctx.settings().set ("albumSize", "L"); page.refresh(); };
    addAndMakeVisible (viewToggle);
    viewToggle.onClick = [this]
    {
        const bool list = page.ctx.settings().get ("albumView", "grid").toString() == "list";
        page.ctx.settings().set ("albumView", list ? "grid" : "list");
        page.refresh();
    };
    sort.addItem ("Tri par défaut"_t, 1);
    sort.addItem ("Ordre alphabétique (titre)"_t, 2);
    sort.addItem ("Artiste"_t, 3);
    sort.addItem ("Année"_t, 4);
    sort.addItem ("Date d'ajout"_t, 5);
    sort.addItem ("Format / résolution (DXD, DSD, PCM)"_t, 6);
    sort.setTooltip ("Trier les albums"_t);
    addAndMakeVisible (sort);
    sort.onChange = [this]
    {
        static const char* modes[] = { "", "", "title", "artist", "year", "added", "format" };
        page.ctx.settings().set ("albumSort", modes[juce::jlimit (1, 6, sort.getSelectedId())]);
        page.refresh();
    };
}

void ListingPage::ViewBar::sync()
{
    const auto size = page.ctx.settings().get ("albumSize", "M").toString();
    small.setToggleState (size == "S", juce::dontSendNotification);
    medium.setToggleState (size != "S" && size != "L", juce::dontSendNotification);
    large.setToggleState (size == "L", juce::dontSendNotification);
    for (auto* b : { &small, &medium, &large }) b->setColour (juce::TextButton::buttonColourId, b->getToggleState() ? page.ctx.theme().accent().withAlpha (0.35f) : juce::Colours::transparentWhite);
    const bool list = page.ctx.settings().get ("albumView", "grid").toString() == "list";
    viewToggle.setIcon (list ? style::Icon::GridView : style::Icon::List);
    const auto mode = page.ctx.settings().get ("albumSort", "").toString();
    sort.setSelectedId (mode == "title" ? 2 : mode == "artist" ? 3 : mode == "year" ? 4 : mode == "added" ? 5 : mode == "format" ? 6 : 1, juce::dontSendNotification);
}

void ListingPage::ViewBar::resized()
{
    auto r = getLocalBounds();
    sort.setBounds (r.removeFromRight (190).reduced (0, 2));
    r.removeFromRight (8);
    viewToggle.setBounds (r.removeFromRight (32).reduced (2));
    r.removeFromRight (8);
    for (auto* b : { &large, &medium, &small }) { b->setBounds (r.removeFromRight (30).reduced (1, 3)); }
}

void ListingPage::refresh()
{
    // sort mode before the loader (setAlbums sorts), size and list view after it (the loader decides what the grid shows)
    grid.sortMode = ctx.settings().get ("albumSort", "").toString();
    if (loader) loader (*this);
    const auto size = ctx.settings().get ("albumSize", "M").toString();
    const bool albumsGrid = grid.libraryAlbums && ! grid.roundArt && showViewBar;
    grid.setCardSize (albumsGrid ? (size == "S" ? 130.0f : size == "L" ? 240.0f : 180.0f) : 0.0f);   // S / M / L
    grid.setListMode (albumsGrid && ctx.settings().get ("albumView", "grid").toString() == "list");
    if (showViewBar) viewBar.sync();
    layoutContent (getWidth() - viewport.getScrollBarThickness());
}

ListingPage::ListingPage (AppContext& c) : ScrollPage (c), grid (c), list (c), pills (c), heroArt (c, 640),
                                           editButton (style::Icon::Edit, "Modifier l'album (nom, artiste, année, pochette)"_t, c)
{
    contentComp.addChildComponent (viewBar);
    contentComp.addChildComponent (editButton);
    editButton.onClick = [this]
    {
        AlbumInfo a;
        a.artist = heroArtist;
        a.title = title;
        if (! heroTracks.empty()) a.year = heroTracks.front().year;
        actions::editAlbum (ctx, a);
    };
    grid.onEdit = [this] (const AlbumInfo& a) { actions::editAlbum (ctx, a); };
    contentComp.addAndMakeVisible (header);
    // the header spans the whole title area and takes the clicks: the controls drawn over it must sit above it
    viewBar.toFront (false);
    editButton.toFront (false);
    contentComp.addChildComponent (pills);
    contentComp.addChildComponent (artistHeader);
    pills.onSelect = [this] (const juce::String& id) { ctx.navigate ({ PageId::Library, id }); };
    contentComp.addAndMakeVisible (grid);
    contentComp.addAndMakeVisible (list);
    contentComp.addChildComponent (heroArt);
    contentComp.addChildComponent (playAll);
    contentComp.addChildComponent (shuffleAll);
    contentComp.addChildComponent (downloadAll);
    contentComp.addChildComponent (cta);
    downloadAll.setTooltip ("Copier ces morceaux du serveur dans le dossier de téléchargement, pour l'écoute hors-ligne"_t);
    downloadAll.onClick = [this]
    {
        const int n = ctx.downloads().enqueue (heroTracks);
        ctx.showMessage (n > 0 ? "Téléchargement de "_t + juce::String (n) + " morceau(x)" : juce::String ("Déjà disponible hors-ligne"_t));
    };
    cta.setToggleState (true, juce::dontSendNotification);
    cta.onClick = [this] { if (ctaAction) ctaAction(); };
    playAll.setToggleState (true, juce::dontSendNotification);
    playAll.onClick = [this]
    {
        if (collection.isNotEmpty()) ctx.playCollection (collection, false);
        else if (! heroTracks.empty()) ctx.playTracks (heroTracks, 0);
    };
    shuffleAll.onClick = [this]
    {
        if (collection.isNotEmpty()) { ctx.playCollection (collection, true); return; }
        auto t = heroTracks;
        std::mt19937 rng { std::random_device{}() };
        std::shuffle (t.begin(), t.end(), rng);
        if (! t.empty()) ctx.playTracks (t, 0);
    };
    grid.onOpen = [this] (const AlbumInfo& a) { ctx.navigate ({ PageId::AlbumDetail, a.artist, a.title }); };
    grid.onPlay = [this] (const AlbumInfo& a) { ctx.playTracks (ctx.library().getAlbumTracks (a.artist, a.title), 0); };
    ctx.artwork().addChangeListener (this);
}

ListingPage::~ListingPage() { ctx.artwork().removeChangeListener (this); }

void ListingPage::Header::paint (juce::Graphics& g)
{
    const auto& t = page.ctx.theme();
    auto r = getLocalBounds();
    if (page.hero)
    {
        r.removeFromLeft (page.heroArt.getWidth() + 28);
        r.removeFromBottom (54);
        g.setColour (t.textDim());
        g.setFont (style::font (12.0f, true));
        g.drawText ("ALBUM"_t, r.removeFromTop (r.getHeight() - 96), juce::Justification::bottomLeft);
        g.setColour (t.textCol());
        g.setFont (style::font (34.0f, true));
        g.drawFittedText (page.title, r.removeFromTop (44), juce::Justification::bottomLeft, 1);
        g.setColour (t.accent());
        g.setFont (style::font (18.0f, true));
        {
            const auto line = page.subtitle.upToFirstOccurrenceOf ("\n", false, false);
            auto ar = r.removeFromTop (26);
            const int aw = juce::jmin (ar.getWidth(), juce::roundToInt (style::textWidth (style::font (18.0f, true), line)) + 2);
            artistRect = page.heroArtist.isNotEmpty() ? ar.withWidth (aw) : juce::Rectangle<int>();
            g.drawText (line, ar, juce::Justification::centredLeft, true);
        }
        g.setColour (t.textDim());
        g.setFont (style::font (13.0f));
        g.drawText (page.subtitle.fromFirstOccurrenceOf ("\n", false, false), r.removeFromTop (22), juce::Justification::centredLeft, true);
        return;
    }
    drawPageTitle (g, t, r, page.title, page.subtitle);
}

void ListingPage::layoutContent (int width)
{
    const int pad = 32;
    const int inner = juce::jmax (100, width - pad * 2);
    int y = 24;

    heroArt.setVisible (hero);
    playAll.setVisible (hasPlayRow());
    shuffleAll.setVisible (hasPlayRow());
    viewBar.setVisible (showViewBar && ! hero && artistName.isEmpty());
    editButton.setVisible (hero);
    editButton.pro = ! ctx.isPremium();
    bool remote = false;
    for (auto& t : heroTracks) remote = remote || DownloadManager::canDownload (t);
    downloadAll.setVisible (remote && playAll.isVisible());
    artistHeader.setVisible (artistName.isNotEmpty());
    header.setVisible (artistName.isEmpty());
    if (artistName.isNotEmpty())
    {
        const int h = artistHeader.heightForWidth (inner);
        artistHeader.setBounds (pad, y, inner, h);
        y += h + 12;
        if (hasPlayRow())
        {
            playAll.setBounds (pad, y, 120, 36);
            shuffleAll.setBounds (pad + 132, y, 130, 36);
            downloadAll.setBounds (pad + 274, y, 140, 36);
            y += 56;
        }
    }
    else if (hero)
    {
        const int art = juce::jmin (240, inner / 3);
        heroArt.setArtwork (heroArtwork, title);
        heroArt.setBounds (pad, y, art, art);
        header.setBounds (pad, y, inner, art);
        playAll.setBounds (pad + art + 28, y + art - 44, 120, 38);
        shuffleAll.setBounds (pad + art + 160, y + art - 44, 130, 38);
        downloadAll.setBounds (pad + art + 302, y + art - 44, 140, 38);
        editButton.setBounds (pad + art + (downloadAll.isVisible() ? 454 : 302), y + art - 44, 38, 38);
        y += art + 28;
    }
    else
    {
        header.setBounds (pad, y, inner, subtitle.isNotEmpty() ? 74 : 54);
        if (viewBar.isVisible()) viewBar.setBounds (pad + inner - 430, y + 8, 430, 34);
        y += header.getHeight() + 8;
        pills.setVisible (showPills);
        if (showPills)
        {
            pills.setBounds (pad, y, inner, 38);
            y += 50;
        }
        if (hasPlayRow())
        {
            playAll.setBounds (pad, y, 120, 36);
            shuffleAll.setBounds (pad + 132, y, 130, 36);
            downloadAll.setBounds (pad + 274, y, 140, 36);
            y += 52;
        }
    }
    if (hero || artistName.isNotEmpty()) pills.setVisible (false);

    cta.setVisible (ctaText.isNotEmpty() && ! hero);
    if (cta.isVisible())
    {
        cta.setButtonText (ctaText);
        cta.setBounds (pad, y, 260, 40);
        y += 60;
    }

    if (topSlot != nullptr && ! topSlot->isEmpty())
    {
        if (topSlot->getParentComponent() != &contentComp) contentComp.addAndMakeVisible (topSlot);
        const int h = juce::jmin (220, topSlot->getPreferredHeight());
        topSlot->setBounds (pad, y, inner, h);
        y += h + 20;
    }

    if (artistName.isNotEmpty() && grid.getAlbums().size() > 0)
    {
        struct Caption : juce::Component
        {
            ListingPage& page;
            explicit Caption (ListingPage& p) : page (p) { setInterceptsMouseClicks (false, false); }
            void paint (juce::Graphics& g) override
            {
                g.setColour (page.ctx.theme().textCol());
                g.setFont (style::font (20.0f, true));
                g.drawText ("Discographie dans la bibliothèque"_t, getLocalBounds(), juce::Justification::centredLeft);
            }
        };
        if (contentComp.findChildWithID ("disco") == nullptr)
        {
            auto* c = new Caption (*this);
            c->setComponentID ("disco");
            contentComp.addAndMakeVisible (c);   // owned by contentComp's children list via deleter below
            captionOwner.reset (c);
        }
        if (auto* c = contentComp.findChildWithID ("disco")) { c->setVisible (true); c->setBounds (pad, y, inner, 32); }
        y += 40;
    }
    else if (auto* c = contentComp.findChildWithID ("disco")) c->setVisible (false);

    const int gh = grid.heightForWidth (inner);
    grid.setBounds (pad, y, inner, gh);
    y += gh + (gh > 0 ? 16 : 0);

    list.setBounds (pad - 10, y, inner + 20, list.getTotalHeight());
    y += list.getTotalHeight() + 120;
    setContentHeight (y);
}

//==============================================================================
ListingPage::ArtistHeader::ArtistHeader (ListingPage& p) : page (p)
{
    page.ctx.artistInfo().addChangeListener (this);
    page.ctx.artwork().addChangeListener (this);
}

ListingPage::ArtistHeader::~ArtistHeader()
{
    page.ctx.artistInfo().removeChangeListener (this);
    page.ctx.artwork().removeChangeListener (this);
}

void ListingPage::ArtistHeader::setArtist (const juce::String& n, int albums, int tracks)
{
    if (n != name) expanded = false;
    name = n;
    albumCount = albums;
    trackCount = tracks;
    meta = page.ctx.artistInfo().get (name);
    repaint();
}

void ListingPage::ArtistHeader::changeListenerCallback (juce::ChangeBroadcaster* b)
{
    if (b == &page.ctx.artistInfo() && name.isNotEmpty())
    {
        auto m = page.ctx.artistInfo().get (name, false);
        const bool changed = m.has_value() != meta.has_value() || (m && meta && (m->bio != meta->bio || m->imageUrl != meta->imageUrl));
        meta = m;
        if (changed) page.layoutContent (page.getWidth() - page.viewport.getScrollBarThickness());
    }
    repaint();
}

int ListingPage::ArtistHeader::heightForWidth (int width) const
{
    int h = 300;   // banner + name block
    if (hasBio())
    {
        juce::AttributedString as;
        as.append (meta->bio, style::font (14.5f));
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) juce::jmin (width, 900));
        const float full = tl.getHeight();
        h += 18 + (int) (expanded ? full : juce::jmin (full, 4.0f * 22.0f)) + 34;
    }
    return h;
}

void ListingPage::ArtistHeader::mouseUp (const juce::MouseEvent& e)
{
    if (moreButton.contains (e.getPosition()))
    {
        expanded = ! expanded;
        page.layoutContent (page.getWidth() - page.viewport.getScrollBarThickness());
    }
}

void ListingPage::ArtistHeader::paint (juce::Graphics& g)
{
    const auto& t = page.ctx.theme();
    auto r = getLocalBounds();
    auto banner = r.removeFromTop (240).toFloat();
    // banner: the artist's fanart, else the blurred photo / album art, under a dark gradient
    juce::Path clip;
    clip.addRoundedRectangle (banner, t.radius());
    g.saveState();
    g.reduceClipRegion (clip);
    juce::Image img;
    if (meta && meta->bannerUrl.isNotEmpty()) img = page.ctx.artwork().get (meta->bannerUrl, 1280);
    if (! img.isValid() && meta && meta->imageUrl.isNotEmpty()) img = page.ctx.artwork().get (meta->imageUrl, 640);
    if (! img.isValid() && ! page.grid.getAlbums().empty()) img = page.ctx.artwork().get (page.grid.getAlbums().front().artworkKey, 640);
    if (img.isValid())
    {
        const float scale = juce::jmax (banner.getWidth() / (float) img.getWidth(), banner.getHeight() / (float) img.getHeight());
        const float w = (float) img.getWidth() * scale, h = (float) img.getHeight() * scale;
        g.setOpacity (0.9f);
        g.drawImage (img, juce::Rectangle<float> (w, h).withCentre (banner.getCentre()).withY (banner.getY() - (h - banner.getHeight()) * 0.3f));
    }
    else
    {
        g.setGradientFill (juce::ColourGradient (t.accent().withAlpha (0.5f), banner.getX(), banner.getY(), t.accent2().withAlpha (0.3f), banner.getRight(), banner.getBottom(), false));
        g.fillRect (banner);
    }
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.05f), 0, banner.getY(), t.bg().withAlpha (0.92f), 0, banner.getBottom(), false));
    g.fillRect (banner);
    g.restoreState();
    if (meta)   // attribution of the online visuals
    {
        const auto fromAdb = [] (const juce::String& u) { return u.containsIgnoreCase ("theaudiodb.com"); };
        const auto credit = fromAdb (meta->bannerUrl) || fromAdb (meta->imageUrl) ? "Source : TheAudioDB.com"_t
                          : meta->imageUrl.containsIgnoreCase ("wikimedia.org") ? "Photo : Wikimedia Commons (Wikipédia)"_t : juce::String();
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (style::font (11.0f));
        g.drawText (credit, banner.reduced (14.0f, 8.0f), juce::Justification::topRight);
    }

    // round photo, overlapping the banner
    const float d = 150.0f;
    auto photo = juce::Rectangle<float> (d, d).withPosition (banner.getX() + 28.0f, banner.getBottom() - d * 0.62f);
    juce::Path circle;
    circle.addEllipse (photo);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 20, { 0, 6 }).drawForPath (g, circle);
    juce::Image face;
    if (meta && meta->imageUrl.isNotEmpty()) face = page.ctx.artwork().get (meta->imageUrl, 320);
    if (! face.isValid() && ! page.grid.getAlbums().empty()) face = page.ctx.artwork().get (page.grid.getAlbums().front().artworkKey, 320);
    if (! face.isValid()) face = imaging::placeholder (name, 160, t.accent(), t.accent2());
    g.saveState();
    g.reduceClipRegion (circle);
    g.setOpacity (1.0f);
    const float side = (float) juce::jmin (face.getWidth(), face.getHeight());
    g.drawImage (face, photo.getX(), photo.getY(), photo.getWidth(), photo.getHeight(),
                 (int) (((float) face.getWidth() - side) * 0.5f), (int) (((float) face.getHeight() - side) * 0.2f), (int) side, (int) side);
    g.restoreState();
    g.setColour (t.bg());
    g.drawEllipse (photo, 4.0f);

    // name + meta
    auto text = juce::Rectangle<float> (photo.getRight() + 24.0f, banner.getBottom() - 92.0f, banner.getRight() - photo.getRight() - 40.0f, 92.0f);
    g.setColour (t.textDim());
    g.setFont (style::font (12.0f, true));
    g.drawText ("ARTISTE"_t, text.removeFromTop (18), juce::Justification::centredLeft);
    g.setColour (t.textCol());
    g.setFont (style::font (38.0f, true));
    g.drawFittedText (name, text.removeFromTop (46).toNearestInt(), juce::Justification::centredLeft, 1);
    juce::StringArray facts;
    if (meta && meta->genre.isNotEmpty()) facts.add (meta->genre);
    if (meta && meta->country.isNotEmpty()) facts.add (meta->country);
    facts.add (juce::String (albumCount) + " album(s) · "_t + juce::String (trackCount) + " morceau(x)"_t);
    g.setColour (t.accent());
    g.setFont (style::font (14.0f, true));
    g.drawText (facts.joinIntoString ("  ·  "_t), text, juce::Justification::centredLeft, true);

    r.removeFromTop (60);
    moreButton = {};
    if (hasBio())
    {
        auto bioArea = r.withWidth (juce::jmin (r.getWidth(), 900)).toFloat();
        juce::AttributedString as;
        as.append (meta->bio, style::font (14.5f), t.textCol().withAlpha (0.88f));
        as.setWordWrap (juce::AttributedString::byWord);
        as.setLineSpacing (3.0f);
        juce::TextLayout tl;
        tl.createLayout (as, bioArea.getWidth());
        const float full = tl.getHeight();
        const float shown = expanded ? full : juce::jmin (full, 4.0f * 22.0f);
        g.saveState();
        g.reduceClipRegion (bioArea.withHeight (shown).toNearestInt());
        tl.draw (g, bioArea.withHeight (full));
        g.restoreState();
        if (! expanded && full > shown)
        {
            g.setGradientFill (juce::ColourGradient (t.bg().withAlpha (0.0f), 0, bioArea.getY() + shown - 30.0f, t.bg().withAlpha (0.85f), 0, bioArea.getY() + shown, false));
            g.fillRect (bioArea.withY (bioArea.getY() + shown - 30.0f).withHeight (30.0f));
        }
        auto under = juce::Rectangle<int> ((int) bioArea.getX(), (int) (bioArea.getY() + shown + 8.0f), (int) bioArea.getWidth(), 22);
        if (full > 4.0f * 22.0f)
        {
            moreButton = under.withWidth (140);
            g.setColour (t.accent());
            g.setFont (style::font (13.0f, true));
            g.drawText (expanded ? "Réduire"_t : "Lire la suite"_t, moreButton, juce::Justification::centredLeft);
        }
        g.setColour (t.textDim());
        g.setFont (style::font (11.5f));
        g.drawText (meta->bioSource == "Wikipedia" ? "Source : Wikipédia — Texte sous licence CC BY-SA 4.0"_t : "Source : TheAudioDB.com"_t,
                    under, juce::Justification::centredRight);
    }
}

//==============================================================================
// SourcePage
//==============================================================================
SourcePage::SourcePage (AppContext& c) : ScrollPage (c) {}

juce::TextEditor* SourcePage::addField (const juce::String& label, const juce::String& key, bool password)
{
    auto* ed = new juce::TextEditor();
    ed->setTextToShowWhenEmpty (label, ctx.theme().textDim());
    ed->setFont (style::font (14.0f));
    ed->setIndents (12, 0);
    ed->setJustification (juce::Justification::centredLeft);
    if (password) ed->setPasswordCharacter ((juce::juce_wchar) 0x2022);
    else ed->setText (ctx.providers().getCredentials().get (sourceId, key), false);
    ed->onTextChange = [this, ed, key, password] { if (! password) ctx.providers().getCredentials().set (sourceId, key, ed->getText().trim()); };
    ed->setName (key);
    rows.add (ed);
    rowHeights.push_back (36);
    contentComp.addAndMakeVisible (ed);
    return ed;
}

juce::TextButton* SourcePage::addButton (const juce::String& text, std::function<void()> fn)
{
    auto* b = new juce::TextButton (text);
    b->onClick = std::move (fn);
    rows.add (b);
    rowHeights.push_back (38);
    contentComp.addAndMakeVisible (b);
    return b;
}

void SourcePage::addText (const juce::String& text, bool dim)
{
    auto* l = new juce::Label ({}, text);
    l->setFont (style::font (14.0f));
    l->setColour (juce::Label::textColourId, dim ? ctx.theme().textDim() : ctx.theme().textCol());
    l->setJustificationType (juce::Justification::topLeft);
    l->setMinimumHorizontalScale (1.0f);
    rows.add (l);
    rowHeights.push_back (juce::jmax (24, 22 * (1 + text.length() / 110 + juce::StringArray::fromLines (text).size() - 1)));
    contentComp.addAndMakeVisible (l);
}

void SourcePage::setSource (const juce::String& id, const juce::String& arg)
{
    sourceId = id;
    sourceArg = arg;
    rows.clear();
    rowHeights.clear();
    tracks.reset();
    albums.reset();
    contentComp.removeAllChildren();

    if (id == "local") buildLocal();
    else if (id == "upnp") buildUpnp();
    else if (auto* p = ctx.providers().getProvider (id)) buildProvider (*p);
    layoutContent (getWidth() - viewport.getScrollBarThickness());
    repaint();
}

void SourcePage::buildLocal()
{
    heading = "Fichiers locaux";
    description = "Disques internes, disques externes et partages réseau montés (SMB / NFS / AFP). Les dossiers sont surveillés : "_t
                  "ajouts, suppressions et modifications apparaissent tout seuls."_t;
    for (auto& f : ctx.library().getFolders())
    {
        auto* row = addButton ("Retirer"_t + "  " + f, nullptr);
        row->onClick = [this, f]
        {
            ctx.library().removeFolder (juce::File (f));
            ctx.library().notifyChanged();
            setSource ("local");
        };
    }
    addButton ("Ajouter un dossier…"_t, [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Dossier de musique", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser] (const juce::FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (! dir.isDirectory()) return;
            ctx.library().addFolder (dir);
            ctx.scanner().requestFullScan();
            ctx.showMessage ("Indexation de " + dir.getFileName() + "…"_t);
            setSource ("local");
        });
    });
    addButton ("Réindexer maintenant"_t, [this] { ctx.scanner().requestFullScan(); ctx.showMessage ("Réindexation lancée"_t); });
    const auto& pr = ctx.scanner().getProgress();
    addText (juce::String (ctx.library().countTracks ("local")) + " morceaux indexés"_t + (pr.scanning.load() ? " · indexation en cours…"_t : ""));
    albums = std::make_unique<AlbumGrid> (ctx);
    albums->setAlbums (ctx.library().getAlbums ("artist", 2000, "local"));
    albums->onOpen = [this] (const AlbumInfo& a) { ctx.navigate ({ PageId::AlbumDetail, a.artist, a.title }); };
    albums->onPlay = [this] (const AlbumInfo& a) { ctx.playTracks (ctx.library().getAlbumTracks (a.artist, a.title), 0); };
    contentComp.addAndMakeVisible (*albums);
}

juce::String SourcePage::upnpArg (const juce::String& udn, const juce::String& objectId, const juce::StringArray& path)
{
    return udn + "\x1f" + objectId + "\x1f" + path.joinIntoString ("\x1e");
}

void SourcePage::buildUpnp()
{
    heading = "NAS (UPnP / DLNA)";
    description = "Serveurs de musique du réseau local (MinimServer, Asset, Synology, QNAP, JRiver, Jellyfin…). Les morceaux sont lus en flux HTTP "_t
                  "par le moteur d'Aura, donc en bit-perfect. Pour un partage SMB, ajoutez simplement le dossier monté dans Fichiers locaux."_t;
    auto* upnp = dynamic_cast<UpnpProvider*> (ctx.providers().getProvider ("upnp"));
    if (upnp == nullptr) return;
    juce::Component::SafePointer<SourcePage> safe (this);
    auto* app = &ctx;

    const auto parts = juce::StringArray::fromTokens (sourceArg, "\x1f", {});
    if (sourceArg.isNotEmpty() && parts.size() >= 2)
    {
        // ---- inside a server folder -----------------------------------------------------------
        const auto udn = parts[0], objectId = parts[1];
        const auto path = juce::StringArray::fromTokens (parts[2], "\x1e", {});
        UpnpServer server;
        for (auto& sv : upnp->getServers()) if (sv.udn == udn) server = sv;
        if (server.udn.isEmpty()) { addText ("Ce serveur n'est plus disponible : relancez la recherche."_t); return; }
        heading = path.isEmpty() ? server.name : path[path.size() - 1];
        description = path.size() > 1 ? path.joinIntoString ("  ›  "_t, 0, path.size() - 1) : server.name;

        const auto key = udn + "|" + objectId;
        auto it = upnpCache.find (key);
        if (it == upnpCache.end())
        {
            addText ("Chargement du dossier…"_t);
            if (! upnpLoading.contains (key))
            {
                upnpLoading.add (key);
                const auto arg = sourceArg;
                juce::Thread::launch ([safe, upnp, server, objectId, key, arg]
                {
                    auto entries = upnp->browseAll (server, objectId);
                    const auto err = upnp->lastError;
                    juce::MessageManager::callAsync ([safe, entries, key, arg, err]
                    {
                        if (safe == nullptr) return;
                        safe->upnpLoading.removeString (key);
                        safe->upnpCache[key] = entries;
                        if (entries.empty() && err.isNotEmpty()) safe->ctx.showMessage (err);
                        if (safe->sourceId == "upnp" && safe->sourceArg == arg) safe->setSource ("upnp", arg);
                    });
                });
            }
            return;
        }

        const auto& entries = it->second;
        std::vector<TrackInfo> items;
        std::vector<AlbumInfo> folders;
        for (auto& e : entries)
        {
            if (! e.isContainer) { items.push_back (e.track); continue; }
            AlbumInfo a;
            a.title = e.title;
            a.artist = e.track.artist.isNotEmpty() ? e.track.artist
                     : (e.childCount >= 0 ? juce::String (e.childCount) + " éléments"_t : juce::String ("Dossier"));
            a.artworkKey = e.track.artworkKey;
            a.source = e.id;
            folders.push_back (a);
        }

        addButton (items.empty() ? juce::String ("Tout lire (sous-dossiers compris)"_t) : juce::String ("Lire ce dossier"), [safe, upnp, server, objectId, app]
        {
            app->showMessage ("Préparation de la lecture…"_t);
            juce::Thread::launch ([safe, upnp, server, objectId, app]
            {
                auto all = upnp->collectTracks (server, objectId, 5000);
                juce::MessageManager::callAsync ([safe, all, app]
                {
                    if (safe == nullptr) return;
                    if (all.empty()) app->showMessage ("Aucun morceau dans ce dossier"_t);
                    else app->playTracks (all, 0);
                });
            });
        });
        addButton ("Télécharger pour l'écoute hors-ligne"_t, [safe, upnp, server, objectId, app]
        {
            app->showMessage ("Préparation du téléchargement…"_t);
            juce::Thread::launch ([safe, upnp, server, objectId, app]
            {
                auto all = upnp->collectTracks (server, objectId, 5000);
                juce::MessageManager::callAsync ([safe, all, app]
                {
                    if (safe == nullptr) return;
                    const int n = app->downloads().enqueue (all);
                    app->showMessage (n > 0 ? "Téléchargement de "_t + juce::String (n) + " morceau(x) vers "_t + app->downloads().getFolder().getFullPathName()
                                            : juce::String ("Rien à télécharger (déjà hors-ligne ?)"_t));
                });
            });
        });
        addButton ("Importer dans la bibliothèque"_t, [safe, upnp, server, objectId, app]
        {
            auto* lib = &app->library();
            juce::Thread::launch ([safe, app, lib, upnp, server, objectId]
            {
                const int n = upnp->importContainer (*lib, server, objectId);
                juce::MessageManager::callAsync ([safe, app, n] { if (safe != nullptr) app->showMessage (juce::String (n) + " morceaux importés depuis le NAS"_t); });
            });
        });
        addText (juce::String ((int) folders.size()) + " dossier(s) · "_t + juce::String ((int) items.size()) + " morceau(x)"_t);

        if (! folders.empty())
        {
            albums = std::make_unique<AlbumGrid> (ctx);
            albums->setAlbums (folders);
            albums->onOpen = [this, udn, path] (const AlbumInfo& a)
            {
                auto deeper = path;
                deeper.add (a.title);
                ctx.navigate ({ PageId::Source, "upnp", upnpArg (udn, a.source, deeper) });
            };
            albums->onPlay = [safe, upnp, server, app] (const AlbumInfo& a)
            {
                const auto id = a.source;
                juce::Thread::launch ([safe, upnp, server, id, app]
                {
                    auto all = upnp->collectTracks (server, id, 5000);
                    juce::MessageManager::callAsync ([safe, all, app] { if (safe != nullptr && ! all.empty()) app->playTracks (all, 0); });
                });
            };
            contentComp.addAndMakeVisible (*albums);
        }
        if (! items.empty())
        {
            tracks = std::make_unique<TrackList> (ctx);
            tracks->setTracks (items, true, true, true);
            contentComp.addAndMakeVisible (*tracks);
        }
        return;
    }

    // ---- server list ----------------------------------------------------------------------------
    auto searchNetwork = [safe, upnp, app]
    {
        juce::Thread::launch ([safe, upnp, app]
        {
            const auto found = upnp->discover (3000);
            const auto err = upnp->lastError;
            juce::MessageManager::callAsync ([safe, app, found, err]
            {
                if (safe == nullptr) return;
                app->saveNasServers();
                app->showMessage (found.empty() ? err : juce::String ((int) found.size()) + " serveur(s) trouvé(s)"_t);
                safe->autoDiscoveryRunning = false;
                if (safe->sourceId == "upnp" && safe->sourceArg.isEmpty()) safe->setSource ("upnp");
            });
        });
    };
    addButton ("Rechercher les serveurs du réseau"_t, [this, searchNetwork] { autoDiscoveryRunning = true; searchNetwork(); });
    // First visit of the NAS page in this session: look for servers automatically (only now, not at startup).
    if (! autoDiscoveryDone && upnp->getServers().empty())
    {
        autoDiscoveryDone = autoDiscoveryRunning = true;
        searchNetwork();
    }
    if (autoDiscoveryRunning) addText ("Recherche des serveurs UPnP / DLNA du réseau…"_t);

    auto* address = addField ("Adresse IP du NAS ou URL de description (ex. 192.168.1.20)"_t, "__nas_address", true);
    address->setPasswordCharacter (0);
    auto connect = [safe, upnp, app, address]
    {
        auto text = address->getText().trim();
        if (text.isEmpty()) return;
        app->showMessage ("Connexion à "_t + text + "…"_t);
        juce::Thread::launch ([safe, upnp, app, text]
        {
            const auto before = upnp->getServers().size();
            bool ok;
            if (text.containsChar ('/')) ok = upnp->addServerFromLocation (text.startsWith ("http") ? text : "http://" + text);
            else ok = upnp->discoverHost (text.upToFirstOccurrenceOf (":", false, false)).size() > before;
            const auto err = upnp->lastError;
            juce::MessageManager::callAsync ([safe, app, ok, err]
            {
                if (safe == nullptr) return;
                app->saveNasServers();
                app->showMessage (ok ? juce::String ("Serveur ajouté"_t) : err);
                safe->setSource ("upnp");
            });
        });
    };
    address->onReturnKey = connect;
    addButton ("Ajouter ce serveur"_t, connect);

    const auto servers = upnp->getServers();
    if (servers.empty() && ! autoDiscoveryRunning)
    {
        addText ("Aucun serveur trouvé pour l'instant. Vérifiez que le serveur UPnP / DLNA du NAS est activé (MinimServer, "_t
                 "Synology Media Server, Asset…) ; si votre réseau filtre la découverte automatique, entrez l'adresse IP du NAS ci-dessus."_t);
        return;
    }
    std::vector<AlbumInfo> cards;
    for (auto& sv : servers)
    {
        AlbumInfo a;
        a.title = sv.name;
        a.artist = juce::URL (sv.location).getDomain();
        a.artworkKey = sv.iconUrl;
        a.source = sv.udn;
        cards.push_back (a);
    }
    albums = std::make_unique<AlbumGrid> (ctx);
    albums->setAlbums (cards);
    albums->onOpen = [this] (const AlbumInfo& a) { ctx.navigate ({ PageId::Source, "upnp", upnpArg (a.source, "0", { a.title }) }); };
    albums->onPlay = albums->onOpen;
    contentComp.addAndMakeVisible (*albums);
}

void SourcePage::buildProvider (MediaProvider& p)
{
    heading = p.getDisplayName();
    const auto id = p.getId();
    if (id == "qobuz")
    {
        description = "FLAC jusqu'à 24 bits / 192 kHz, lu par le moteur bit-perfect d'Aura (changement automatique de fréquence du DAC). "_t
                      "L'API Qobuz demande des identifiants d'application partenaire (app_id / app_secret) délivrés par Qobuz."_t;
        if (CredentialStore::builtIn ("qobuz", "app_id").isEmpty())
        {
            addField ("app_id (partenaire Qobuz)", "app_id");
            addField ("app_secret", "app_secret", false);
        }
        auto* email = addField ("E-mail du compte Qobuz", "email");
        auto* password = addField ("Mot de passe", "password_input", true);
        addButton (p.isSignedIn() ? "Se reconnecter" : "Se connecter", [this, &p, email, password]
        {
            juce::String err;
            if (static_cast<QobuzProvider&> (p).signIn (email->getText(), password->getText(), err)) ctx.showMessage ("Qobuz connecté"_t);
            else ctx.showMessage ("Qobuz : " + err);
            setSource ("qobuz");
        });
    }
    else if (id == "tidal")
    {
        description = "FLAC (et FLAC Max jusqu'à 24 / 192 en DASH) lu par le moteur bit-perfect. Connexion par code sur link.tidal.com "_t
                      "avec un client_id délivré par Tidal (programme développeurs). Les flux chiffrés (DRM) ne sont pas pris en charge."_t;
        if (CredentialStore::builtIn ("tidal", "client_id").isEmpty())
        {
            addField ("client_id (Tidal)", "client_id");
            addField ("client_secret (facultatif)", "client_secret");
        }
        addButton (p.isSignedIn() ? "Se reconnecter" : "Se connecter", [this, &p]
        {
            auto& tidal = static_cast<TidalProvider&> (p);
            TidalProvider::DeviceLogin login;
            juce::String err;
            if (! tidal.beginDeviceLogin (login, err)) { ctx.showMessage ("Tidal : " + err); return; }
            juce::URL (login.verificationUri).launchInDefaultBrowser();
            ctx.showMessage ("Tidal : entrez le code " + login.userCode + " sur " + login.verificationUri);
            juce::Component::SafePointer<SourcePage> safe (this);
            juce::Thread::launch ([safe, &tidal, login]
            {
                for (int i = 0; i * login.interval < login.expiresIn; ++i)
                {
                    juce::Thread::sleep (login.interval * 1000);
                    juce::String e;
                    if (tidal.pollDeviceLogin (login, e))
                    {
                        juce::MessageManager::callAsync ([safe] { if (safe) { safe->ctx.showMessage ("Tidal connecté"_t); safe->setSource ("tidal"); } });
                        return;
                    }
                    if (e != "authorization_pending" && e != "slow_down") break;
                }
            });
        });
    }
    else if (id == "spotify")
    {
        description = "Recherche, bibliothèque et playlists via l'API Web officielle de Spotify (connexion PKCE dans votre navigateur, "_t
                      "redirection http://127.0.0.1:43821/callback). La lecture est confiée à votre appareil Spotify Connect : Spotify ne "_t
                      "délivre pas de flux audio aux applications tierces, ces morceaux ne passent donc pas par le moteur bit-perfect."_t;
        if (CredentialStore::builtIn ("spotify", "client_id").isEmpty())
            addField ("client_id (developer.spotify.com, redirection http://127.0.0.1:43821/callback)", "client_id");
        addButton (p.isSignedIn() ? "Se reconnecter" : "Se connecter (navigateur)", [this, &p]
        {
            juce::Component::SafePointer<SourcePage> safe (this);
            juce::Thread::launch ([safe, &p]
            {
                juce::String err;
                const bool ok = static_cast<SpotifyProvider&> (p).signIn (err);
                juce::MessageManager::callAsync ([safe, ok, err] { if (safe) { safe->ctx.showMessage (ok ? juce::String ("Spotify connecté"_t) : "Spotify : " + err); safe->setSource ("spotify"); } });
            });
        });
    }
    else if (id == "youtube")
    {
        description = "Recherche et playlists via l'API YouTube Data v3 (clé d'API Google). La lecture s'ouvre sur music.youtube.com : "_t
                      "les conditions de YouTube interdisent d'extraire l'audio, ces morceaux ne passent donc pas par le moteur."_t;
        addField ("Clé d'API YouTube Data v3"_t, "api_key");
        addField ("Identifiants de playlists publiques (séparés par des virgules)"_t, "playlists");
    }

    addText ("État : "_t + p.getStatusText() + (p.lastError.isNotEmpty() ? "  ·  "_t + p.lastError : juce::String()), false);

    if (p.isSignedIn())
    {
        juce::Component::SafePointer<SourcePage> safe (this);
        auto* provider = &p;
        if (id == "spotify")
        {
            // Spotify browser: search, liked tracks, playlists. Double-click plays through Spotify Connect.
            auto* spotify = static_cast<SpotifyProvider*> (provider);
            auto showTracks = [safe] (std::vector<TrackInfo> list, juce::String error, juce::String what)
            {
                juce::MessageManager::callAsync ([safe, list = std::move (list), error, what]
                {
                    if (safe == nullptr || safe->tracks == nullptr) return;
                    safe->tracks->setTracks (list, true, true);
                    safe->ctx.showMessage (list.empty() ? (error.isNotEmpty() ? error : what + " : aucun morceau"_t)
                                                        : what + " : " + juce::String ((int) list.size()) + " morceaux"_t);
                    safe->layoutContent (safe->getWidth() - safe->viewport.getScrollBarThickness());
                });
            };

            auto* search = addField ("Rechercher sur Spotify (Entrée)"_t, "__search", true);
            search->setPasswordCharacter (0);
            search->onReturnKey = [search, spotify, showTracks]
            {
                const auto q = search->getText().trim();
                if (q.isEmpty()) return;
                juce::Thread::launch ([spotify, showTracks, q] { auto r = spotify->searchTracks (q, 30); showTracks (std::move (r), spotify->lastError, "« "_t + q + " »"_t); });
            };

            auto* lists = new juce::ComboBox();
            lists->setTextWhenNothingSelected ("Chargement de vos playlists…"_t);
            rows.add (lists);
            rowHeights.push_back (36);
            contentComp.addAndMakeVisible (lists);
            auto playlistIds = std::make_shared<juce::StringArray>();
            lists->onChange = [lists, spotify, showTracks, playlistIds]
            {
                const int idx = lists->getSelectedItemIndex();
                if (idx < 0) return;
                const auto name = lists->getText();
                const auto plId = (*playlistIds)[idx];
                juce::Thread::launch ([spotify, showTracks, plId, name]
                {
                    auto r = plId == "__liked" ? spotify->getLikedTracks() : spotify->getPlaylistTracks (plId);
                    showTracks (std::move (r), spotify->lastError, name);
                });
            };
            juce::Component::SafePointer<juce::ComboBox> safeList (lists);
            juce::Thread::launch ([spotify, safeList, playlistIds, showTracks]
            {
                auto pls = spotify->getPlaylists();
                const auto err = spotify->lastError;
                auto liked = spotify->getLikedTracks (200);
                const auto likedErr = spotify->lastError;
                juce::MessageManager::callAsync ([safeList, playlistIds, pls, err]
                {
                    if (safeList == nullptr) return;
                    safeList->clear (juce::dontSendNotification);
                    playlistIds->clear();
                    safeList->addItem ("Titres likés"_t, 1);
                    playlistIds->add ("__liked");
                    for (auto& pl : pls)
                    {
                        safeList->addItem (pl.name + (pl.trackCount > 0 ? "  (" + juce::String (pl.trackCount) + ")" : juce::String()), playlistIds->size() + 1);
                        playlistIds->add (pl.externalId);
                    }
                    safeList->setTextWhenNothingSelected (pls.empty() && err.isNotEmpty() ? err : juce::String ("Choisir une playlist…"_t));
                });
                showTracks (std::move (liked), likedErr, "Titres likés"_t);
            });
            addText ("Double-cliquez sur un morceau : il est lancé sur votre appareil Spotify (application Spotify ouverte, "_t
                     "compte Premium nécessaire pour le pilotage à distance ; sinon le morceau s'ouvre dans Spotify)."_t);
        }
        addButton ("Importer mes playlists dans la bibliothèque"_t, [safe, provider]
        {
            if (safe == nullptr) return;
            juce::Thread::launch ([safe, provider]
            {
                const auto lists = provider->getPlaylists();
                juce::MessageManager::callAsync ([safe, provider, lists]
                {
                    if (safe == nullptr) return;
                    for (auto& pl : lists)
                        safe->ctx.providers().importPlaylist (provider->getId(), pl, [safe, name = pl.name] (int n)
                        {
                            if (safe) safe->ctx.showMessage (name + " : " + juce::String (n) + " morceaux");
                        });
                    if (lists.empty()) safe->ctx.showMessage (provider->lastError.isNotEmpty() ? provider->lastError : juce::String ("Aucune playlist"_t));
                });
            });
        });
        tracks = std::make_unique<TrackList> (ctx);
        if (id != "spotify") tracks->setTracks (ctx.library().getTracks ("artist", 3000, id), true, true);
        contentComp.addAndMakeVisible (*tracks);
    }
}

void SourcePage::layoutContent (int width)
{
    const int pad = 32;
    const int inner = juce::jmax (100, width - pad * 2);
    int y = 24 + 54;
    // description height
    juce::AttributedString as;
    as.append (description, style::font (14.0f));
    as.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout tl;
    tl.createLayout (as, (float) inner);
    y += (int) tl.getHeight() + 30;

    for (int i = 0; i < rows.size(); ++i)
    {
        const int h = rowHeights[(size_t) i];
        const int w = dynamic_cast<juce::TextButton*> (rows[i]) != nullptr ? juce::jmin (inner, 520) : juce::jmin (inner, 620);
        rows[i]->setBounds (pad, y, dynamic_cast<juce::Label*> (rows[i]) != nullptr ? inner : w, h);
        y += h + 10;
    }
    y += 10;
    if (albums != nullptr)
    {
        const int h = albums->heightForWidth (inner);
        albums->setBounds (pad, y, inner, h);
        y += h + 20;
    }
    if (tracks != nullptr)
    {
        tracks->setBounds (pad - 10, y, inner + 20, tracks->getTotalHeight());
        y += tracks->getTotalHeight();
    }
    setContentHeight (y + 120);

    struct Head : juce::Component
    {
        SourcePage& page;
        explicit Head (SourcePage& p) : page (p) { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            drawPageTitle (g, page.ctx.theme(), getLocalBounds().removeFromTop (54), page.heading);
            juce::AttributedString as;
            as.append (page.description, style::font (14.0f), page.ctx.theme().textDim());
            as.setWordWrap (juce::AttributedString::byWord);
            juce::TextLayout tl;
            tl.createLayout (as, (float) getWidth());
            tl.draw (g, getLocalBounds().toFloat().withTrimmedTop (60.0f));
        }
    };
    if (contentComp.findChildWithID ("head") == nullptr)
    {
        auto* h = new Head (*this);
        h->setComponentID ("head");
        rows.add (h);
        rowHeights.push_back (0);
        contentComp.addAndMakeVisible (h);
    }
    if (auto* h = contentComp.findChildWithID ("head")) h->setBounds (pad, 24, inner, 60 + (int) tl.getHeight() + 10);
}

//==============================================================================
// SettingsPage
//==============================================================================
SettingsPage::SettingsPage (AppContext& c) : ScrollPage (c)
{
    contentComp.addAndMakeVisible (labels);
    refresh();
}

void SettingsPage::section (const juce::String& title)
{
    Row r;
    r.label = title;
    r.isSection = true;
    r.height = 48;
    layoutRows.push_back (r);
}

namespace
{
    /** Device banner (Merging Anubis, RME ADI-2): the interface, its sound profiles (one click) and a tip. */
    class DeviceProfilePanel : public juce::Component
    {
    public:
        DeviceProfilePanel (AppContext& c, DeviceFamily f, const juce::String& device, bool detected)
            : ctx (c), family (f), deviceName (device), autoDetected (detected)
        {
            setRepaintsOnMouseActivity (true);
            setBufferedToImage (true);           // static card: scrolling the page only blits it
        }

        static int idealHeight (DeviceFamily) { return 286; }

        void paint (juce::Graphics& g) override
        {
            const auto& t = ctx.theme();
            style::paintGlass (g, *this, ctx, getLocalBounds().toFloat(), t.radius());
            g.setColour (t.accent().withAlpha (0.35f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), t.radius(), 1.0f);

            auto r = getLocalBounds().reduced (18, 14);
            auto head = r.removeFromTop (44);
            // interface icon: a small rack / desktop unit with its ports
            auto icon = head.removeFromLeft (44).toFloat().reduced (2.0f);
            g.setColour (t.accent().withAlpha (0.16f));
            g.fillRoundedRectangle (icon, 8.0f);
            g.setColour (t.accent());
            g.drawRoundedRectangle (icon.reduced (8.0f, 13.0f), 3.0f, 1.6f);
            for (int k = 0; k < 2; ++k)
                g.fillRoundedRectangle (icon.getX() + 12.0f + (float) k * 9.0f, icon.getCentreY() - 2.5f, 6.0f, 5.0f, 1.0f);
            g.fillEllipse (icon.getRight() - 15.0f, icon.getCentreY() - 2.5f, 5.0f, 5.0f);
            head.removeFromLeft (12);
            g.setColour (t.textCol());
            g.setFont (style::font (16.0f, true));
            g.drawText (familyTitle (family), head.removeFromTop (22), juce::Justification::centredLeft, true);
            g.setColour (t.textDim());
            g.setFont (style::font (12.5f));
            g.drawText ((autoDetected ? "Détecté : "_t : "Profil choisi manuellement · "_t) + deviceName, head, juce::Justification::centredLeft, true);

            r.removeFromTop (10);
            const int count = familyProfileCount (family);
            const int current = familyCurrentProfile (ctx.engine().getSettings(), family);
            tiles.clear();
            const int gap = 10;
            const int w = (r.getWidth() - (count - 1) * gap) / juce::jmax (1, count);
            auto row = r.removeFromTop (112);
            for (int i = 0; i < count; ++i)
            {
                auto tile = row.removeFromLeft (w);
                row.removeFromLeft (gap);
                tiles.push_back (tile);
                const bool on = i == current;
                const bool hover = tile.contains (getMouseXYRelative()) && isMouseOver (true);
                g.setColour (on ? t.accent().withAlpha (0.18f) : style::neutral().withAlpha (hover ? 0.08f : 0.04f));
                g.fillRoundedRectangle (tile.toFloat(), 10.0f);
                g.setColour (on ? t.accent() : t.border());
                g.drawRoundedRectangle (tile.toFloat().reduced (0.5f), 10.0f, on ? 1.6f : 1.0f);
                auto tr = tile.reduced (12, 10);
                g.setColour (on ? t.accent() : t.textCol());
                g.setFont (style::font (13.5f, true));
                g.drawText (profileName (family, i).fromFirstOccurrenceOf (juce::String::fromUTF8 ("\xe2\x80\x94 "), false, false), tr.removeFromTop (20),
                            juce::Justification::centredLeft, true);
                g.setColour (t.textDim());
                g.setFont (style::font (11.5f));
                g.drawFittedText (profileDescription (family, i), tr, juce::Justification::topLeft, 5, 0.9f);
                if (i == 0)
                {
                    g.setColour (t.textDim().withAlpha (0.8f));
                    g.setFont (style::font (10.5f, true));
                    g.drawText ("PAR DÉFAUT"_t, tile.reduced (12, 8), juce::Justification::topRight);
                }
            }

            r.removeFromTop (12);
            g.setColour (t.colour ("--nx-quality-low", juce::Colour (0xffffb547)));
            g.setFont (style::font (12.5f, true));
            g.drawText (family == DeviceFamily::Merging ? "Astuce MT VAD"_t : family == DeviceFamily::Fiio ? "Astuce FiiO"_t : "Astuce RME"_t, r.removeFromTop (18), juce::Justification::centredLeft);
            g.setColour (t.textCol().withAlpha (0.85f));
            g.setFont (style::font (12.5f));
            const auto tip = familyTip (family).fromFirstOccurrenceOf (" : ", false, false);
            g.drawFittedText (tip.substring (0, 1).toUpperCase() + tip.substring (1), r, juce::Justification::topLeft, 3, 0.95f);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            for (size_t i = 0; i < tiles.size(); ++i)
                if (tiles[i].contains (e.getPosition()))
                {
                    auto s = ctx.engine().getSettings();
                    familySetProfile (s, family, (int) i);
                    ctx.engine().setSettings (s);
                    ctx.applyEngineSettings();
                    ctx.showMessage ("Profil activé : "_t + profileName (family, (int) i));
                    repaint();
                }
        }

    private:
        AppContext& ctx;
        DeviceFamily family;
        juce::String deviceName;
        bool autoDetected;
        std::vector<juce::Rectangle<int>> tiles;
    };

    /** Equipment catalogue: search your model and apply its profile to the active output. */
    /** Equipment manager: search field on top, then a compact list (≈ 190 px) that scrolls / swipes. */
    class DeviceCatalogPanel : public juce::Component
    {
    public:
        DeviceCatalogPanel (AppContext& c, std::function<void()> applied) : ctx (c), onApplied (std::move (applied)), list (*this)
        {
            search.setTextToShowWhenEmpty ("Rechercher votre modèle (RME, Anubis, FiiO KA17…)"_t, juce::Colours::grey);
            search.setFont (style::font (14.0f));
            search.onTextChange = [this] { setResults (searchDeviceModels (search.getText())); };
            addAndMakeVisible (search);
            viewport.setViewedComponent (&list, false);
            viewport.setScrollBarsShown (true, false);
            viewport.setScrollBarThickness (6);
            viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);   // swipe on touch screens / trackpads
            addAndMakeVisible (viewport);
            setBufferedToImage (true);           // the frame is static; the list scrolls on its own
            setResults (searchDeviceModels ({}));
        }

        static constexpr int headH = 96, rowH = 38, listH = 190;
        static int idealHeight() { return headH + listH + 12; }
        int getListHeight() const { return viewport.getHeight(); }
        juce::Viewport& getViewport() { return viewport; }
        const std::vector<const DeviceModel*>& getResults() const { return results; }

        void resized() override
        {
            search.setBounds (16, 54, getWidth() - 32, 32);
            viewport.setBounds (10, headH, getWidth() - 20, listH);
            list.setSize (viewport.getMaximumVisibleWidth(), juce::jmax (listH, (int) results.size() * rowH + 4));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = ctx.theme();
            style::paintGlass (g, *this, ctx, getLocalBounds().toFloat(), t.radius());
            auto r = getLocalBounds().reduced (16, 12);
            g.setColour (t.textCol());
            g.setFont (style::font (15.0f, true));
            g.drawText ("Gestionnaire d'équipements"_t, r.removeFromTop (20), juce::Justification::centredLeft);
            g.setColour (t.textDim());
            g.setFont (style::font (12.0f));
            g.drawText ("Pilote non reconnu ? Choisissez votre modèle : son profil est appliqué à la sortie active et mémorisé."_t, r.removeFromTop (18), juce::Justification::centredLeft, true);
        }

        /** Applies a model (click on its row). */
        void choose (const DeviceModel& m)
        {
            if (! ctx.isPremium() && juce::String (m.id) != "generic") { ctx.showUpsell (UpsellReason::DeviceProfiles); return; }
            auto s = ctx.engine().getSettings();
            applyDeviceModel (s, m);
            ctx.engine().setSettings (s);
            ctx.applyEngineSettings();
            ctx.showMessage ("Équipement appliqué : "_t + juce::String (m.brand) + (*m.brand ? " " : "") + tr (juce::String::fromUTF8 (m.name)));
            auto cb = onApplied;
            juce::MessageManager::callAsync ([cb] { if (cb) cb(); });
        }

    private:
        void setResults (std::vector<const DeviceModel*> r)
        {
            results = std::move (r);
            resized();
            list.repaint();
        }

        struct List : juce::Component
        {
            DeviceCatalogPanel& owner;
            int hover = -1;
            explicit List (DeviceCatalogPanel& o) : owner (o) {}
            void paint (juce::Graphics& g) override
            {
                const auto& t = owner.ctx.theme();
                const auto current = canonicalModelId (owner.ctx.engine().getSettings().deviceModel);
                if (owner.results.empty())
                {
                    g.setColour (t.textDim());
                    g.setFont (style::font (13.0f));
                    g.drawText ("Aucun équipement ne correspond."_t, getLocalBounds().removeFromTop (rowH), juce::Justification::centred);
                    return;
                }
                for (int i = 0; i < (int) owner.results.size(); ++i)
                {
                    const auto* m = owner.results[(size_t) i];
                    auto row = juce::Rectangle<int> (0, i * rowH, getWidth(), rowH).reduced (0, 2);
                    if (! row.intersects (g.getClipBounds())) continue;
                    const bool on = current == m->id;
                    g.setColour (on ? t.accent().withAlpha (0.16f) : style::neutral().withAlpha (i == hover ? 0.07f : 0.025f));
                    g.fillRoundedRectangle (row.toFloat(), 7.0f);
                    if (on) { g.setColour (t.accent().withAlpha (0.8f)); g.drawRoundedRectangle (row.toFloat().reduced (0.5f), 7.0f, 1.0f); }
                    auto in = row.reduced (10, 3);
                    deviceart::drawLogo (g, m->id, in.removeFromLeft (28).toFloat(), on ? t.accent() : t.textCol().withAlpha (0.9f));
                    in.removeFromLeft (10);
                    auto tags = in.removeFromRight (130);
                    const auto sub = juce::String::fromUTF8 (m->subtitle);
                    g.setColour (t.textCol());
                    g.setFont (style::font (13.0f, true));
                    g.drawText (juce::String (m->brand) + (*m->brand ? " " : "") + tr (juce::String::fromUTF8 (m->name)), in.removeFromTop (17), juce::Justification::centredLeft, true);
                    g.setColour (t.textDim());
                    g.setFont (style::font (11.0f));
                    g.drawText (sub.isNotEmpty() ? sub : m->series[0] ? "Série "_t + m->series : "Profil standard"_t, in, juce::Justification::centredLeft, true);
                    g.setColour (t.accent2());
                    g.setFont (style::font (10.5f, true));
                    g.drawText (m->badges(), tags, juce::Justification::centredRight, true);
                }
            }
            int rowAt (int y) const { const int i = y / rowH; return i >= 0 && i < (int) owner.results.size() ? i : -1; }
            void mouseMove (const juce::MouseEvent& e) override { if (const int i = rowAt (e.y); i != hover) { hover = i; repaint(); } }
            void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
            void mouseUp (const juce::MouseEvent& e) override
            {
                if (e.mouseWasDraggedSinceMouseDown()) return;                // a swipe, not a choice
                if (const int i = rowAt (e.y); i >= 0) owner.choose (*owner.results[(size_t) i]);
            }
        };

        AppContext& ctx;
        std::function<void()> onApplied;
        juce::TextEditor search;
        juce::Viewport viewport;
        std::vector<const DeviceModel*> results;
        List list;
    };
}

namespace
{
    /** One reassignable shortcut: click, then press the new combination (Escape cancels). */
    class ShortcutButton : public juce::Component
    {
    public:
        ShortcutButton (AppContext& c, shortcuts::Action a) : ctx (c), action (a)
        {
            setWantsKeyboardFocus (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTitle (tr (juce::String::fromUTF8 (shortcuts::info (a).label)));
        }
        bool isListening() const noexcept { return listening; }
        void mouseUp (const juce::MouseEvent&) override { listening = true; grabKeyboardFocus(); repaint(); }
        void focusLost (FocusChangeType) override { listening = false; repaint(); }
        void handleCommandMessage (int) override { listening = true; repaint(); }   // start capturing without a click (tests, accessibility)
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (! listening) return false;
            listening = false;
            if (k != juce::KeyPress (juce::KeyPress::escapeKey)) assign (k);
            repaint();
            return true;
        }
        /** Assigns a key (also used by the tests); refused if another action already uses it. */
        bool assign (const juce::KeyPress& k)
        {
            if (auto other = shortcuts::match (ctx.settings(), k); other && *other != action)
            {
                ctx.showMessage (shortcuts::describe (k) + " : "_t + "déjà utilisé par « "_t
                                 + tr (juce::String::fromUTF8 (shortcuts::info (*other).label)) + " »"_t);
                return false;
            }
            shortcuts::set (ctx.settings(), action, k);
            repaint();
            return true;
        }
        void paint (juce::Graphics& g) override
        {
            const auto& t = ctx.theme();
            auto r = getLocalBounds().toFloat().reduced (1.0f, 3.0f);
            g.setColour (listening ? t.accent().withAlpha (0.18f) : style::neutral().withAlpha (isMouseOver() ? 0.09f : 0.05f));
            g.fillRoundedRectangle (r, 7.0f);
            g.setColour (listening ? t.accent() : t.border().withMultipliedAlpha (1.5f));
            g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
            g.setColour (listening ? t.accent() : t.textCol());
            g.setFont (style::font (13.0f, ! listening));
            g.drawText (listening ? "Appuyez sur la combinaison… (Échap : annuler)"_t : shortcuts::describe (shortcuts::get (ctx.settings(), action)),
                        r.reduced (10, 0), juce::Justification::centred, true);
        }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }
    private:
        AppContext& ctx;
        shortcuts::Action action;
        bool listening = false;
    };
}

namespace
{
    /** Réglages › À propos: edition (Free / Premium), the licence key, the version and the update check. */
    class LicenseCard : public juce::Component, private juce::Timer
    {
    public:
        LicenseCard (AppContext& c, std::function<void()> changed) : ctx (c), onChanged (std::move (changed))
        {
            const bool premium = ctx.isPremium();
            keyField.setTextToShowWhenEmpty ("Collez votre clé NXP1-…"_t, juce::Colours::grey);
            keyField.setFont (style::font (13.0f));
            keyField.onReturnKey = [this] { activate(); };
            addChildComponent (keyField);
            keyField.setVisible (! premium);
            action.setButtonText (premium ? "Désactiver sur cet appareil"_t : "Activer"_t);
            action.onClick = [this]
            {
                if (ctx.isPremium()) { ctx.deactivateLicense(); notify(); }
                else activate();
            };
            addAndMakeVisible (action);
            buy.setButtonText ("Acheter Aura Premium"_t);
            buy.onClick = [this] { ctx.openShop(); };
            addChildComponent (buy);
            buy.setVisible (! premium);
            autoCheck.setButtonText ("Vérifier automatiquement les mises à jour au lancement"_t);
            autoCheck.setToggleState ((bool) ctx.settings().get ("autoUpdateCheck", true), juce::dontSendNotification);
            autoCheck.onClick = [this] { ctx.settings().set ("autoUpdateCheck", autoCheck.getToggleState()); };
            addAndMakeVisible (autoCheck);
            checkNow.setButtonText ("Vérifier maintenant"_t);
            checkNow.onClick = [this] { ctx.checkForUpdates (true); lastState = -1; startTimerHz (4); };
            addAndMakeVisible (checkNow);
            download.setButtonText ("Télécharger la mise à jour"_t);
            download.onClick = [this]
            {
                const auto url = ctx.updates().getStatus().info.downloadUrlForThisOS();
                if (url.isNotEmpty()) juce::URL (url).launchInDefaultBrowser();
            };
            addChildComponent (download);
            syncUpdate();
            startTimerHz (2);
        }

        static int idealHeight (AppContext& c)
        {
            return c.updates().getStatus().state == UpdateChecker::State::Available ? 304 : 246;
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (18, 14);
            r.removeFromTop (54);
            auto keyRow = r.removeFromTop (34);
            action.setBounds (keyRow.removeFromRight (ctx.isPremium() ? 230 : 120));
            keyRow.removeFromRight (10);
            if (buy.isVisible()) { buy.setBounds (keyRow.removeFromRight (230)); keyRow.removeFromRight (10); }
            keyField.setBounds (keyRow);
            r.removeFromTop (68);
            auto buttons = r.removeFromTop (32);
            checkNow.setBounds (buttons.removeFromLeft (190));
            buttons.removeFromLeft (10);
            download.setBounds (buttons.removeFromLeft (240));
            r.removeFromTop (6);
            autoCheck.setBounds (r.removeFromTop (26));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = ctx.theme();
            style::paintGlass (g, *this, ctx, getLocalBounds().toFloat(), t.radius());
            auto r = getLocalBounds().reduced (18, 14);
            const bool premium = ctx.isPremium();
            auto head = r.removeFromTop (24);
            if (premium)
            {
                // "ÉDITION PREMIUM" badge
                const auto text = "ÉDITION PREMIUM"_t;
                g.setFont (style::font (11.0f, true));
                auto badge = head.removeFromRight ((int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 22).toFloat().reduced (0, 2);
                g.setGradientFill (juce::ColourGradient (t.accent(), badge.getX(), 0, t.accent2(), badge.getRight(), 0, false));
                g.fillRoundedRectangle (badge, badge.getHeight() * 0.5f);
                g.setColour (t.colour ("--nx-on-accent", juce::Colours::black));
                g.drawText (text, badge, juce::Justification::centred);
            }
            g.setColour (t.textCol());
            g.setFont (style::font (15.0f, true));
            g.drawText (premium ? "Aura Premium — Activé pour "_t + ctx.license().getLicensee() : juce::String ("Aura Free (PCM jusqu'à 192 kHz)"_t),
                        head, juce::Justification::centredLeft, true);
            g.setColour (t.textDim());
            g.setFont (style::font (12.0f));
            g.drawText (premium ? juce::String ("Tout est débloqué : DSD, DXD, SACD, mode exclusif, profils matériels, Aura Remote, éditeur de métadonnées."_t)
                                : juce::String ("Premium débloque le DSD, le DXD (> 192 kHz), les images SACD, le mode exclusif, les profils matériels, Aura Remote et l'éditeur de métadonnées."_t),
                        r.removeFromTop (22), juce::Justification::centredLeft, true);
            r.removeFromTop (8 + 34 + 10);
            g.setColour (t.border());
            g.fillRect (r.removeFromTop (1));
            r.removeFromTop (8);
            g.setColour (t.textCol());
            g.setFont (style::font (13.0f, true));
            g.drawText ("Version actuelle : v"_t + ctx.updates().getCurrentVersion(), r.removeFromTop (20), juce::Justification::centredLeft);
            g.setColour (t.textDim());
            g.setFont (style::font (12.0f));
            g.drawText (statusText(), r.removeFromTop (20), juce::Justification::centredLeft, true);
            const auto st = ctx.updates().getStatus();
            if (st.state == UpdateChecker::State::Available && st.info.releaseNotes.isNotEmpty())
            {
                const juce::Rectangle<int> notes (18, 240, getWidth() - 36, 54);
                g.setColour (t.textCol().withAlpha (0.85f));
                g.drawFittedText (st.info.releaseNotes, notes, juce::Justification::topLeft, 3);
            }
        }

        juce::String statusText() const
        {
            const auto st = ctx.updates().getStatus();
            const auto when = st.checkedAt.toMilliseconds() > 0 ? " (" + st.checkedAt.formatted ("%H:%M") + ")" : juce::String();
            switch (st.state)
            {
                case UpdateChecker::State::Idle:      return "Mises à jour : pas encore vérifiées"_t;
                case UpdateChecker::State::Checking:  return "Recherche de mises à jour…"_t;
                case UpdateChecker::State::UpToDate:  return "Aura est à jour"_t + when;
                case UpdateChecker::State::Available: return "Nouvelle version disponible : v"_t + st.info.latestVersion + (st.info.mandatory ? " · mise à jour requise"_t : juce::String()) + when;
                case UpdateChecker::State::Failed:    return "Vérification impossible : "_t + st.error + when;
            }
            return {};
        }

        /** Activates the key typed in the field (tests call it too). */
        bool activate()
        {
            juce::String err;
            if (! ctx.activateLicense (keyField.getText().trim(), err)) { ctx.showMessage (err); return false; }
            notify();
            return true;
        }
        juce::TextEditor& getKeyField() { return keyField; }

    private:
        void notify() { auto cb = onChanged; juce::MessageManager::callAsync ([cb] { if (cb) cb(); }); }
        void syncUpdate()
        {
            const auto st = ctx.updates().getStatus();
            download.setVisible (st.state == UpdateChecker::State::Available && st.info.downloadUrlForThisOS().isNotEmpty());
            checkNow.setEnabled (st.state != UpdateChecker::State::Checking);
        }
        void timerCallback() override
        {
            const int s = (int) ctx.updates().getStatus().state;
            if (s == lastState) return;
            const bool grew = s == (int) UpdateChecker::State::Available && getHeight() < idealHeight (ctx);
            lastState = s;
            syncUpdate();
            repaint();
            if (grew) notify();                          // room for the release notes
        }

        AppContext& ctx;
        std::function<void()> onChanged;
        juce::TextEditor keyField;
        juce::TextButton action, checkNow, download, buy;
        juce::ToggleButton autoCheck;
        int lastState = -1;
    };
}

void SettingsPage::fullRow (juce::Component* control, int height)
{
    Row r;
    r.control = control;
    r.height = height;
    r.full = true;
    owned.add (control);
    contentComp.addAndMakeVisible (control);
    layoutRows.push_back (r);
}

void SettingsPage::row (const juce::String& label, juce::Component* control, int height, const juce::String& help)
{
    Row r;
    r.label = label;
    r.help = help;
    r.control = control;
    r.height = height;
    if (control != nullptr) { owned.add (control); contentComp.addAndMakeVisible (control); }
    layoutRows.push_back (r);
}

juce::StringArray SettingsPage::tabIds() { return { "audio", "bibliotheque", "interface", "remote", "outils", "apropos" }; }
juce::StringArray SettingsPage::tabLabels()
{
    return { "Audio & Système"_t, "Bibliothèque & NAS"_t, "Interface"_t, "Aura Remote", "Outils Studio"_t, "À propos"_t };
}

namespace
{
    /** Multi-line text block of the settings (credits, legal notices): its height follows the width. */
    struct NoticeText : juce::Component
    {
        NoticeText (juce::String t, float size, juce::Colour c) : text (std::move (t)), fontSize (size), colour (c) { setInterceptsMouseClicks (false, false); }
        juce::TextLayout layout (int width) const
        {
            juce::AttributedString as;
            as.append (text, style::font (fontSize), colour);
            as.setWordWrap (juce::AttributedString::byWord);
            as.setLineSpacing (2.0f);
            juce::TextLayout tl;
            tl.createLayout (as, (float) juce::jmax (100, width));
            return tl;
        }
        int heightFor (int width) const { return (int) std::ceil (layout (width).getHeight()) + 4; }
        void paint (juce::Graphics& g) override { layout (getWidth()).draw (g, getLocalBounds().toFloat()); }
        juce::String text;
        float fontSize;
        juce::Colour colour;
    };

    juce::String trademarkNotice()
    {
        return "RME et ADI-2 sont des marques de RME Audio AG. Merging, Anubis et RAVENNA sont des marques de Merging Technologies SA. "
               "FiiO et KA17 sont des marques de FiiO Electronics Technology Co., Ltd. ASIO est une marque de Steinberg Media Technologies GmbH. "
               "Super Audio CD et Direct Stream Digital sont des marques de Sony Group Corporation et de Koninklijke Philips N.V. "
               "Les autres noms de produits ou de sociétés cités appartiennent à leurs propriétaires respectifs. Ces noms sont utilisés "
               "uniquement pour indiquer la compatibilité des profils audio d'Aura. Aura n'est ni affilié à ces sociétés, ni approuvé, "
               "sponsorisé ou certifié par elles. Les profils matériels sont fournis « en l'état », sans garantie de résultat sur un appareil donné."_t;
    }

    juce::String creditsText()
    {
        juce::StringArray l;
        l.add ("Aura © 2026 NX Audio (ZEYN). Tous droits réservés."_t);
        l.add ("Aura utilise les composants suivants ; nous remercions leurs auteurs."_t);
        l.add ({});
        l.add ("JUCE — © Raw Material Software Limited. Utilisé sous licence JUCE (licence commerciale)."_t);
        l.add ("JUCE intègre :"_t);
        l.add ("   • FLAC — © Josh Coalson, Xiph.Org Foundation. Licence BSD."_t);
        l.add ("   • Ogg / Vorbis — © Xiph.Org Foundation. Licence BSD."_t);
        l.add ("   • zlib — © Jean-loup Gailly et Mark Adler. Licence zlib."_t);
        l.add ("   • libpng — © The PNG Reference Library Authors. Licence libpng."_t);
        l.add ("   • Ce logiciel est basé en partie sur le travail de l'Independent JPEG Group."_t);
        l.add ("   • HarfBuzz — © ses auteurs. Licence « Old MIT »."_t);
        l.add ("   • SheenBidi — © Muhammad Tayyab Akram. Licence Apache 2.0."_t);
        l.add ("   • choc — © Julian Storer. Licence ISC. QuickJS — © Fabrice Bellard, Charlie Gordon. Licence MIT."_t);
        l.add ("Monocypher 4.0.3 — © Loup Vaillant et contributeurs. Licence BSD 2 clauses (ou CC0)."_t);
        l.add ("libwebp — © Google LLC. Licence BSD 3 clauses et octroi de brevets."_t);
        l.add ("stb_image — Sean Barrett. Domaine public (ou licence MIT)."_t);
        l.add ("Apple Lossless Audio Codec — © Apple Inc. Licence Apache 2.0 (fichiers modifiés pour arm64 / MSVC)."_t);
        l.add ("SQLite — domaine public."_t);
        l.add ("QR Code generator library — © Project Nayuki. Licence MIT."_t);
        l.add ("ASIO — technologie et marque de Steinberg Media Technologies GmbH (version Windows)."_t);
        l.add ({});
        l.add ("Données et services en ligne (facultatifs, désactivables dans les réglages) :"_t);
        l.add ("   • Biographies d'artistes : Wikipédia, textes sous licence CC BY-SA 4.0 (source affichée sous chaque texte)."_t);
        l.add ("   • Photos et informations d'artistes : TheAudioDB.com (uniquement avec votre clé API personnelle)."_t);
        l.add ({});
        l.add ("Les textes complets de ces licences sont fournis dans le fichier « Licences.txt »."_t);
        return l.joinIntoString ("\n");
    }

    /** Licences.txt (embedded) in a read-only window. */
    void showLicencesFile (const Theme& theme)
    {
        auto* ed = new juce::TextEditor();
        ed->setMultiLine (true, true);
        ed->setReadOnly (true);
        ed->setScrollbarsShown (true);
        ed->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        ed->setColour (juce::TextEditor::backgroundColourId, theme.bg());
        ed->setColour (juce::TextEditor::textColourId, theme.textCol());
        ed->setText (juce::String::fromUTF8 (NXData::Licences_txt, NXData::Licences_txtSize), false);
        ed->setSize (820, 640);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (ed);
        o.dialogTitle = "Licences.txt";
        o.dialogBackgroundColour = theme.bg();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }
}

namespace
{
    juce::ComboBox* makeCombo (const juce::StringArray& items, const juce::String& selected, std::function<void (const juce::String&)> fn)
    {
        auto* cb = new juce::ComboBox();
        cb->addItemList (items, 1);
        cb->setText (selected, juce::dontSendNotification);
        cb->onChange = [cb, fn] { fn (cb->getText()); };
        return cb;
    }
    juce::ToggleButton* makeToggle (bool on, std::function<void (bool)> fn)
    {
        auto* t = new juce::ToggleButton();
        t->setToggleState (on, juce::dontSendNotification);
        t->onClick = [t, fn] { fn (t->getToggleState()); };
        return t;
    }
    juce::TextButton* makeButton (const juce::String& text, std::function<void()> fn)
    {
        auto* b = new juce::TextButton (text);
        b->onClick = std::move (fn);
        return b;
    }
}

void SettingsPage::refresh()
{
    for (auto* c : owned) contentComp.removeChildComponent (c);
    owned.clear();
    layoutRows.clear();
    if (! tabIds().contains (tab)) tab = "audio";
    if (tab == "audio") buildAudio();
    else if (tab == "bibliotheque") buildLibrary();
    else if (tab == "interface") buildInterface();
    else if (tab == "remote") buildRemote();
    else if (tab == "outils") buildTools();
    else buildAbout();
    layoutContent (getWidth() - viewport.getScrollBarThickness());
}

void SettingsPage::buildAudio()
{
    auto& eng = ctx.engine();
    auto s = eng.getSettings();

    fullRow (makeHardwareDashboard (ctx).release(), HardwareDashboardHeight);

    section ("Sortie audio"_t);
    row ("Pilote"_t, makeCombo (ctx.getBackendNames(), ctx.getCurrentBackendName(), [this] (const juce::String& n) { ctx.selectBackend (n); refresh(); }), 36,
         "ASIO et WASAPI exclusif (Windows), CoreAudio + Hog Mode (macOS), ALSA hw (Linux) contournent le mixeur du système."_t);
    row ("Périphérique"_t, makeCombo (eng.getBackend().getDeviceNames(), eng.getDeviceName(), [this] (const juce::String& n) { ctx.selectDevice (n); refresh(); }));
    const bool premium = ctx.isPremium();
    row ("Accès exclusif (Hog Mode / exclusif)"_t + (premium ? juce::String() : juce::String ("  ·  PRO")), makeToggle (s.exclusive && premium, [this] (bool v)
    {
        if (v && ! ctx.isPremium()) { ctx.showUpsell (UpsellReason::ExclusiveMode); refresh(); return; }
        auto st = ctx.engine().getSettings(); st.exclusive = v; ctx.engine().setSettings (st); ctx.applyEngineSettings();
    }));
    {
        auto* buf = makeCombo ({ "Automatique"_t, "10 ms", "20 ms", "50 ms", "100 ms", "200 ms" },
                               s.bufferMilliseconds == 0 ? "Automatique"_t : juce::String (s.bufferMilliseconds) + " ms",
                               [this] (const juce::String& v) { auto st = ctx.engine().getSettings(); st.bufferMilliseconds = v.getIntValue(); ctx.engine().setSettings (st); ctx.applyEngineSettings(); });
        row ("Tampon du périphérique"_t, buf);
    }
    const auto status = eng.getOutputStatus();
    row (status.open ? "État : "_t + status.backendName + " · "_t + formatRate (status.sampleRate) + " · "_t + status.detail : juce::String ("État : périphérique fermé (s'ouvre à la lecture)"_t), nullptr, 28);

    // Hardware profile (Merging Anubis / RAVENNA, RME ADI-2, FiiO KA17)
    {
        fullRow (new DeviceCatalogPanel (ctx, [this] { refresh(); }), DeviceCatalogPanel::idealHeight());
        fullRow (new NoticeText (trademarkNotice(), 11.0f, ctx.theme().textDim().withAlpha (0.8f)), 60);
        const juce::StringArray profiles { "Automatique (détection)"_t, "Générique"_t, "Merging Anubis / RAVENNA (AES67)"_t, "RME ADI-2 (USB)", "FiiO KA17 (USB)" };
        row ("Profil matériel"_t + (premium ? juce::String() : juce::String ("  ·  PRO")), makeCombo (profiles, profiles[premium ? (int) s.hardwareProfile : 1], [this, profiles] (const juce::String& v)
        {
            if (! ctx.isPremium()) { ctx.showUpsell (UpsellReason::DeviceProfiles); refresh(); return; }   // Free: the standard profile only
            auto st = ctx.engine().getSettings();
            st.hardwareProfile = (HardwareProfile) juce::jmax (0, profiles.indexOf (v));
            st.deviceModel = {};                 // a manual family choice leaves the catalogue model
            ctx.engine().setSettings (st);
            ctx.applyEngineSettings();
            refresh();
        }), 36, "Un périphérique reconnu reçoit des réglages dédiés : Merging (« Merging », « Anubis », « RAVENNA », « VAD ») et RME (« RME », « ADI-2 »)."_t);

        const auto family = activeFamily (s, eng.getDeviceName());
        if (family != DeviceFamily::None)
        {
            fullRow (new DeviceProfilePanel (ctx, family, eng.getDeviceName(), s.hardwareProfile == HardwareProfile::Auto),
                     DeviceProfilePanel::idealHeight (family));
            if (family == DeviceFamily::Merging)
            {
                const juce::StringArray gains { "0 dB", "+3 dB", "+6 dB" };
                const int gi = s.anubisDsdGainDb >= 5.0 ? 2 : s.anubisDsdGainDb >= 2.0 ? 1 : 0;
                row ("Gain DSD -> DXD (profils Anubis)"_t, makeCombo (gains, gains[gi], [this] (const juce::String& v)
                {
                    auto st = ctx.engine().getSettings();
                    st.anubisDsdGainDb = v.startsWith ("+6") ? 6.0 : v.startsWith ("+3") ? 3.0 : 0.0;
                    ctx.engine().setSettings (st);
                    ctx.applyEngineSettings();
                }), 36, "SACD 0 dB = −6 dBFS : +6 dB retrouve le niveau du PCM. Un limiteur 64 bits (plafond −0,2 dBFS) évite tout écrêtage."_t);
                row ("Modelage harmonique (Analog Warmth)"_t, makeToggle (s.anubisHarmonics, [this] (bool v)
                {
                    auto st = ctx.engine().getSettings();
                    st.anubisHarmonics = v;
                    ctx.engine().setSettings (st);
                    ctx.applyEngineSettings();
                }), 36, "Harmonique paire d'ordre 2 à −90 dB et détente de −0,5 dB au-dessus de 18 kHz. Désactivé : filtre à phase minimale seul."_t);
            }
            else
            {
                row ("Sécurité RME DSD Direct (verrouiller le volume logiciel à 100 %)"_t, makeToggle (s.rmeLockVolume, [this] (bool v)
                {
                    auto st = ctx.engine().getSettings();
                    st.rmeLockVolume = v;
                    ctx.engine().setSettings (st);
                    ctx.applyEngineSettings();
                }), 36, "Attention : en « DSD Direct », l'ADI-2 désactive son volume matériel sur les sorties ligne. Le volume d'Aura reste "_t
                        "à 100 % (bit-perfect) : réglez le niveau avec un contrôle analogique externe."_t);
            }
        }
    }

    section ("DSD");
    const juce::StringArray modes { "Automatique (natif, puis DoP, puis PCM)"_t, "DSD natif (ASIO)"_t, "DoP (DSD over PCM)", "Conversion en PCM (DXD)"_t };
    row ("Mode DSD"_t, makeCombo (modes, modes[(int) s.dsdMode], [this, modes] (const juce::String& v)
    {
        auto st = ctx.engine().getSettings();
        st.dsdMode = (DsdMode) juce::jmax (0, modes.indexOf (v));
        ctx.engine().setSettings (st);
        ctx.applyEngineSettings();
    }));
    row ("Autoriser DoP"_t, makeToggle (s.dopAllowed, [this] (bool v) { auto st = ctx.engine().getSettings(); st.dopAllowed = v; ctx.engine().setSettings (st); ctx.applyEngineSettings(); }), 36,
         "Le DAC doit reconnaître les marqueurs DoP (0x05 / 0xFA) ; sinon il joue du bruit à faible niveau."_t);
    const juce::StringArray gains { "0 dB (sans écrêtage)"_t, "+6 dB (SACD 0 dB = 0 dBFS)" };
    row ("Gain DSD -> PCM"_t, makeCombo (gains, s.dsdToPcmGainDb > 3.0 ? gains[1] : gains[0],
         [this, gains] (const juce::String& v) { auto st = ctx.engine().getSettings(); st.dsdToPcmGainDb = v == gains[1] ? 6.0 : 0.0; ctx.engine().setSettings (st); ctx.applyEngineSettings(); }));

    section ("Moteur & système"_t);
    row ("Memory Play : charger chaque morceau en RAM"_t, makeToggle (s.memoryPlay, [this] (bool v)
    {
        auto st = ctx.engine().getSettings();
        st.memoryPlay = v;
        ctx.engine().setSettings (st, false);
        ctx.applyEngineSettings();
    }), 36, "Le fichier entier est copié en mémoire au début de la lecture : plus aucun accès disque ou réseau pendant l'écoute. Données identiques au fichier (bit-perfect)."_t);
    {
        const juce::StringArray sizes { "512 Mo", "1 Go", "1,5 Go", "2 Go", "4 Go" };
        const int mb[] = { 512, 1024, 1536, 2048, 4096 };
        int sel = 2;
        for (int i = 0; i < 5; ++i) if (s.memoryPlayMaxMb == mb[i]) sel = i;
        row ("Memory Play : taille maximale d'un fichier"_t, makeCombo (sizes, sizes[sel], [this, sizes] (const juce::String& v)
        {
            const int values[] = { 512, 1024, 1536, 2048, 4096 };
            auto st = ctx.engine().getSettings();
            st.memoryPlayMaxMb = values[juce::jmax (0, sizes.indexOf (v))];
            ctx.engine().setSettings (st, false);
            ctx.applyEngineSettings();
        }), 36, "Un fichier plus gros (DSD512, DXD de plusieurs heures) est lu depuis le disque."_t);
    }
    row ("Mode Streamer / Gaming"_t, makeToggle (ctx.isStreamerMode(), [this] (bool v) { ctx.setStreamerMode (v); }), 36,
         "Fond, spectre, paroles et transitions au repos ; 1 image / s quand Aura est en arrière-plan. L'audio n'est pas modifié."_t);
    row ("Overlay « Now Playing » pour OBS"_t, makeToggle (ctx.isNowPlayingOverlayVisible(), [this] (bool v) { ctx.setNowPlayingOverlay (v); }), 36,
         "Petite fenêtre en verre givré (pochette, titre, artiste, badge Hi-Res / DSD) à capturer dans OBS. Double-clic : masquer."_t);
    row ("Mode Streamer : now_playing.txt et now_playing_cover.jpg mis à jour en temps réel"_t, makeButton ("Ouvrir le dossier"_t, [this]
    {
        ctx.userDataDirectory().getChildFile ("now_playing.txt").revealToUser();
        if (! ctx.userDataDirectory().getChildFile ("now_playing.txt").existsAsFile()) ctx.userDataDirectory().startAsProcess();
    }), 36, "Sources « Texte (lire depuis un fichier) » et « Image » d'OBS Studio."_t);
    row ("Empêcher la mise en veille pendant la lecture"_t, makeToggle ((bool) ctx.settings().get ("preventSleep", true), [this] (bool v) { ctx.settings().set ("preventSleep", v); }), 36,
         SleepBlocker::isSupported() ? "L'ordinateur reste éveillé tant que la musique joue (l'écran peut s'éteindre)."_t
                                     : "Non disponible sur ce système."_t);
    row ("Suréchantillonnage sinc vers DXD (352,8 / 384 kHz)"_t, makeToggle (s.upsampleToDxd, [this] (bool v) { auto st = ctx.engine().getSettings(); st.upsampleToDxd = v; ctx.engine().setSettings (st); ctx.applyEngineSettings(); }), 36,
         "Filtre polyphase 64 bits, 160 dB. Rapport entier (×2, ×4, ×8) dans la même famille de fréquences. Désactive le bit-perfect."_t);
    row ("Volume numérique : 100 % = bit-perfect"_t, nullptr, 28);
}

void SettingsPage::buildLibrary()
{
    section ("Dossiers de musique"_t);
    for (auto& f : ctx.library().getFolders())
        row (f, makeButton ("Retirer"_t, [this, f]
        {
            ctx.library().removeFolder (juce::File (f));
            ctx.library().notifyChanged();
            refresh();
        }));
    row ("Ajouter un dossier (disque interne, externe, partage SMB / NFS monté)"_t, makeButton ("Ajouter un dossier…"_t, [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Dossier de musique"_t, juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser] (const juce::FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (! dir.isDirectory()) return;
            ctx.library().addFolder (dir);
            ctx.scanner().requestFullScan();
            ctx.showMessage ("Indexation de "_t + dir.getFileName() + "…"_t);
            refresh();
        });
    }));
    row (juce::String (ctx.library().countTracks ("local")) + " morceaux locaux indexés"_t, makeButton ("Réindexer"_t, [this] { ctx.scanner().requestFullScan(); ctx.showMessage ("Réindexation lancée"_t); }));

    section ("NAS / Jellyfin"_t);
    const int nasAlbums = ctx.library().countAlbumsFromSourcePrefix ("upnp");
    row (juce::String (nasAlbums) + " album(s) du NAS dans la bibliothèque"_t, nullptr, 28);
    row ("Retirer les albums des serveurs indisponibles / hors-ligne"_t, makeButton ("Retirer les indisponibles"_t, [this]
    {
        const int n = actions::removeUnavailableNasAlbums (ctx);
        ctx.showMessage (juce::String (n) + " morceau(x) de serveurs indisponibles retiré(s)"_t);
        refresh();
    }), 36, "Garde les serveurs connectés pendant cette session, retire les autres. Les fichiers restent sur le NAS."_t);
    row ("Purger tous les éléments du NAS de la bibliothèque"_t, makeButton ("Purger le NAS"_t, [this]
    {
        actions::confirm (ctx, "Purger le NAS"_t, "Tous les albums importés d'un NAS / Jellyfin seront retirés de la bibliothèque (les fichiers restent sur le serveur)."_t,
                          "Purger"_t, [this] { const int n = actions::purgeNas (ctx); ctx.showMessage (juce::String (n) + " morceau(x) retiré(s)"_t); refresh(); });
    }));

    section ("Téléchargements"_t);
    {
        const auto folder = ctx.downloads().getFolder();
        row ("Dossier de téléchargement (NAS / Jellyfin)"_t, makeButton (folder.getFullPathName(), [this]
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Dossier de téléchargement"_t, ctx.downloads().getFolder());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (! dir.isDirectory()) return;
                ctx.downloads().setFolder (dir);
                ctx.settings().set ("downloadFolder", dir.getFullPathName());
                ctx.showMessage ("Téléchargements : "_t + dir.getFullPathName());
                refresh();
            });
        }), 36, "Les morceaux et albums téléchargés depuis un serveur sont copiés ici (Artiste / Album / fichier original) "_t
                "puis ajoutés à la bibliothèque, disponibles hors-ligne."_t);
    }

    section ("Artistes & paroles"_t);
    row ("Photos et biographies d'artistes en ligne (Wikipédia, TheAudioDB avec votre clé)"_t, makeToggle (ctx.artistInfo().isEnabled(), [this] (bool v)
    {
        ctx.artistInfo().setEnabled (v);
        ctx.settings().set ("artistInfoOnline", v);
    }), 36, "Seul le nom de l'artiste est envoyé. Les réponses sont gardées 30 jours dans la bibliothèque. "
            "Biographies Wikipédia sous licence CC BY-SA 4.0 (source affichée sous chaque texte)."_t);
    {
        auto* key = new juce::TextEditor();
        key->setTextToShowWhenEmpty ("Votre clé (theaudiodb.com)"_t, ctx.theme().textDim());
        key->setFont (style::font (14.0f));
        key->setIndents (10, 0);
        key->setJustification (juce::Justification::centredLeft);
        key->setText (ctx.settings().get ("audioDbKey").toString(), false);
        key->onTextChange = [this, key]
        {
            ctx.settings().set ("audioDbKey", key->getText().trim());
            ctx.artistInfo().setAudioDbKey (key->getText());
        };
        row ("Clé API TheAudioDB personnelle (optionnel)"_t, key, 36,
             "Sans clé personnelle, TheAudioDB n'est jamais interrogé (biographies Wikipédia seules)."_t);
    }
    row ("Paroles : fichiers .lrc / .txt à côté du morceau et tags intégrés (aucune requête en ligne)"_t, nullptr, 26);
}

void SettingsPage::buildInterface()
{
    section ("Personnalisez Aura"_t);
    const juce::StringArray langs { juce::String::fromUTF8 ("Fran\xc3\xa7" "ais"), "English" };
    row ("Langue / Language"_t, makeCombo (langs, i18n::isEnglish() ? juce::String ("English") : langs[0], [this] (const juce::String& v)
    {
        ctx.setLanguage (v == "English" ? "en" : "fr");
    }));
    fullRow (new appearance::Picker (ctx, appearance::Picker::Kind::Appearance), appearance::Picker::idealHeight (appearance::Picker::Kind::Appearance));
    {
        juce::StringArray names, ids;
        for (auto& th : ctx.themes().getThemes()) { names.add (th.name); ids.add (th.id); }
        const auto* chosen = ctx.themes().findTheme (ctx.selectedThemeId());
        row ("Thème"_t, makeCombo (names, chosen != nullptr ? chosen->name : ctx.theme().name, [this, names, ids] (const juce::String& v)
        {
            const int i = names.indexOf (v);
            if (i >= 0) ctx.applyTheme (ids[i]);
        }), 36, "Thèmes : ici ; plug-ins : barre latérale › Extensions."_t);
    }
    row ("Thèmes de la communauté : un fichier .json se partage et s'importe en un clic"_t,
         makeButton ("Importer un thème (.json)"_t, [this] { ctx.chooseThemeToImport(); }), 36);
    row ("Votre thème (avec votre couleur d'accent), à partager sur Discord"_t,
         makeButton ("Exporter le thème actuel"_t, [this] { ctx.chooseThemeExport(); }), 36);

    section ("Raccourcis clavier"_t);
    for (auto& sc : shortcuts::all())
        row (tr (juce::String::fromUTF8 (sc.label)), new ShortcutButton (ctx, sc.action), 34,
             sc.action == shortcuts::Action::PlayPause ? juce::String ("Cliquez sur un raccourci puis tapez la nouvelle combinaison. Elgato Stream Deck : action « Raccourci clavier » avec la même combinaison."_t) : juce::String());
    row ("Raccourcis par défaut"_t, makeButton ("Réinitialiser par défaut"_t, [this]
    {
        shortcuts::resetAll (ctx.settings());
        ctx.showMessage ("Raccourcis clavier réinitialisés"_t);
        refresh();
    }));

    section ("Disposition Aura"_t);
    row ("Densité de la barre d'écoute"_t, nullptr, 24);
    fullRow (new appearance::Picker (ctx, appearance::Picker::Kind::Density), appearance::Picker::idealHeight (appearance::Picker::Kind::Density));
    const auto layout = ctx.settings().get ("layout");
    const juce::StringArray sidebarModes { "Étendue (icônes et libellés)"_t, "Compacte (icônes)"_t };
    row ("Barre latérale"_t, makeCombo (sidebarModes, layout.getProperty ("sidebar", "wide").toString() == "compact" ? sidebarModes[1] : sidebarModes[0],
                                        [this, sidebarModes] (const juce::String& v) { ctx.setLayoutOption ("sidebar", v == sidebarModes[1] ? "compact" : "wide"); }));
    const juce::StringArray playerModes { "En bas"_t, "En haut"_t };
    row ("Barre de lecture"_t, makeCombo (playerModes, layout.getProperty ("player", "bottom").toString() == "top" ? playerModes[1] : playerModes[0],
                                          [this, playerModes] (const juce::String& v) { ctx.setLayoutOption ("player", v == playerModes[1] ? "top" : "bottom"); }));

    section ("Vue immersive"_t);
    const auto imm = ctx.settings().get ("immersive");
    const juce::StringArray layouts { "Partagée : pochette + paroles"_t, "Centrée : grande pochette"_t, "Minimale"_t };
    const auto current = imm.getProperty ("layout", "split").toString();
    row ("Disposition de la vue immersive"_t, makeCombo (layouts, current == "centered" ? layouts[1] : current == "minimal" ? layouts[2] : layouts[0],
         [this, layouts] (const juce::String& v)
    {
        auto o = ctx.settings().get ("immersive");
        if (! o.isObject()) o = juce::var (new juce::DynamicObject());
        o.getDynamicObject()->setProperty ("layout", v == layouts[1] ? "centered" : v == layouts[2] ? "minimal" : "split");
        ctx.settings().set ("immersive", o);
    }), 36, "Les thèmes (.nxtheme : immersive.json, --nx-immersive-*) et les plug-ins peuvent aussi la modifier."_t);
    const std::pair<const char*, const char*> widgets[] = { { "lyrics", "Paroles synchronisées" }, { "waveform", "Barre de progression en forme d'onde" },
                                                            { "vu", "VU-mètres" }, { "spectrum", "Spectre" }, { "tech", "Infos techniques (DSD / DXD, chemin du signal)" } };
    for (auto& w : widgets)
    {
        const juce::String key (w.first);
        const bool def = key == "lyrics" || key == "waveform" || key == "tech";
        row (tr (juce::String::fromUTF8 (w.second)), makeToggle ((bool) imm.getProperty (key, def), [this, key] (bool v)
        {
            auto o = ctx.settings().get ("immersive");
            if (! o.isObject()) o = juce::var (new juce::DynamicObject());
            o.getDynamicObject()->setProperty (key, v);
            ctx.settings().set ("immersive", o);
        }));
    }

    section ("Assistant"_t);
    row ("Assistant de premier démarrage (langue, disposition, DAC, sources)"_t, makeButton ("Relancer l'assistant"_t, [this] { ctx.showOnboarding(); }));
    if (ctx.discordUrl().isNotEmpty())
    {
        section ("Communauté"_t);
        row ("Serveur Discord NX Audio : entraide, nouveautés, presets"_t, makeButton ("Rejoindre la communauté NX Audio"_t, [this] { ctx.openDiscord(); }), 36);
    }
}

void SettingsPage::buildRemote()
{
    section ("Aura Remote");
    row ("Activer Aura Remote (Wi-Fi, à la demande)"_t, makeToggle (ctx.remote().isRunning(), [this] (bool v) { ctx.setRemoteEnabled (v); refresh(); }), 36,
         "Un serveur web local démarre sur cet ordinateur : scannez le QR code une seule fois avec votre téléphone (même réseau Wi-Fi). Le lien reste identique d'une session à l'autre : gardez l'onglet ou ajoutez-le aux favoris. Réactivé automatiquement au lancement si vous l'avez laissé allumé."_t);
    if (ctx.remote().isRunning())
    {
        fullRow (makeRemotePanel (ctx).release(), RemotePanelHeight);
        row ("Régénérer le lien et le QR code (révoque les appareils déjà connectés)"_t, makeButton ("Nouveau code"_t, [this] { ctx.remote().regenerateToken(); refresh(); }));
    }
}

void SettingsPage::buildTools()
{
    section ("Outils Studio"_t);
    auto& st = ctx.studioTools();
    const juce::StringArray monitors { "Stéréo"_t, "Mono (L+R)", "Solo Mid (M)", "Solo Side (S)" };
    row ("Écoute"_t, makeCombo (monitors, monitors[(int) st.monitor.load()], [this, monitors] (const juce::String& v)
    {
        ctx.studioTools().monitor = (StudioToolsState::Monitor) juce::jmax (0, monitors.indexOf (v));
        ctx.applyStudioTools();
    }), 36, "Contrôles de mastering, en 64 bits. Tout outil actif fait sortir du bit-perfect (indiqué dans le chemin du signal) ; le DSD passe alors en DXD."_t);
    row (juce::String::fromUTF8 ("Inversion de phase (\xc3\x98)"), makeToggle (st.invertPhase.load(), [this] (bool v) { ctx.studioTools().invertPhase = v; ctx.applyStudioTools(); }));
    row ("Smart Crossfeed (casque, Bauer 700 Hz / 4,5 dB)"_t, makeToggle (st.crossfeed.load(), [this] (bool v) { ctx.studioTools().crossfeed = v; ctx.applyStudioTools(); }), 36,
         "Mélange une part filtrée de chaque voie dans l'autre, comme des enceintes ; inactif en mono."_t);

    section ("AuraEngine X");
    const auto configured = ctx.settings().get ("auraEnginePath").toString();
    const auto app = aura::findApplication (configured);
    row (app.exists() ? "Application : "_t + app.getFullPathName() : juce::String ("AuraEngine X non trouvé"_t),
         makeButton ("Choisir…"_t, [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> ("AuraEngine X"_t, juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                              [this, chooser] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.exists()) return;
            ctx.settings().set ("auraEnginePath", f.getFullPathName());
            refresh();
        });
    }), 36, "Clic droit sur un morceau ou un album › « Analyser / Convertir dans AuraEngine X » : les fichiers s'ouvrent dans l'application (analyse, puis conversion DSD / PCM)."_t);
}

void SettingsPage::buildAbout()
{
    section ("Licence & Mises à jour"_t);
    fullRow (new LicenseCard (ctx, [this] { refresh(); }), LicenseCard::idealHeight (ctx));
    section ("À propos"_t);
    struct LogoHeader : juce::Component
    {
        juce::Colour text;
        void paint (juce::Graphics& g) override
        {
            logo::draw (g, getLocalBounds().removeFromLeft (getHeight()).toFloat());
            g.setColour (text);
            g.setFont (style::font (28.0f, true));
            g.drawText ("Aura", getLocalBounds().withTrimmedLeft (getHeight() + 16), juce::Justification::centredLeft);
        }
    };
    auto* header = new LogoHeader();
    header->text = ctx.theme().textCol();
    fullRow (header, 104);
    row ("Aura " NX_VERSION " · NX Audio Suite · logiciel propriétaire"_t, nullptr, 26);
    if (ctx.discordUrl().isNotEmpty())
        row ("Questions, idées, presets d'égaliseur : rejoignez les autres auditeurs"_t,
             makeButton ("Rejoindre la communauté NX Audio"_t, [this] { ctx.openDiscord(); }), 36);
    row ("Un bug ? Exportez les journaux (aucune donnée personnelle ni clé de licence) et envoyez le fichier au support"_t,
         makeButton ("Exporter les journaux (.zip)"_t, [this] { ctx.exportDiagnostics(); }), 36);
    if (! ctx.isPremium())
        row ("DSD, DXD, SACD, mode exclusif, profils matériels, Aura Remote"_t, makeButton ("Passer à Aura Premium"_t, [this] { ctx.openShop(); }), 36);

    section ("Licences & mentions légales"_t);
    struct Title : juce::Component
    {
        juce::Colour c;
        void paint (juce::Graphics& g) override
        {
            g.setColour (c);
            g.setFont (style::font (16.0f, true));
            g.drawText ("Aura — Licences et crédits"_t, getLocalBounds(), juce::Justification::centredLeft);
        }
    };
    auto* title = new Title();
    title->c = ctx.theme().textCol();
    fullRow (title, 28);
    fullRow (new NoticeText (creditsText(), 13.0f, ctx.theme().textCol().withAlpha (0.85f)), 400);
    row ("Textes complets des licences des composants tiers"_t, makeButton ("Afficher Licences.txt"_t, [this] { showLicencesFile (ctx.theme()); }), 36);
    section ("Marques"_t);
    fullRow (new NoticeText (trademarkNotice(), 12.5f, ctx.theme().textDim()), 120);
}

void SettingsPage::layoutContent (int width)
{
    const int pad = 32;
    const int inner = juce::jmax (200, width - pad * 2);
    int y = embedded ? 12 : 24 + 60;
    painted.clear();
    for (auto& r : layoutRows)
    {
        if (r.isSection)
        {
            painted.push_back ({ { pad, y, inner, r.height }, r });
            y += r.height;
            continue;
        }
        if (r.full)
        {
            if (auto* n = dynamic_cast<NoticeText*> (r.control)) r.height = n->heightFor (juce::jmin (inner, 980));
            r.control->setBounds (pad, y, juce::jmin (inner, 980), r.height);
            y += r.height + 12;
            continue;
        }
        const int controlW = juce::jmin (380, inner / 2);
        auto rowArea = juce::Rectangle<int> (pad, y, inner, r.height + (r.help.isNotEmpty() ? 20 : 0));
        if (r.control != nullptr)
            r.control->setBounds (rowArea.getRight() - controlW, y, controlW, r.height);
        painted.push_back ({ rowArea.withTrimmedRight (r.control != nullptr ? controlW + 20 : 0), r });
        y += rowArea.getHeight() + 10;
    }
    labels.setBounds (0, 0, width, y + 120);
    setContentHeight (y + 120);   // bottom margin: the last block is never glued to the window edge
}

void SettingsPage::Labels::paint (juce::Graphics& g)
{
    const auto& t = p.ctx.theme();
    if (! p.embedded) drawPageTitle (g, t, { 32, 24, getWidth() - 64, 54 }, "Réglages"_t);
    const auto clip = g.getClipBounds();
    for (auto& [area, r] : p.painted)
    {
        if (! area.expanded (0, 24).intersects (clip)) continue;     // only the rows being scrolled into view
        if (r.isSection)
        {
            g.setColour (t.accent());
            g.setFont (style::font (12.5f, true));
            g.drawText (style::upper (r.label), area.withTrimmedTop (18), juce::Justification::centredLeft);
            g.setColour (t.border());
            g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
            continue;
        }
        auto a = area;
        g.setColour (t.textCol());
        g.setFont (style::font (14.0f));
        g.drawFittedText (r.label, a.removeFromTop (r.height), juce::Justification::centredLeft, 2);
        if (r.help.isNotEmpty())
        {
            g.setColour (t.textDim());
            g.setFont (style::font (12.0f));
            g.drawFittedText (r.help, a.withHeight (20), juce::Justification::centredLeft, 2);
        }
    }
}

} // namespace nx
