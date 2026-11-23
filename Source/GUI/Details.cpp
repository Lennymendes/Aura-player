// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "Details.h"
#include "Waveform.h"
#include "../Library/TagReader.h"
#include <set>

namespace nx
{

namespace
{
    juce::String yesNo (bool b) { return b ? "Oui"_t : "Non"_t; }

    juce::String tagFamily (const juce::File& f)
    {
        const auto e = f.getFileExtension().toLowerCase();
        if (e == ".flac" || e == ".ogg" || e == ".opus") return "Vorbis";
        if (e == ".m4a" || e == ".mp4" || e == ".alac" || e == ".aac") return "MP4";
        if (e == ".mp3" || e == ".dsf" || e == ".dff" || e == ".aif" || e == ".aiff" || e == ".wav") return e == ".wav" ? "ID3v2 / RIFF INFO" : "ID3v2";
        return {};
    }

    juce::String bitDepth (const TrackInfo& t)
    {
        if (t.isDsd) return "1 bit (DSD" + juce::String ((int) std::lround (t.sampleRate / 44100.0)) + ")";
        return t.bitsPerSample > 0 ? juce::String (t.bitsPerSample) + " bits" : juce::String();
    }
}

std::vector<DetailSection> trackDetails (const TrackInfo& t, double truePeakDb, double ceilingDb)
{
    std::vector<DetailSection> out;
    auto add = [] (DetailSection& s, const juce::String& label, const juce::String& value) { if (value.isNotEmpty()) s.rows.push_back ({ label, value }); };

    DetailSection track { "Morceau"_t, {} };
    add (track, "Titre"_t, t.title);
    add (track, "Artiste"_t, t.artist);
    add (track, "Artiste de l'album"_t, t.albumArtist);
    add (track, "Album"_t, t.album);
    add (track, "Numéro"_t, t.trackNumber > 0 ? (t.discNumber > 0 ? juce::String (t.discNumber) + juce::String::fromUTF8 (" \xc2\xb7 ") : juce::String()) + juce::String (t.trackNumber) : juce::String());
    add (track, "Année"_t, t.year > 0 ? juce::String (t.year) : juce::String());
    add (track, "Genre"_t, t.genre);
    add (track, "Compositeur"_t, t.composer);
    add (track, "Durée"_t, t.duration > 0.0 ? style::formatTime (t.duration) : juce::String());
    add (track, "Lectures"_t, t.playCount > 0 ? juce::String (t.playCount) : juce::String());
    out.push_back (std::move (track));

    DetailSection audio { "Audio"_t, {} };
    add (audio, "Format"_t, t.codec);
    add (audio, "Fréquence d'échantillonnage"_t, t.sampleRate > 0.0 ? formatRate (t.sampleRate) : juce::String());
    add (audio, "Profondeur"_t, bitDepth (t));
    add (audio, "Canaux"_t, t.channels > 0 ? juce::String (t.channels) + " (" + channelBadge (t.channels) + ")" : juce::String());
    if (t.fileSize > 0 && t.duration > 0.0)
        add (audio, "Débit moyen"_t, juce::String (juce::roundToInt ((double) t.fileSize * 8.0 / t.duration / 1000.0)) + " kb/s");
    add (audio, "Compression"_t, t.codec.isNotEmpty() ? (t.lossy ? "Avec perte"_t : "Sans perte"_t) : juce::String());
    add (audio, "Résolution"_t, t.resolutionBadge());
    {
        const auto num = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (v, 1).replaceCharacter ('.', ','); };
        add (audio, "True Peak", std::isfinite (truePeakDb) ? num (truePeakDb) + " dBTP (ITU-R BS.1770-4)"
                                                            : juce::String ("Non mesuré (mesuré en tâche de fond à la lecture d'un fichier)"_t));
        juce::String ah;
        if (! std::isfinite (ceilingDb)) ah = "Désactivé"_t;
        else if (t.isDsd) ah = "Source DSD : affichage seulement"_t;
        else if (! std::isfinite (truePeakDb)) ah = "En attente de la mesure"_t;
        else if (truePeakDb > ceilingDb) ah = num (ceilingDb - truePeakDb) + " dB (" + "plafond "_t + num (ceilingDb) + " dBTP)";
        else ah = "0 dB, sous le plafond de "_t + num (ceilingDb) + " dBTP";
        add (audio, "Auto-Headroom", ah);
    }
    out.push_back (std::move (audio));

    DetailSection file { "Fichier"_t, {} };
    const bool local = t.source == "local" && t.externalId.isNotEmpty();
    add (file, "Source"_t, local ? "Fichier local"_t : t.source.startsWith ("upnp:") ? "Serveur réseau (NAS / Jellyfin)"_t : t.source);
    add (file, local ? "Chemin"_t : "Adresse"_t, local ? t.externalId : t.uri);
    add (file, "Taille"_t, t.fileSize > 0 ? juce::File::descriptionOfSizeInBytes (t.fileSize) : juce::String());
    add (file, "Modifié"_t, t.modified > 0 ? juce::Time (t.modified).toString (true, true) : juce::String());
    add (file, "Ajouté à la bibliothèque"_t, t.added > 0 ? juce::Time (t.added).toString (true, true) : juce::String());
    add (file, "Paroles"_t, t.lyricsPath.isNotEmpty() ? t.lyricsPath : juce::String());
    out.push_back (std::move (file));

    // what the file itself carries
    const juce::File f (t.externalId);
    TagData tags;
    if (local && f.existsAsFile() && readTags (f, tags))
    {
        DetailSection s { "Étiquettes"_t + (tagFamily (f).isNotEmpty() ? " (" + tagFamily (f) + ")" : juce::String()), {} };
        auto keys = tags.fields.getAllKeys();
        keys.sort (true);
        for (auto& k : keys) add (s, k, tags.fields.getValue (k, {}));
        if (tags.picture.getSize() > 0)
            add (s, "Pochette intégrée"_t, (tags.pictureMime.isNotEmpty() ? tags.pictureMime + ", " : juce::String()) + juce::File::descriptionOfSizeInBytes ((juce::int64) tags.picture.getSize()));
        if (s.rows.empty()) add (s, juce::String::fromUTF8 ("\xe2\x80\x94"), "Aucune étiquette dans ce fichier"_t);
        out.push_back (std::move (s));
        if (! tags.credits.isEmpty())
        {
            DetailSection c { "Crédits"_t, {} };
            for (auto& line : tags.credits) add (c, line.upToFirstOccurrenceOf (":", false, false).trim(), line.fromFirstOccurrenceOf (":", false, false).trim().isNotEmpty() ? line.fromFirstOccurrenceOf (":", false, false).trim() : line);
            out.push_back (std::move (c));
        }
    }
    else
    {
        DetailSection s { "Étiquettes"_t, {} };
        add (s, juce::String::fromUTF8 ("\xe2\x80\x94"), local ? "Fichier introuvable : valeurs de la bibliothèque ci-dessus"_t
                           : "Source réseau ou flux : les étiquettes du fichier ne sont pas lisibles, valeurs de la bibliothèque ci-dessus"_t);
        out.push_back (std::move (s));
        if (t.credits.isNotEmpty())
        {
            DetailSection c { "Crédits"_t, {} };
            for (auto& line : juce::StringArray::fromLines (t.credits)) add (c, line.upToFirstOccurrenceOf (":", false, false).trim(), line.fromFirstOccurrenceOf (":", false, false).trim());
            out.push_back (std::move (c));
        }
    }
    return out;
}

std::vector<DetailSection> albumDetails (const std::vector<TrackInfo>& tracks, const juce::String& albumTitle)
{
    std::vector<DetailSection> out;
    DetailSection sum { "Album"_t, {} };
    auto add = [] (DetailSection& s, const juce::String& l, const juce::String& v) { if (v.isNotEmpty()) s.rows.push_back ({ l, v }); };
    double seconds = 0.0, maxRate = 0.0;
    juce::int64 bytes = 0;
    int maxBits = 0, year = 0;
    std::set<juce::String> codecs, sources;
    juce::String artist;
    for (auto& t : tracks)
    {
        seconds += t.duration;
        bytes += t.fileSize;
        maxRate = juce::jmax (maxRate, t.sampleRate);
        maxBits = t.isDsd ? maxBits : juce::jmax (maxBits, t.bitsPerSample);
        year = juce::jmax (year, t.year);
        if (t.codec.isNotEmpty()) codecs.insert (t.codec);
        sources.insert (t.source.startsWith ("upnp:") ? "Serveur réseau"_t : t.source == "local" ? "Fichiers locaux"_t : t.source);
        if (artist.isEmpty()) artist = t.albumArtist.isNotEmpty() ? t.albumArtist : t.artist;
    }
    add (sum, "Titre"_t, albumTitle);
    add (sum, "Artiste"_t, artist);
    add (sum, "Année"_t, year > 0 ? juce::String (year) : juce::String());
    add (sum, "Morceaux"_t, juce::String ((int) tracks.size()));
    add (sum, "Durée totale"_t, seconds > 0.0 ? style::formatTime (seconds) : juce::String());
    juce::StringArray c;
    for (auto& s : codecs) c.add (s);
    add (sum, "Formats"_t, c.joinIntoString (", "));
    add (sum, "Fréquence maximale"_t, maxRate > 0.0 ? formatRate (maxRate) : juce::String());
    add (sum, "Profondeur maximale"_t, maxBits > 0 ? juce::String (maxBits) + " bits" : juce::String());
    add (sum, "Taille totale"_t, bytes > 0 ? juce::File::descriptionOfSizeInBytes (bytes) : juce::String());
    juce::StringArray src;
    for (auto& s : sources) src.add (s);
    add (sum, "Source"_t, src.joinIntoString (", "));
    out.push_back (std::move (sum));

    DetailSection list { "Morceaux"_t, {} };
    for (auto& t : tracks)
    {
        juce::String left = (t.discNumber > 1 ? juce::String (t.discNumber) + "." : juce::String()) + (t.trackNumber > 0 ? juce::String (t.trackNumber) : juce::String::fromUTF8 ("\xc2\xb7"));
        juce::String right = t.title;
        juce::StringArray bits;
        if (t.codec.isNotEmpty()) bits.add (t.codec);
        if (t.resolutionBadge().isNotEmpty()) bits.add (t.resolutionBadge());
        if (t.duration > 0.0) bits.add (style::formatTime (t.duration));
        if (bits.size() > 0) right << juce::String::fromUTF8 ("   \xe2\x80\x94   ") << bits.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 "));
        list.rows.push_back ({ left, right });
    }
    out.push_back (std::move (list));
    return out;
}

juce::String detailsToText (const juce::String& heading, const std::vector<DetailSection>& sections)
{
    juce::String s = heading + "\n";
    for (auto& sec : sections)
    {
        s << "\n[" << sec.title << "]\n";
        for (auto& r : sec.rows) s << r.label << " : " << r.value << "\n";
    }
    return s;
}

//==============================================================================
DetailsSheet::DetailsSheet (AppContext& c)
    : closeButton (style::Icon::Close, "Fermer (Échap)"_t, c), copyButton ("Copier"_t), ctx (c)
{
    setWantsKeyboardFocus (true);
    setVisible (false);
    closeButton.onClick = [this] { close(); };
    copyButton.setTooltip ("Copier ces informations (texte brut), par exemple pour un rapport de bug"_t);
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (getText()); ctx.showMessage ("Informations copiées"_t); };
    addAndMakeVisible (closeButton);
    addAndMakeVisible (copyButton);
    viewport.setViewedComponent (&body, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

void DetailsSheet::open (const std::vector<TrackInfo>& t, const juce::String& albumTitle)
{
    tracks = t;
    if (tracks.empty()) return;
    if (tracks.size() == 1)
    {
        heading = tracks.front().title;
        sub = tracks.front().displayArtist() + (tracks.front().album.isNotEmpty() ? juce::String::fromUTF8 (" \xe2\x80\x94 ") + tracks.front().album : juce::String());
        const auto& t0 = tracks.front();
        double tp = std::numeric_limits<double>::quiet_NaN();
        if (t0.source == "local")
            if (const auto v = ctx.library().getTruePeak (t0.externalId)) tp = *v;
        if (! std::isfinite (tp))
            if (const auto a = ctx.trackAnalyzer().get (t0.source == "local" ? t0.externalId : t0.uri)) tp = a->truePeakDb;
        const auto st = ctx.engine().getSettings();
        sections = trackDetails (t0, tp, st.autoTruePeak ? st.truePeakCeilingDb : std::numeric_limits<double>::quiet_NaN());
    }
    else
    {
        heading = albumTitle;
        sub = juce::String ((int) tracks.size()) + " " + "morceaux"_t;
        sections = albumDetails (tracks, albumTitle);
    }
    setVisible (true);
    toFront (true);
    resized();
    viewport.setViewPosition (0, 0);
    grabKeyboardFocus();
    repaint();
}

void DetailsSheet::close()
{
    if (! isVisible()) return;
    setVisible (false);
    if (auto* p = getParentComponent()) p->grabKeyboardFocus();
}

void DetailsSheet::resized()
{
    panel = getLocalBounds().withSizeKeepingCentre (juce::jmin (780, getWidth() - 48), juce::jmin (getHeight() - 64, 760));
    closeButton.setBounds (panel.getRight() - 50, panel.getY() + 14, 36, 36);
    copyButton.setBounds (closeButton.getX() - 96, panel.getY() + 16, 88, 32);
    auto v = panel.withTrimmedTop (74).reduced (4, 0).withTrimmedBottom (10);
    viewport.setBounds (v);
    body.setSize (v.getWidth() - viewport.getScrollBarThickness(), body.layoutFor (v.getWidth() - viewport.getScrollBarThickness()));
}

void DetailsSheet::paint (juce::Graphics& g)
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
    g.setFont (style::font (19.0f, true));
    g.drawText (heading, panel.getX() + 24, panel.getY() + 14, copyButton.getX() - panel.getX() - 36, 28, juce::Justification::centredLeft, true);
    g.setColour (t.textDim());
    g.setFont (style::font (13.0f));
    g.drawText (sub, panel.getX() + 24, panel.getY() + 42, copyButton.getX() - panel.getX() - 36, 22, juce::Justification::centredLeft, true);
}

namespace
{
    constexpr int labelW = 190, pad = 20, rowGap = 5;
    juce::TextLayout layoutText (const juce::String& text, float width, const juce::Font& font, juce::Colour c)
    {
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byChar);          // paths and URLs have no spaces
        as.append (text, font, c);
        juce::TextLayout tl;
        tl.createLayout (as, width);
        return tl;
    }
}

int DetailsSheet::Body::layoutFor (int width)
{
    lines.clear();
    int y = 6;
    const auto font = style::font (13.0f);
    const bool album = sheet.tracks.size() > 1;
    for (auto& sec : sheet.sections)
    {
        y += 18 + 8;                                            // section title
        for (size_t i = 0; i < sec.rows.size(); ++i)
        {
            const auto tl = layoutText (sec.rows[i].value, (float) (width - labelW - 2 * pad), font, juce::Colours::white);
            const int h = juce::jmax (18, (int) std::ceil (tl.getHeight())) + rowGap;
            const bool trackRow = album && sec.title == "Morceaux"_t;
            lines.push_back ({ { 0, y, width, h }, trackRow ? (int) i : -1 });
            y += h;
        }
        y += 14;
    }
    return y;
}

void DetailsSheet::Body::paint (juce::Graphics& g)
{
    const auto& t = sheet.ctx.theme();
    const auto font = style::font (13.0f);
    size_t li = 0;
    int y = 6;
    for (auto& sec : sheet.sections)
    {
        g.setColour (t.accent());
        g.setFont (style::font (12.0f, true));
        g.drawText (sec.title.toUpperCase(), pad, y, getWidth() - 2 * pad, 18, juce::Justification::centredLeft);
        y += 18;
        g.setColour (t.border());
        g.fillRect (pad, y + 3, getWidth() - 2 * pad, 1);
        y += 8;
        for (auto& r : sec.rows)
        {
            if (li >= lines.size()) return;
            const auto a = lines[li++].area;
            g.setColour (t.textDim());
            g.setFont (font);
            g.drawText (r.label, pad, a.getY(), labelW - 12, 18, juce::Justification::topLeft, true);
            layoutText (r.value, (float) (getWidth() - labelW - 2 * pad), font, t.textCol())
                .draw (g, juce::Rectangle<float> ((float) (pad + labelW), (float) a.getY(), (float) (getWidth() - labelW - 2 * pad), (float) a.getHeight()));
            y = a.getBottom();
        }
        y += 14;
    }
}

void DetailsSheet::Body::mouseUp (const juce::MouseEvent& e)
{
    for (auto& l : lines)
        if (l.track >= 0 && l.area.contains (e.getPosition()) && l.track < (int) sheet.tracks.size())
        {
            const auto track = sheet.tracks[(size_t) l.track];
            sheet.open ({ track }, {});                         // that track's own sheet
            return;
        }
}

} // namespace nx
