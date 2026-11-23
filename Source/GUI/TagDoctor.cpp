// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "TagDoctor.h"

namespace nx
{

TagDoctor::TagDoctor (AppContext& c)
    : scanButton ("Analyser la bibliothèque"_t), dupTab ("Doublons"_t), auditTab ("Pochettes & étiquettes"_t),
      hideButton ("Masquer les versions inférieures"_t), unhideButton ("Réafficher les versions masquées"_t),
      trashButton ("Versions inférieures à la corbeille…"_t), coverButton ("Choisir une image…"_t), onlineButton ("Chercher en ligne"_t),
      uniformButton ("Uniformiser l'album…"_t), confirmButton ("Confirmer"_t), cancelButton ("Annuler"_t),
      onlineToggle ("Recherche de pochettes en ligne (iTunes)"_t), closeButton (style::Icon::Close, "Fermer (Échap)"_t, c), ctx (c)
{
    setWantsKeyboardFocus (true);
    setVisible (false);
    for (auto* b : { &scanButton, &dupTab, &auditTab, &hideButton, &unhideButton, &trashButton, &coverButton, &onlineButton, &uniformButton,
                     &confirmButton, &cancelButton })
        addAndMakeVisible (b);
    addAndMakeVisible (onlineToggle);
    addAndMakeVisible (closeButton);
    viewport.setViewedComponent (&body, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    closeButton.onClick = [this] { close(); };
    dupTab.onClick = [this] { setTab (Tab::Duplicates); };
    auditTab.onClick = [this] { setTab (Tab::Audit); };
    for (auto* b : { &dupTab, &auditTab }) { b->setClickingTogglesState (false); b->setRadioGroupId (0); }
    scanButton.onClick = [this]
    {
        // in the background: covers are decoded to know their size
        scanButton.setEnabled (false);
        scanButton.setButtonText ("Analyse en cours…"_t);
        std::weak_ptr<int> weak = alive;
        juce::Component::SafePointer<TagDoctor> safe (this);
        auto& lib = ctx.library();
        juce::Thread::launch ([weak, safe, &lib]
        {
            auto d = std::make_shared<std::vector<doctor::DuplicateGroup>> (doctor::findDuplicates (lib));
            auto a = std::make_shared<doctor::Audit> (doctor::audit (lib));
            juce::MessageManager::callAsync ([weak, safe, d, a]
            {
                if (weak.expired() || safe == nullptr) return;
                safe->dups = std::move (*d);
                safe->auditResult = std::move (*a);
                safe->scanned = true;
                safe->scanButton.setEnabled (true);
                safe->scanButton.setButtonText ("Analyser la bibliothèque"_t);
                safe->rebuildRows();
            });
        });
    };
    hideButton.onClick = [this]
    {
        const int n = hideLowerVersions();
        ctx.showMessage (juce::String (n) + " version(s) masquée(s) dans la bibliothèque (réversible, les fichiers restent en place)"_t);
    };
    unhideButton.onClick = [this] { const int n = showHiddenAgain(); ctx.showMessage (juce::String (n) + " version(s) réaffichée(s)"_t); };
    trashButton.onClick = [this] { requestTrashLowerVersions(); };
    coverButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Image de pochette"_t, juce::File(), "*.jpg;*.jpeg;*.png");
        juce::Component::SafePointer<TagDoctor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile()) safe->requestCoverFromFile (fc.getResult());
        });
    };
    onlineButton.onClick = [this] { requestOnlineCover(); };
    uniformButton.onClick = [this] { requestUniform(); };
    confirmButton.onClick = [this]
    {
        const int n = confirmPending();
        ctx.showMessage (juce::String (n) + " élément(s) modifié(s)"_t);
    };
    cancelButton.onClick = [this] { cancelPending(); };
    onlineToggle.setToggleState ((bool) ctx.settings().get ("doctorOnlineCovers", false), juce::dontSendNotification);
    onlineToggle.setTooltip ("Désactivé par défaut : aucune requête réseau. Activé : la pochette est cherchée sur l'API publique iTunes Search (1200 px)."_t);
    onlineToggle.onClick = [this] { ctx.settings().set ("doctorOnlineCovers", onlineToggle.getToggleState()); updateButtons(); };
    updateButtons();
}

TagDoctor::~TagDoctor() = default;

void TagDoctor::open()
{
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    if (! scanned) scanNow();
}

void TagDoctor::close()
{
    if (! isVisible()) return;
    cancelPending();
    setVisible (false);
    if (auto* p = getParentComponent()) p->grabKeyboardFocus();
}

void TagDoctor::scanNow()
{
    dups = doctor::findDuplicates (ctx.library());
    auditResult = doctor::audit (ctx.library());
    scanned = true;
    rebuildRows();
}

void TagDoctor::setTab (Tab t)
{
    tab = t;
    cancelPending();
    selected = -1;
    rebuildRows();
}

void TagDoctor::rebuildRows()
{
    rows.clear();
    const auto dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
    if (tab == Tab::Duplicates)
    {
        for (int g = 0; g < (int) dups.size(); ++g)
        {
            const auto& grp = dups[(size_t) g];
            rows.push_back ({ "groupe", grp.artist + juce::String::fromUTF8 (" \xe2\x80\x94 ") + grp.title,
                              juce::String ((int) grp.versions.size()) + " versions"_t, g, -1, true });
            for (int v = 0; v < (int) grp.versions.size(); ++v)
            {
                const auto& ver = grp.versions[(size_t) v];
                rows.push_back ({ v == 0 ? "meilleure" : "inferieure", (v == 0 ? juce::String::fromUTF8 ("\xe2\x98\x85 ") : juce::String ("    ")) + ver.label,
                                  ver.track.externalId, g, v, false });
            }
        }
        if (rows.empty()) rows.push_back ({ "vide", "Aucun doublon dans la bibliothèque locale"_t, {}, -1, -1, true });
    }
    else
    {
        const auto& a = auditResult;
        rows.push_back ({ "titre", "Albums sans pochette"_t + " (" + juce::String ((int) a.missingCovers.size()) + ")", {}, -1, -1, true });
        for (int i = 0; i < (int) a.missingCovers.size(); ++i)
            rows.push_back ({ "sans-pochette", a.missingCovers[(size_t) i].album.artist + juce::String::fromUTF8 (" \xe2\x80\x94 ") + a.missingCovers[(size_t) i].album.title, {}, 0, i, false });
        rows.push_back ({ "titre", "Pochettes sous 500 × 500 px"_t + " (" + juce::String ((int) a.lowResCovers.size()) + ")", {}, -1, -1, true });
        for (int i = 0; i < (int) a.lowResCovers.size(); ++i)
        {
            const auto& c = a.lowResCovers[(size_t) i];
            rows.push_back ({ "basse-resolution", c.album.artist + juce::String::fromUTF8 (" \xe2\x80\x94 ") + c.album.title,
                              juce::String (c.width) + " × "_t + juce::String (c.height) + " px", 1, i, false });
        }
        rows.push_back ({ "titre", "Albums incohérents (artistes / années)"_t + " (" + juce::String ((int) a.inconsistentAlbums.size()) + ")", {}, -1, -1, true });
        for (int i = 0; i < (int) a.inconsistentAlbums.size(); ++i)
        {
            const auto& f = a.inconsistentAlbums[(size_t) i];
            rows.push_back ({ "incoherent", f.album + dot + f.artist + (f.otherArtists.isEmpty() ? juce::String() : " + " + f.otherArtists.joinIntoString (", ")),
                              f.years ? "années différentes"_t : "album éclaté entre plusieurs artistes"_t, 2, i, false });
        }
        rows.push_back ({ "titre", "Pistes aux étiquettes incomplètes"_t + " (" + juce::String ((int) a.incompleteTags.size()) + ")", {}, -1, -1, true });
        for (int i = 0; i < (int) a.incompleteTags.size(); ++i)
        {
            const auto& tg = a.incompleteTags[(size_t) i];
            rows.push_back ({ "etiquettes", (tg.track.title.isNotEmpty() ? tg.track.title : juce::File (tg.track.externalId).getFileName()),
                              "manque : "_t + tg.missing.joinIntoString (", "), 3, i, false });
        }
    }
    if (selected >= (int) rows.size()) selected = -1;
    body.setSize (juce::jmax (100, viewport.getMaximumVisibleWidth()), (int) rows.size() * rowH + 12);
    updateButtons();
    body.repaint();
    repaint();
}

juce::StringArray TagDoctor::rowTexts() const
{
    juce::StringArray out;
    for (auto& r : rows) out.add (r.kind + "|" + r.text + (r.sub.isNotEmpty() ? " | " + r.sub : juce::String()));
    return out;
}

void TagDoctor::selectRow (int index)
{
    selected = index >= 0 && index < (int) rows.size() && ! rows[(size_t) index].header ? index : -1;
    cancelPending();
    updateButtons();
    body.repaint();
}

const AlbumInfo* TagDoctor::selectedAlbum() const
{
    if (selected < 0) return nullptr;
    const auto& r = rows[(size_t) selected];
    if (r.kind == "sans-pochette") return &auditResult.missingCovers[(size_t) r.item].album;
    if (r.kind == "basse-resolution") return &auditResult.lowResCovers[(size_t) r.item].album;
    return nullptr;
}

void TagDoctor::updateButtons()
{
    const bool dup = tab == Tab::Duplicates;
    int lower = 0;
    for (auto& g : dups) lower += (int) g.versions.size() - 1;
    const int hidden = ctx.library().getHiddenPaths().size();
    hideButton.setVisible (dup);
    trashButton.setVisible (dup);
    unhideButton.setVisible (dup);
    hideButton.setEnabled (lower > 0);
    trashButton.setEnabled (lower > 0);
    unhideButton.setEnabled (hidden > 0);
    hideButton.setButtonText ("Masquer les versions inférieures"_t + " (" + juce::String (lower) + ")");
    unhideButton.setButtonText ("Réafficher les versions masquées"_t + " (" + juce::String (hidden) + ")");
    const auto* album = selectedAlbum();
    const bool fix = selected >= 0 && rows[(size_t) selected].kind == "incoherent";
    for (auto* b : { &coverButton, &onlineButton, &uniformButton }) b->setVisible (! dup);
    onlineToggle.setVisible (! dup);
    coverButton.setEnabled (album != nullptr);
    onlineButton.setEnabled (album != nullptr && onlineToggle.getToggleState());
    uniformButton.setEnabled (fix);
    dupTab.setToggleState (dup, juce::dontSendNotification);
    auditTab.setToggleState (! dup, juce::dontSendNotification);
    confirmButton.setVisible (pending != nullptr);
    cancelButton.setVisible (pending != nullptr);
    resized();
}

void TagDoctor::arm (const juce::String& text, std::function<int()> action)
{
    pendingText = text;
    pending = std::move (action);
    updateButtons();
    repaint();
}

int TagDoctor::confirmPending()
{
    if (pending == nullptr) return 0;
    auto action = std::move (pending);
    pending = nullptr;
    pendingText.clear();
    const int n = action();
    ctx.library().notifyChanged();
    scanNow();
    return n;
}

void TagDoctor::cancelPending()
{
    pending = nullptr;
    pendingText.clear();
    updateButtons();
    repaint();
}

int TagDoctor::hideLowerVersions()
{
    int n = 0;
    for (auto& g : dups)
        for (size_t v = 1; v < g.versions.size(); ++v)
            if (ctx.library().hideTrack (g.versions[v].track.source, g.versions[v].track.externalId)) ++n;
    ctx.library().notifyChanged();
    scanNow();
    return n;
}

int TagDoctor::showHiddenAgain()
{
    const int n = ctx.library().unhideAll();
    ctx.library().notifyChanged();
    scanNow();
    return n;
}

void TagDoctor::requestTrashLowerVersions()
{
    std::vector<TrackInfo> lower;
    for (auto& g : dups)
        for (size_t v = 1; v < g.versions.size(); ++v) lower.push_back (g.versions[v].track);
    if (lower.empty()) return;
    arm ("Déplacer "_t + juce::String ((int) lower.size()) + " fichier(s) vers la corbeille du système ? La meilleure version de chaque morceau est gardée."_t,
         [this, lower]
         {
             int n = 0;
             for (auto& t : lower) if (doctor::moveToTrash (ctx.library(), t)) ++n;
             if (n < (int) lower.size()) ctx.showMessage ("Certains fichiers n'ont pas pu aller à la corbeille : ils sont restés en place"_t);
             return n;
         });
}

void TagDoctor::requestCoverFromFile (const juce::File& image)
{
    const auto* album = selectedAlbum();
    if (album == nullptr || ! image.existsAsFile()) return;
    const auto a = *album;
    arm ((a.artworkKey.isEmpty() ? "Appliquer cette image comme pochette de « "_t : "Remplacer la pochette de « "_t) + a.title
             + " » ? (bibliothèque seulement, les fichiers audio ne sont pas modifiés)"_t,
         [this, a, image] { return ctx.library().updateAlbum (a.artist, a.title, a.title, a.artist, 0, image); });
}

void TagDoctor::requestOnlineCover()
{
    const auto* album = selectedAlbum();
    if (album == nullptr || ! onlineToggle.getToggleState()) return;
    const auto a = *album;
    onlineButton.setEnabled (false);
    std::weak_ptr<int> weak = alive;
    juce::Component::SafePointer<TagDoctor> safe (this);
    const auto tmp = ctx.userDataDirectory().getChildFile ("cover-download.jpg");
    juce::Thread::launch ([weak, safe, a, tmp]
    {
        const auto json = doctor::itunesSearchUrl (a.artist, a.title).readEntireTextStream();
        const auto art = doctor::itunesArtworkFromJson (json);
        juce::MemoryBlock mb;
        const bool ok = art.isNotEmpty() && juce::URL (art).readEntireBinaryStream (mb) && mb.getSize() > 1000 && tmp.replaceWithData (mb.getData(), mb.getSize());
        juce::MessageManager::callAsync ([weak, safe, ok, tmp]
        {
            if (weak.expired() || safe == nullptr) return;
            safe->updateButtons();
            if (ok) safe->requestCoverFromFile (tmp);
            else safe->ctx.showMessage ("Aucune pochette trouvée en ligne pour cet album"_t);
        });
    });
}

void TagDoctor::requestUniform()
{
    if (selected < 0 || rows[(size_t) selected].kind != "incoherent") return;
    const auto fix = auditResult.inconsistentAlbums[(size_t) rows[(size_t) selected].item];
    arm ("Uniformiser « "_t + fix.album + " » : artiste de l'album « "_t + fix.artist + " »" + (fix.year > 0 ? ", année "_t + juce::String (fix.year) : juce::String())
             + " ? (bibliothèque seulement, les étiquettes des fichiers ne sont pas modifiées)"_t,
         [this, fix] { return doctor::makeUniform (ctx.library(), fix); });
}

void TagDoctor::resized()
{
    panel = getLocalBounds().withSizeKeepingCentre (juce::jmin (1040, getWidth() - 48), juce::jmin (760, getHeight() - 40));
    closeButton.setBounds (panel.getRight() - 52, panel.getY() + 14, 36, 36);
    auto r = panel.reduced (24, 20);
    r.removeFromTop (60);
    auto top = r.removeFromTop (34);
    dupTab.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (6);
    auditTab.setBounds (top.removeFromLeft (200));
    scanButton.setBounds (top.removeFromRight (220));
    r.removeFromTop (12);
    auto actions = r.removeFromBottom (34);
    if (tab == Tab::Duplicates)
    {
        hideButton.setBounds (actions.removeFromLeft (300));
        actions.removeFromLeft (8);
        unhideButton.setBounds (actions.removeFromLeft (290));
        actions.removeFromLeft (8);
        trashButton.setBounds (actions);
    }
    else
    {
        coverButton.setBounds (actions.removeFromLeft (180));
        actions.removeFromLeft (8);
        onlineButton.setBounds (actions.removeFromLeft (170));
        actions.removeFromLeft (8);
        uniformButton.setBounds (actions.removeFromLeft (200));
        actions.removeFromLeft (12);
        onlineToggle.setBounds (actions);
    }
    r.removeFromBottom (10);
    confirmBar = pending != nullptr ? r.removeFromBottom (46) : juce::Rectangle<int>();
    if (pending != nullptr)
    {
        auto cb = confirmBar.reduced (10, 7);
        cancelButton.setBounds (cb.removeFromRight (110));
        cb.removeFromRight (8);
        confirmButton.setBounds (cb.removeFromRight (130));
        r.removeFromBottom (8);
    }
    viewport.setBounds (r);
    body.setSize (juce::jmax (100, r.getWidth() - viewport.getScrollBarThickness()), (int) rows.size() * rowH + 12);
}

void TagDoctor::paint (juce::Graphics& g)
{
    const auto& t = ctx.theme();
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    juce::Path p;
    p.addRoundedRectangle (panel.toFloat(), 14.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 28, { 0, 10 }).drawForPath (g, p);
    g.setColour (t.surface());
    g.fillPath (p);
    g.setColour (t.border());
    g.strokePath (p, juce::PathStrokeType (1.0f));
    g.setColour (t.textCol());
    g.setFont (style::font (20.0f, true));
    g.drawText ("Tag & Cover Doctor", panel.getX() + 24, panel.getY() + 16, 400, 28, juce::Justification::centredLeft);
    g.setColour (t.textDim());
    g.setFont (style::font (13.0f));
    g.drawText ("Doublons classés par qualité, pochettes et étiquettes. Rien n'est supprimé ni réécrit sans votre confirmation."_t,
                panel.getX() + 24, panel.getY() + 44, panel.getWidth() - 100, 20, juce::Justification::centredLeft, true);
    if (pending != nullptr)
    {
        g.setColour (juce::Colour (0xffff8a3d).withAlpha (0.14f));
        g.fillRoundedRectangle (confirmBar.toFloat(), 10.0f);
        g.setColour (juce::Colour (0xffff8a3d));
        g.drawRoundedRectangle (confirmBar.toFloat().reduced (0.5f), 10.0f, 1.0f);
        g.setColour (t.textCol());
        g.setFont (style::font (13.0f, true));
        g.drawFittedText (pendingText, confirmBar.reduced (14, 4).withTrimmedRight (260), juce::Justification::centredLeft, 2);
    }
}

void TagDoctor::Body::paint (juce::Graphics& g)
{
    const auto& t = doctor.ctx.theme();
    for (int i = 0; i < (int) doctor.rows.size(); ++i)
    {
        const auto& r = doctor.rows[(size_t) i];
        auto area = juce::Rectangle<int> (0, i * rowH, getWidth(), rowH);
        if (i == doctor.selected)
        {
            g.setColour (t.accent().withAlpha (0.16f));
            g.fillRoundedRectangle (area.toFloat().reduced (0, 1), 6.0f);
        }
        area.removeFromLeft (r.header ? 4 : 18);
        g.setColour (r.kind == "meilleure" ? juce::Colour (0xff3ecf8e) : r.header ? t.textCol() : t.textCol().withAlpha (0.85f));
        g.setFont (style::font (r.header ? 14.0f : 13.0f, r.header || r.kind == "meilleure"));
        const int textW = r.sub.isNotEmpty() ? area.getWidth() * 55 / 100 : area.getWidth();
        g.drawText (r.text, area.removeFromLeft (textW), juce::Justification::centredLeft, true);
        if (r.sub.isNotEmpty())
        {
            g.setColour (t.textDim());
            g.setFont (style::font (12.0f));
            g.drawText (r.sub, area.reduced (8, 0), juce::Justification::centredLeft, true);
        }
    }
}

void TagDoctor::Body::mouseUp (const juce::MouseEvent& e) { doctor.selectRow (e.getPosition().y / rowH); }

} // namespace nx
