# Composants tiers

Aura est un logiciel propriétaire (voir `LICENSE`). Les composants ci-dessous gardent leur propre licence ;
leurs textes complets sont réunis dans `Licences.txt`, embarqué dans l'application et affiché dans
Réglages › À propos › Licences & mentions légales.

| Composant | Version | Licence | Utilisation | Inclus dans le dépôt |
|---|---|---|---|---|
| JUCE | 8.0.15 | licence commerciale JUCE | fenêtres, graphisme, périphériques audio, FLAC / MP3 / Vorbis, QuickJS, réseau | non (téléchargé par CMake) |
| FLAC (via JUCE) | — | BSD (Xiph.Org) | décodage FLAC | non (dans JUCE) |
| Ogg / Vorbis (via JUCE) | 1.3.7 | BSD (Xiph.Org) | décodage Ogg Vorbis | non (dans JUCE) |
| zlib (via JUCE) | — | zlib | décompression | non (dans JUCE) |
| libpng (via JUCE) | — | libpng | images PNG | non (dans JUCE) |
| libjpeg IJG (via JUCE) | 9 | IJG (« basé en partie sur le travail de l'Independent JPEG Group ») | images JPEG | non (dans JUCE) |
| HarfBuzz (via JUCE) | — | Old MIT | mise en forme du texte | non (dans JUCE) |
| SheenBidi (via JUCE) | — | Apache 2.0 | texte bidirectionnel | non (dans JUCE) |
| choc (via JUCE) | — | ISC | moteur JavaScript | non (dans JUCE) |
| QuickJS (via JUCE / choc) | — | MIT | plug-ins d'interface JavaScript | non (dans JUCE) |
| Monocypher | 4.0.3 | BSD 2 clauses ou CC0 (au choix) | signatures Ed25519 des clés de licence | oui, `ThirdParty/monocypher` |
| libwebp | — | BSD 3 clauses + octroi de brevets Google | pochettes WebP | oui, `ThirdParty/libwebp` (`COPYING`, `PATENTS`) |
| stb_image | 2.30 | domaine public ou MIT (au choix) | pochettes JPEG progressives / CMYK, PNG 16 bits | oui, `ThirdParty/stb` |
| Apple Lossless Audio Codec | macosforge/alac | Apache 2.0 | décodage ALAC (encodeur utilisé par le banc d'essai) | oui, `ThirdParty/alac` (ordre des octets corrigé pour arm64 / MSVC) |
| SQLite | système, ou amalgamation 3.38 | domaine public | bibliothèque | non (système, ou téléchargé sous Windows) |
| QR Code generator (Project Nayuki) | C++ | MIT | QR code d'Aura Remote | oui, `ThirdParty/qrcodegen` |
| SDK ASIO de Steinberg | 2.3.x | licence Steinberg ASIO ou GPLv3 | sortie ASIO (PCM + DSD natif), Windows | non : à fournir via `NX_ASIO_SDK_DIR` |

## Conséquences pour une distribution

- **JUCE** : un binaire vendu d'Aura suppose une licence commerciale JUCE active (Starter, Indie ou Pro,
  selon le chiffre d'affaires) au nom de l'éditeur. Sans elle, JUCE n'est utilisable que sous AGPLv3, incompatible
  avec une licence propriétaire.
- **ASIO** : un binaire Windows compilé avec le SDK ASIO suppose l'accord de licence Steinberg signé.
  ASIO est une marque de Steinberg Media Technologies GmbH.
- **Kit de plug-ins** : `Source/Ext/nxplayer_dsp.h` et les plug-ins d'exemple restent sous licence MIT pour que des
  tiers puissent écrire des plug-ins.
- **Marques** : RME, ADI-2 et SteadyClock (RME Audio AG), Merging, Anubis et RAVENNA (Merging Technologies SA),
  FiiO et KA17 (FiiO Electronics Technology Co., Ltd.), ASIO (Steinberg Media Technologies GmbH), Super Audio CD et
  Direct Stream Digital (Sony Group Corporation et Koninklijke Philips N.V.) appartiennent à leurs propriétaires.
  Aura les cite seulement pour indiquer la compatibilité de ses profils audio ; il n'est ni affilié à ces
  sociétés, ni approuvé par elles. Les profils de l'application ne reprennent pas le nom SteadyClock (« Horloge DoP
  Haute Précision »).
- **Services** : Qobuz, Tidal, Spotify et YouTube sont des marques de leurs propriétaires. Le streaming n'est
  compilé qu'avec `NX_ENABLE_STREAMING` (désactivé par défaut).

## Données en ligne

- **TheAudioDB** : aucune clé n'est fournie. Les photos et fiches d'artistes ne sont demandées que si l'utilisateur
  saisit sa propre clé API (Réglages › Bibliothèque & NAS) ; « Source : TheAudioDB.com » est alors affiché sous les
  visuels.
- **Wikipédia** : les biographies portent toujours la mention « Source : Wikipédia — Texte sous licence
  CC BY-SA 4.0 ».
- **Paroles** : aucune requête vers LRCLIB ni lyrics.ovh. Seuls les fichiers `.lrc` / `.txt` placés à côté du
  morceau et les tags de paroles (ID3 USLT / SYLT, Vorbis LYRICS, MP4 ©lyr) sont lus.

Ces données restent dans la base locale de l'utilisateur et ne sont jamais redistribuées avec l'application.
