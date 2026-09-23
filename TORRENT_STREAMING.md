# Streaming torrent on-device (Stremio) — étude de faisabilité & contraintes

> Branche : `feat/stremio-torrent-support`. Ce document fige les recherches et les
> contraintes pour lire **on-device** les sources Stremio de type `infoHash` (torrent
> brut), sans passer par un débrideur ni par un serveur auto-hébergé distant. Objectif :
> le device (Switch/Vita/PS4/desktop) gère lui-même le P2P, de façon **éphémère** (le
> moteur ne vit que le temps d'une lecture).

---

## 1. Objectif & décision de périmètre

- **But** : rendre jouable une source Stremio `infoHash`-only en la streamant en P2P
  directement depuis le device, comme le fait le streaming-server de Stremio, mais
  embarqué dans l'app.
- **Écarté explicitement** :
  - *Serveur torrent auto-hébergé distant* (TorrServer / stream-server / webtor.io). Ce
    serait « un débrideur perso » ; ce n'est **pas** l'idée. Le device doit gérer le P2P.
  - *Débrideur* : déjà supporté, reste le chemin recommandé quand le device ne peut pas.
- **Modèle éphémère assumé** : le moteur démarre au lancement d'un film, et est **détruit
  à l'arrêt de lecture ou à la mise en veille**. Buffer **RAM jetable**, aucune
  persistance. Cette contrainte est délibérée : elle neutralise les fragilités connues
  (voir §7) et simplifie fortement l'implémentation.

---

## 2. L'insight d'architecture central

Toute la lecture de l'app converge vers **un seul appel** : `MPVCore::setUrl(url, extra)`
(`app/src/activity/player_view.cpp:323`), alimenté par `resolvePlayback` du backend actif
(`app/src/api/stremio/backend.cpp:896`). mpv gère nativement HTTP(S) avec en-têtes `Range`
et le seek.

**Conséquence : le player n'a rien à changer.** Il suffit qu'un moteur torrent expose un
endpoint HTTP local `http://127.0.0.1:PORT/…` et que `resolvePlayback` renvoie cette URL.
C'est exactement l'architecture de Stremio (streaming-server sur `127.0.0.1:11470` via
`enginefs`), et **les docs projet l'avaient déjà planifié** :

> `MULTI_BACKEND.md:26,233,285` — « Torrents (`infoHash`) → streaming server local
> `127.0.0.1:11470` → **desktop uniquement** » (jamais implémenté).

Rien de tel n'existe encore : **aucun serveur HTTP embarqué ni lancement de sous-processus**
dans l'app (vérifié). Sur les consoles homebrew il n'y a de toute façon pas de modèle de
processus pour lancer un binaire annexe → **tout doit être in-process**, dans le binaire de
l'app.

---

## 3. État actuel du code (point de départ)

- **Parsing** : un stream Stremio porte `infoHash` + `fileIdx` (`app/include/api/stremio/types.hpp:474-484`).
- **Classification** : un stream avec `url` est jouable (`Direct`/`Debrid`) ; un
  `infoHash`-only devient `SourceKind::Torrent` **non jouable**
  (`app/include/api/stremio/types.hpp:651`, enum `app/include/api/media/types.hpp:181-187`).
  `Media::playable()` = « a une `parts[0].key` non vide » (`media/types.hpp:206`).
- **Lecture** : `resolvePlayback` renvoie `{}` quand la source n'a pas d'URL
  (`stremio/backend.cpp:903`) → le player affiche « lecture impossible ».
- **UI** : les torrents sont comptés et affichés comme sources non jouables renvoyant vers
  un débrideur (`app/src/tab/media_movie.cpp:348-476`, i18n `torrents`/`torrents_help`,
  `resources/i18n/fr/main.json:470,473`).
- **Garde-fous Vita déjà en place** : filtre 4K avant tri (`stremio/backend.cpp:222-232`),
  et `qualityRankVita` qui rétrograde le 1080p sous le ≤720p (`stremio/types.hpp:547`).

**Ce qu'il faut ajouter** : un moteur torrent → serveur HTTP local, et faire de la ligne
`Torrent` une source jouable (renseigner `parts[0].key` avec l'URL locale, câblé dans
`streamToMedia`/`resolvePlayback`).

---

## 4. Moteurs & bibliothèques évalués

Constat majeur : **stremio-core (Rust) ne contient PAS de moteur torrent** — c'est le
`server.js` propriétaire, séparé. stremiox se contente de bundler ce binaire fermé → **non
réutilisable**. Personne ne publie un « moteur torrent portable console » clé en main.

| Brique | Langage | Streaming (pièces séquentielles) | Boost ? | Verdict pour nos cibles |
|---|---|---|---|---|
| **libtorrent-rasterbar** | C++ | ✅ natif (`set_piece_deadline`) | ❌ **oblige Boost** | Desktop/Android — **aucun portage console**, Boost = bloqueur |
| **libtransmission** | C | ❌ download-only | ✅ sans Boost | **Prouvé sur Switch** (nxTransmission) mais à réécrire pour streamer |
| **rakshasa/libtorrent** (rtorrent) | C++ | partiel | ✅ sans Boost | POSIX epoll/kqueue → portage newlib non garanti, 0 portage console |
| **jech/dht** | C pur | (brique DHT seule) | ✅ | Très portable, réutilisable **isolément** pour la DHT |
| **librqbit** | Rust | ✅ HTTP Range/seek natif | — | Embarquable en lib, cible desktop |
| **TorrServer / anacrolix** | Go | ✅ + cache RAM | — | Modèle serveur autonome (écarté : voie distante) |
| **Elementum / Torrest** (Kodi) | Go + libtorrent-go | ✅ | (via Go) | **Modèle d'archi** « engine→HTTP→player », RPi/Android |
| **peerflix / webtorrent** | Node | ✅ | — | Bon modèle, runtime Node → pas console |

Go et Node sont d'excellents **modèles d'architecture** mais leurs runtimes ne ciblent pas
libnx / vitasdk / openorbis → inutilisables tels quels sur les consoles.

---

## 5. Faisabilité par device

### Verdict de synthèse

| | Switch | PS Vita | PS4 | Desktop / RPi / Android |
|---|---|---|---|---|
| **Moteur P2P on-device** | ✅ **Viable** | 🟠 **Marginal & étroit** | 🟡 Viable mais risqué | ✅ Trivial |
| Bloqueur principal | aucun rédhibitoire | décodeur + CPU + RAM | maturité openorbis | — |

### Les 3 contraintes qui décident réellement (pas l'API socket)

**1. RAM — non bloquant sur Switch.** L'app se lance via son **forwarder NSP**
(`BUILTIN_NSP=ON` dans `scripts/build-switch.sh`, title `0500474D43410000`,
`CMakeLists.txt:64`) → *title takeover* → **~3,2 Go** (et non les 442 Mo du mode applet).
Un buffer glissant RAM de 64–128 Mo est trivial. Vita : 512 Mo LPDDR2 mais budget app
réduit et partagé avec borealis + mpv → fenêtre ~32–64 Mo, serré mais faisable en 720p.
PS4 : ~4,5 Go garantis, large marge.

**2. Sockets P2P — non bloquant sur Switch, prouvé empiriquement.** La limite libnx n'est
pas figée : `num_bsd_sessions`, `sb_efficiency` et la transfer memory sont configurables
(`SocketInitConfig`). Surtout, **nxTransmission tourne** et Transmission maintient par
défaut des dizaines à centaines de connexions peers → la Switch encaisse un swarm réel
(preuve plus fiable que le chiffre du header). Vita (SceNetPs) alloue les sockets depuis un
pool mémoire (fallback `malloc`) → borné par la RAM. PS4 (openorbis) : sockets BSD FreeBSD,
serveur TCP fonctionnel démontré.

**3. Codec — LE discriminant Switch vs Vita.**
- **Switch (Tegra X1 / NVDEC)** : décode **H.264 ET HEVC/H.265** (jusqu'à 4K60 10-bit) +
  VP9 en hardware → les torrents modernes x265 passent. ✅
- **Vita (`sceAvcdec`)** : **H.264 uniquement, pas de HEVC** ; le Cortex-A9 ne fait pas de
  HEVC software. Donc sur Vita seuls les torrents **H.264 ≤720p** sont jouables — une part
  étroite du contenu moderne (souvent x265). Ajouté au CPU faible qui doit assurer SHA-1 des
  pièces + TCP + demux + décodage **en même temps**, Vita reste « ça marche pour du
  contenu bien-seedé H.264 léger », pas une expérience générale.

### Preuves de terrain

- **nxTransmission** : Transmission 2.94 porté sur Switch (devkitA64/libnx), deps
  `switch-curl`/`switch-mbedtls`/`switch-miniupnpc`/`switch-zlib` (C, **sans Boost**) →
  prouve que le P2P + le toolchain marchent. Limites documentées : DHT crashe à la sortie,
  veille/changement réseau = broken pipe, FAT32 4 Go, corruption exFAT si crash. **C'est un
  downloader, pas un streamer.**
- **VitaTPBPlayer** : seule app « torrent » Vita — n'embarque **aucun moteur P2P**, envoie
  les magnets à Real-Debrid et télécharge en HTTP. Précédent qui confirme que le P2P natif
  sur Vita n'est pas la voie retenue par la communauté sur ce matériel.

---

## 6. Décision moteur : **créer**, pas forker

Le cœur du problème n'est pas « un client torrent » mais **un moteur de streaming** :
sélection de pièces en ordre **séquentiel + deadline** biaisée vers la tête de lecture,
servies sur un **HTTP local qui bloque tant que la pièce n'est pas disponible**.

| Voie | Verdict |
|---|---|
| **Porter libtorrent-rasterbar (+Boost)** | ❌ **Piège.** Streaming natif, mais Boost = « nightmare » non porté sur les 3 consoles (0 précédent = signal). Boost absent des portlibs devkitPro et vitasdk. Risque d'échec élevé. |
| **Forker libtransmission + greffer le streaming** | 🟡 Fallback. Réseau/peers/DHT **prouvés sur Switch**, C sans Boost, deps en portlibs — mais son picker est conçu pour du **download complet** ; le plier au streaming est de la vraie chirurgie (on hérite de l'ADN d'un downloader). |
| **Créer un moteur streaming-first minimal** | ✅ **Recommandé.** Conçu autour du streaming éphémère. Deps minimales **déjà présentes** : sockets (shim par plateforme), **mbedtls** pour SHA-1 (déjà linké via curl partout), bencode (trivial). DHT = réutiliser **jech/dht** (C, zéro dépendance). Surface réduite = portable sur les 3 consoles + desktop. |

**Pourquoi créer** : une seule cible commune (Switch/Vita/PS4/desktop) toutes sans Boost,
toutes avec seulement des sockets BSD-ish + mbedtls. Un moteur dédié, petit, derrière un
**shim socket par plateforme** (libnx `bsd` / SceNet / openorbis-BSD / POSIX), est le
meilleur ajustement long terme et le plus portable. C'est aussi un composant greenfield
bien cadré (specs BEP publiques) adapté à un échafaudage par agent dédié.

### Esquisse d'interface

```
// moteur, in-process, une instance par lecture
class TorrentEngine {
    // magnet ou infoHash + index du fichier vidéo (StreamOption.fileIdx)
    std::string open(const std::string& infoHash, int fileIdx);  // -> "http://127.0.0.1:PORT/…"
    void setPlayhead(int64_t byteOffset);   // pilote le picker séquentiel/deadline
    Stats stats();                          // peers, vitesse, % buffer  (pour l'UI)
    void close();                           // teardown total (arrêt lecture / veille)
};
```

Dépendances par plateforme (toutes disponibles) : **sockets** via shim, **mbedtls**
(SHA-1 des pièces ; openssl côté desktop), **bencode** maison, **jech/dht** optionnel.
Le serveur HTTP local est minimal (un seul client = mpv, un seul fichier, support `Range`)
→ quelques centaines de lignes, pas de nouveau portlib.

---

## 7. Cadrage MVP & plan de portage (dérisquer dans l'ordre)

1. **PoC desktop d'abord.** Pipeline complet : magnet → metadata (BEP-9 `ut_metadata`) →
   trackers (HTTP/UDP announce, BEP-12) → peers → **picker séquentiel** → HTTP local Range
   → mpv. **Trackers + PEX seulement, DHT reporté** (source de crash de nxTransmission, et
   Torrentio fournit déjà des trackers). Valide le pipeline sans toolchain console.
2. **Portage Switch.** Shim socket libnx (`bsd`) + `switch-mbedtls`. Cible la plus
   rentable : RAM (title takeover) + HEVC OK. Lancement obligatoirement en **application
   mode** (via le forwarder).
3. **Vita / PS4.** Même moteur derrière le shim. Vita : réservé au **H.264 ≤720p** bien-
   seedé (réutiliser le filtre existant `stremio/backend.cpp:222-232` + `qualityRankVita`).
   PS4 : selon appétit pour openorbis (aucun précédent torrent).

**Intégration app** (indépendante du portage) : rendre la ligne `SourceKind::Torrent`
jouable en renseignant `parts[0].key` avec l'URL locale (dans `streamToMedia` /
`resolvePlayback`), derrière une capacité/flag par plateforme ; adapter l'UI (l'actuel
`torrents_help` disparaît quand le moteur est dispo) ; ajouter un état « buffering » +
indicateur peers/vitesse.

---

## 8. Risques & points de vigilance

- **Santé du swarm (le risque hors-code).** Sans débrideur, la fluidité dépend du nombre/
  débit des peers : un torrent bien-seedé streame, un froid bufferise indéfiniment. Aucun
  choix de lib ne corrige ça (le server de Stremio a le même souci). À traiter **côté UX** :
  indicateur peers/vitesse, état buffering, préférence aux sources bien-seedées.
- **Robustesse réseau.** Le modèle éphémère (teardown à l'arrêt/veille) est ce qui rend la
  chose fiable : il évite les broken-pipe en veille et les crashs DHT à la sortie observés
  sur nxTransmission. **Ne pas** viser un moteur persistant en tâche de fond.
- **Vita** : codec H.264-only + CPU faible + RAM serrée → périmètre étroit, à documenter
  comme tel (ne pas promettre l'équivalent Switch).
- **Stockage** : buffer **RAM** jetable (pas d'écriture SD/memory-card) → évite FAT32 4 Go
  et la corruption exFAT de nxTransmission. Dimensionner la fenêtre selon la plateforme.
- **Légal / produit** : un moteur P2P embarqué fait du device un **seeder** (upload, IP
  exposée au swarm/à l'ISP). Décision de positionnement à assumer explicitement (option
  « ne pas uploader » / limiter le ratio à évaluer).
- **Sockets** : dimensionner la transfer memory libnx / le pool SceNet en amont (sinon
  débit plafonné) ; borner le nombre de peers selon la plateforme.

---

## 9. Sources

Architecture Stremio & moteurs :
- Stremio streaming-server / enginefs — https://github.com/Stremio/enginefs
- stremio-core (pas de moteur torrent) — https://github.com/Stremio/stremio-core
- stremiox (bundle le server.js propriétaire) — https://github.com/yvdjee/stremiox
- Elementum (Kodi, modèle engine→HTTP) — https://github.com/elgatito/plugin.video.elementum
- libtorrent-go — https://github.com/ElementumOrg/libtorrent-go
- TorrServer — https://github.com/YouROK/TorrServer
- librqbit — https://github.com/ikatson/rqbit

Libs torrent & deps :
- libtorrent building (Boost requis) — https://www.libtorrent.org/building.html
- libtorrent tuning/streaming — https://www.libtorrent.org/tuning.html
- libtransmission — https://github.com/transmission/transmission
- jech/dht — https://github.com/jech/dht
- Boost absent des portlibs devkitPro — https://github.com/devkitPro/pacman-packages/issues/90
- portlibs vitasdk (vdpm) — https://github.com/vitasdk/vdpm

Faisabilité console :
- nxTransmission (preuve Switch) — https://github.com/t-flo/nxTransmission
- fil GBAtemp nxTransmission — https://gbatemp.net/threads/nxtransmission-a-torrent-client-for-the-switch.555951/
- libnx sockets services — https://switchbrew.org/wiki/Sockets_services
- libnx socket.h (SocketInitConfig) — https://switchbrew.github.io/libnx/socket_8h_source.html
- nx-hbloader RAM 2.0 (title takeover) — https://gbatemp.net/threads/nx-hbloader-updated-to-version-2-0-0-now-has-full-access-to-ram.521091/
- Tegra X1 NVDEC (HEVC/H.264/VP9) — https://chipsandcheese.com/p/examining-the-nintendo-switch-tegra-x1-video-engine
- Vita videodec.h (AVC/H.264 only) — https://github.com/vitasdk/vita-headers/blob/master/include/psp2/videodec.h
- VitaTPBPlayer (offload debrid) — https://github.com/mzzvxm/VitaTPBPlayer
- openorbis PS4 toolchain — https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
