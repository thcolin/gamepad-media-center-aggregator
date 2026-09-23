# Moteur de streaming torrent on-device — spec d'implémentation

> Branche : `feat/stremio-torrent-support`. Ce document décrit l'**architecture
> réelle** du moteur torrent minimal (streaming-first, éphémère) livré comme
> squelette compilable + PoC desktop fonctionnel. Il fait suite à l'étude de
> faisabilité `TORRENT_STREAMING.md` (source de vérité des décisions) et suit le
> style des design docs du repo (`MULTI_BACKEND.md`).
>
> **Décisions déjà arrêtées (non re-débattues)** : créer un moteur, pas forker
> libtorrent/libtransmission ; zéro Boost ; instance éphémère par lecture, buffer
> RAM jetable ; modèle `moteur → HTTP local 127.0.0.1:PORT → mpv`. Voir
> `TORRENT_STREAMING.md` §1, §6.

---

## 1. Vue d'ensemble

```
   magnet / infoHash / .torrent
              │
              ▼
   ┌──────────────────────────────────────────────────────────┐
   │  TorrentEngine  (torrent/engine.hpp — 1 instance/lecture)  │
   │                                                            │
   │  [announcer thread] ── trackers HTTP/UDP (BEP-3/12/15) ──┐ │
   │                                          + PEX (BEP-11)  │ │
   │                                                          ▼ │
   │  [engine loop thread] ── select() non-bloquant ── peers TCP│
   │      • BEP-9 ut_metadata (metadata depuis infoHash)        │
   │      • bitfield/have, choke/unchoke, request/piece         │
   │      • PiecePicker séquentiel + deadline ─┐                │
   │  [web-seed thread] ── BEP-19 HTTP Range ──┤                │
   │                                           ▼                │
   │                          PieceStore (RAM, SHA-1, jetable)  │
   │                                           │                │
   │  [http server thread(s)] ── 127.0.0.1:PORT, Range 206 ◀────┘
   └──────────────────────────────────────────────────────────┘
              │  http://127.0.0.1:PORT/<fichier>
              ▼
   mpv  (MPVCore::setUrl — inchangé)
```

Le player n'a **rien à changer** : le moteur expose une URL HTTP locale que mpv
ouvre comme n'importe quelle source (`TORRENT_STREAMING.md` §2). L'intégration
future câble cette URL dans `resolvePlayback`.

---

## 2. Disposition des fichiers

Headers `app/include/torrent/`, sources `app/src/torrent/`, PoC + CMake `torrent/`.
Le moteur est **autonome** (aucune dépendance borealis/nlohmann) pour être
liftable sur les toolchains console derrière le shim socket.

| Module | Header / Source | Rôle | État |
|---|---|---|---|
| Bencode | `bencode.{hpp,cpp}` | décodeur/encodeur BEP-3 (spans octet pour extraire l'info dict brut) | ✅ prouvé |
| SHA-1 | `sha1.{hpp,cpp}` | abstraction ; CommonCrypto (Apple) / OpenSSL / **mbedtls (seam console)** / fallback intégré | ✅ prouvé (CommonCrypto) |
| Log | `log.{hpp,cpp}` | hook de log (stderr par défaut ; branché sur `brls::Logger` à l'intégration) | ✅ |
| Util | `util.{hpp,cpp}` | horloge monotone, aléa, peer id | ✅ |
| Socket shim | `socket.{hpp,cpp}` | TCP non-bloquant + UDP + `select()`, **impl POSIX** ; seams libnx/SceNet/openorbis | ✅ POSIX / 🟡 seams console |
| Transport | `transport.{hpp,cpp}` | abstraction porteur : `TcpTransport` (TCP) ou µTP, sous `PeerConnection` | ✅ prouvé |
| µTP | `utp.{hpp,cpp}` + `app/vendor/libutp/` | BEP-29 (libutp vendoré, MIT) : 1 socket UDP partagée, callbacks → shim | ✅ prouvé (byte-for-byte via µTP + MSE-sur-µTP) |
| Metadata | `metadata.{hpp,cpp}` | parse magnet/hex/base32, .torrent, info dict (BEP-9), `url-list` (BEP-19), `announce-list` (BEP-12) | ✅ prouvé |
| HTTP client | `http_client.{hpp,cpp}` | libcurl : trackers HTTP + web seeds | ✅ prouvé |
| Storage | `storage.{hpp,cpp}` | pièces en RAM, vérif SHA-1, lecture bloquante pour HTTP, éviction (fenêtre glissante) | ✅ prouvé (éviction non stressée) |
| Picker | `piece_picker.{hpp,cpp}` | séquentiel + fenêtre read-ahead + deadline + endgame | ✅ prouvé |
| Peer | `peer.{hpp,cpp}` | wire protocol BEP-3, extension BEP-10, ut_metadata BEP-9, PEX BEP-11 (leech-only) | ✅ prouvé |
| Tracker | `tracker.{hpp,cpp}` | HTTP (BEP-3/23 compact) + UDP (BEP-15), dispatch par schéma | ✅ HTTP prouvé / 🟡 UDP non prouvé (pas de tracker UDP sous la main) |
| HTTP server | `http_server.{hpp,cpp}` | 127.0.0.1:PORT, 1 fichier, GET/HEAD, Range 206, bloque sur pièces manquantes | ✅ prouvé |
| Engine | `engine.{hpp,cpp}` | orchestration, threads, `PeerHost` | ✅ prouvé |
| PoC | `torrent/poc.cpp` | driver CLI desktop | ✅ prouvé |

---

## 3. Threads & synchronisation

Une instance = 3 threads moteur + le(s) thread(s) du serveur HTTP.

- **engine loop** (`engineLoop`) : boucle `select()` non-bloquante ; possède
  `peers_`, la machine à états de chaque peer, l'assemblage metadata, le picker.
  Aucun autre thread ne touche `peers_`. C'est le seul thread « chaud ».
- **announcer** (`announcerLoop`) : annonces trackers **bloquantes** (DNS +
  round-trips) hors de la boucle ; pousse les peers découverts dans
  `pendingPeers_` (sous `peersMutex_`), re-annonce selon l'intervalle (plafonné).
- **web-seed** (`webSeedLoop`) : télécharge des pièces via HTTP Range (BEP-19),
  fallback swarm froid. Torrents mono-fichier seulement pour l'instant.
- **http server** (`HttpServer`) : `accept()` bloquant + 1 thread détaché par
  connexion ; lit via `PieceStore::readFile` (bloque tant que la pièce manque).

Verrous : `PieceStore` est thread-safe (un mutex + CV interne). Le `PiecePicker`
n'est pas thread-safe ; il est protégé par `pickerMutex_` (partagé engine loop ↔
web-seed). `metadataReady_` est atomique (barrière : lu par le thread appelant
`open()` et par le serveur HTTP ; publié en dernier par `startDataStage`).

**Teardown éphémère** (`close()`) : `running_=false`, `store_->stop()` débloque le
serveur HTTP, join des 3 threads, arrêt du serveur, destruction du buffer RAM.
Rien sur disque → veille/sortie ne peuvent pas corrompre d'état (contraste
nxTransmission, `TORRENT_STREAMING.md` §7-8).

---

## 4. Machine à états peer (`PeerConnection`)

```
Idle ──startConnect()──▶ Connecting ──checkConnected==1──▶ HandshakeWait
                                                               │ (reçoit le handshake pair,
                                                               │  infohash vérifié)
                                                               ▼
                                                            Ready ──(erreur/EOF/timeout)──▶ Closed
```

- **Connecting** : `connect()` non-bloquant ; le handshake 68 octets est déjà en
  file d'envoi, flushé dès la connexion établie (writable). Timeout 8 s.
- **HandshakeWait → Ready** : sur réception du handshake pair, on vérifie
  `pstrlen==19` et l'infohash (drop si mismatch), on lit le bit d'extension
  (reserved[5] & 0x10). Si extensions supportées → envoi du handshake étendu
  BEP-10 (m = {ut_metadata:1, ut_pex:2}). Puis `interested`.
- **Ready** : traitement des messages length-prefixés (choke/unchoke/have/
  bitfield/piece/extended). Keep-alive toutes les 110 s.

Détails :
- **Leech-only** : les messages `request` entrants sont ignorés (on n'upload
  jamais de données) — posture éphémère à faible empreinte voulue par le projet.
- **bitfield/have avant metadata** : bufferisés (`pendingBitfield_`,
  `pendingHaves_`) puis appliqués quand `onMetadataReady(numPieces)` dimensionne
  la carte `peerHas_`.
- **Extension BEP-10** : le handshake étendu du pair fournit ses ids
  `ut_metadata`/`ut_pex` (utilisés pour lui **envoyer**) et `metadata_size`.
  Les messages entrants utilisent **nos** ids (1 = ut_metadata, 2 = ut_pex).
- **ut_metadata (BEP-9)** : dict bencodé + (pour `msg_type=1`) les octets bruts de
  la pièce metadata appendés après le dict — la frontière est donnée par le span
  `end` du décodeur bencode.
- **PEX (BEP-11)** : clé `added` = peers compacts IPv4 (6 octets) → nouveaux peers.

---

## 4bis. Transport TCP / µTP (BEP-29) — `transport.hpp`, `utp.hpp`

Le `PeerConnection` parle le protocole wire au-dessus d'un **flux d'octets ordonné
et fiable** ; ce flux est abstrait par `PeerTransport` (transport-agnostique). Deux
porteurs :

- **`TcpTransport`** : fine enveloppe du shim TCP non-bloquant (chemin d'origine,
  inchangé — 1 fd/pair, poll par la boucle).
- **µTP (`UtpManager` + transport µTP)** : LEDBAT sur UDP via **libutp vendoré**
  (`app/vendor/libutp/`, MIT). **Une seule socket UDP** partagée multiplexe tous les
  pairs µTP (libutp les distingue par (adresse, connection id)). libutp est piloté
  par callbacks (pas par poll) : la boucle ajoute le fd UDP partagé à son `select()`,
  et sur lecture `serviceReadable()` draine les datagrammes dans `utp_process_udp`.
  libutp rappelle alors : `SENDTO` → on émet un datagramme via le shim UDP ;
  `ON_READ` → octets applicatifs vers le transport du pair ; `ON_STATE_CHANGE` /
  `ON_ERROR` → connect/writable/eof/erreur. `checkTimeouts()` pompe les timers
  (retransmission, LEDBAT) toutes les ~500 ms.

**MSE au-dessus de µTP** : le transport n'étant qu'un tuyau d'octets, le handshake
MSE/PE + RC4 fonctionne **verbatim** sur µTP comme sur TCP (prouvé, cf. §11).

**Politique de connexion** (`EngineConfig::enableTcp` / `enableUtp`, défaut les deux) :
un pair frais est composé en TCP d'abord ; un pair qui **meurt avant son handshake
BitTorrent** est re-tenté sur l'échelle de repli **MSE→plaintext (même porteur) puis
TCP→µTP** (chaque combo au plus une fois par endpoint). But : élargir le pool de
pairs joignables (derrière NAT/CGNAT, ISPs qui throttlent le BT-sur-TCP). Réglages :
`enableTcp` seul = TCP only (comportement pré-µTP) ; `enableUtp` seul = µTP only
(test déterministe). **Portabilité** : UDP via le shim (POSIX/libnx/openorbis/SceNet) ;
libutp est du transport pur (ni TLS ni Boost), compile sous devkitA64 et vitasdk
(un shim `IN6_IS_ADDR_V4MAPPED` force-inclus côté Vita).

**Modèle éphémère** : `close()` détruit les pairs (→ `utp_close`, userdata détaché
pour éviter tout use-after-free) **avant** le contexte libutp (`utp_destroy` + socket
UDP fermée), une fois le thread moteur joint (libutp mono-thread).

---

## 5. Picker séquentiel + deadline (`PiecePicker`) — LE cœur du streaming

Un client de **download** vise le taux de complétion (rarest-first). Un moteur de
**streaming** doit alimenter le décodeur depuis la tête de lecture. Algo :

1. **Séquentiel depuis le playhead.** `pickForPeer` parcourt les pièces à partir
   de `store.playheadPiece()` en ordre **croissant** et choisit la plus basse que
   le pair possède et qui a des blocs manquants → les octets dont mpv a besoin
   ensuite arrivent en premier.
2. **Fenêtre read-ahead** `[playhead, playhead + readAheadPieces]` (défaut 24).
   Le prefetch agressif est **confiné** à cette fenêtre : au-delà, on n'émet plus
   de requêtes (borne le buffer RAM = fenêtre glissante). Phase 3 (catch-up) ne
   sert un pair que si la fenêtre n'a rien à lui donner (post-seek, pièces de
   queue), toujours plus-basse-d'abord.
3. **Deadline par bloc.** `tick()` ré-arme un bloc en vol trop vieux (re-requête,
   éventuellement à un autre pair). Le délai est **plus court près du playhead**
   (`deadlineMsForPiece` : 3 s à ≤2 pièces, 6 s à ≤8, 12 s sinon).
4. **Endgame.** Quand la fenêtre ne peut être satisfaite sans doublon, la même
   pièce peut être demandée à plusieurs pairs (jamais deux fois au même) pour
   battre un pair lent.

Pipelining : jusqu'à `blockPipelineDepth` (défaut 16) blocs de 16 KiB en vol par
pair (`scheduleBlockRequests`).

Le web-seed emprunte le même ordre via `pickPieceForWebSeed` (réclame une pièce
entière, sans doublonner un bloc déjà en vol côté pairs).

---

## 6. Serveur HTTP local (`HttpServer`)

- Bind **127.0.0.1** uniquement (port éphémère par défaut, ou imposé).
- `GET`/`HEAD`, un seul fichier (celui épinglé par `fileIdx`).
- **Range** : `Range: bytes=start-end` → `206 Partial Content` +
  `Content-Range: bytes start-end/total`, `Accept-Ranges: bytes`. Sans Range →
  `200` avec `Content-Length` total. `416` si plage invalide.
- Le corps est streamé par chunks de 256 KiB via `PieceStore::readFile`, qui
  **bloque sur une CV tant que la pièce couvrante n'est pas dispo** — c'est le
  point de stall du streaming. L'offset demandé est poussé comme **playhead**, et
  ré-avancé au fil de la lecture → le picker suit la position de lecture (seek).
- Sockets serveur (`bind`/`listen`/`accept`) **bloquantes BSD** : c'est le seul
  endroit nécessitant une socket d'écoute (seam console documenté, cf. §8).

---

## 7. Interface `TorrentEngine` (publique)

Conforme à l'esquisse `TORRENT_STREAMING.md` §6 :

```cpp
TorrentEngine engine(EngineConfig{});
std::string url = engine.open("magnet:?xt=urn:btih:…", fileIdx);  // ou openTorrentFile(raw)
// url = "http://127.0.0.1:PORT/<fichier>"  →  MPVCore::setUrl(url, …)
engine.setPlayhead(byteOffset);   // biaise le picker (aussi piloté par le serveur HTTP)
Stats s = engine.stats();         // peers, débit, pièces, octets contigus (UI)
engine.close();                   // teardown total (arrêt lecture / veille)
```

Extras : `addPeer(host,port)` (test déterministe / ajout direct), `addTracker`,
`addWebSeed`, `waitForMetadata`, `waitForContiguous` (prebuffer).

`EngineConfig` (extrait, `types.hpp`) : `maxPeers` (console 8-16),
`blockPipelineDepth`, `readAheadPieces`, `ramBudgetBytes` (console 32-64 MiB),
`enableWebSeed`, `enablePex`, `httpPort`.

---

## 8. Plan de portage console (les shims)

Trois abstractions concentrent toute la dépendance plateforme. Le PoC n'implémente
que POSIX ; chaque divergence est un **seam documenté** (headers + TODO).

1. **Socket** (`socket.hpp/.cpp`) — le plus gros seam.
   - `globalInit()` : point de bring-up réseau. **Switch** : `socketInitialize()`
     avec un `SocketInitConfig` accordé (`num_bsd_sessions`, `sb_efficiency`,
     transfer memory — nxTransmission augmente ces valeurs sinon le débit
     plafonne). **Vita** : `sceNetInit(&pool)` + `sceNetCtlInit`. **PS4** :
     `sceNetInit`/openorbis.
   - TCP/UDP/`poll` : **Switch (libnx `bsd`) et PS4 (openorbis) sont des sockets
     BSD** → réutilisent quasi tel quel le code POSIX. **Vita (SceNet)** est
     l'unique vraie réécriture (`sceNetSocket`/`sceNetConnect`/`sceNetRecv`/
     `sceNetSelect`, pool mémoire) ; chaque fonction a son jumeau SceNet.
   - Serveur HTTP : `bind`/`listen`/`accept` bloquants BSD → OK Switch/PS4 ;
     Vita = `sceNetListen`/`sceNetAccept`.
2. **SHA-1** (`sha1.hpp/.cpp`) : `-DTORRENT_SHA1_MBEDTLS` bascule sur **mbedtls**,
   déjà linké via curl sur les 3 consoles (`TORRENT_STREAMING.md` §6). Aucun call
   site à changer.
3. **HTTP** (`http_client.cpp`) : libcurl, présent partout (switch-curl / vita
   curl / openorbis / desktop).

Ordre recommandé (`TORRENT_STREAMING.md` §7) : **Switch d'abord** (RAM via title
takeover + HEVC OK, sockets prouvés par nxTransmission), puis Vita (réservé
H.264 ≤720p bien-seedé, réutiliser le filtre 4K `stremio/backend.cpp:222-232` +
`qualityRankVita` `stremio/types.hpp:547`), puis PS4.

---

## 9. Points d'intégration app (à câbler plus tard — hors périmètre PoC)

Le moteur n'est **pas encore** branché dans l'app (cible séparée, gate CMake OFF).
Le câblage futur :

- `app/include/api/stremio/types.hpp` `streamToMedia` (≈L629) : pour une source
  `infoHash`, au lieu de laisser `SourceKind::Torrent` non jouable (`:651`),
  renseigner `parts[0].key` avec l'URL locale renvoyée par le moteur (derrière une
  capacité/flag plateforme). `Media::playable()` (`media/types.hpp:206`) devient
  alors vrai.
- `app/src/api/stremio/backend.cpp` `resolvePlayback` (≈L896) : instancier/piloter
  un `TorrentEngine` par lecture, `open(infoHash, fileIdx)` → renvoyer
  `PlaybackSource{url,…}`. `close()` à l'arrêt/reload (comme `stopTranscode`).
- `app/src/activity/player_view.cpp` `MPVCore::setUrl` (≈L323) : inchangé (reçoit
  l'URL locale). Ajouter un état « buffering » + indicateur peers/débit depuis
  `Stats` (`TORRENT_STREAMING.md` §7-8).

---

## 10. Gate de build (ne casse rien)

- `CMakeLists.txt` : `option(ENABLE_TORRENT … OFF)`. Les sources
  `app/src/torrent/**` sont **exclues inconditionnellement** de la GLOB de l'app
  (`list(FILTER MAIN_SRC EXCLUDE REGEX "/app/src/torrent/")`) → **aucune** des
  cibles Switch/Vita/PS4/desktop n'est affectée, option ON ou OFF (vérifié :
  108 → 95 `.cpp`, zéro fuite).
- `torrent/CMakeLists.txt` : cible **séparée** (`torrent_engine` statique +
  `torrent_poc`). Configurable **standalone** *ou* via `add_subdirectory(torrent)`
  quand `ENABLE_TORRENT=ON` (vérifié : configure OK dans les deux modes).

---

## 11. État d'implémentation — prouvé vs stub (honnête)

### Prouvé (testé sur macOS, cf. §12)

| Capacité | Preuve |
|---|---|
| Parse magnet/infoHash/.torrent, info dict, `url-list`, `announce-list` | Tests A/B/Ubuntu chargent et parsent |
| **BEP-9 ut_metadata** (metadata depuis infoHash seul) | Test B : « metadata size 191 bytes → verified » depuis un magnet nu |
| Handshake + BEP-10 extended + bitfield + unchoke + request/piece | Test B : 6 pièces reçues d'un vrai pair transmission |
| **Picker séquentiel** | Tests A/B/Ubuntu : octets contigus depuis la tête croissants (0→N) |
| **Vérif SHA-1 des pièces** | md5 servi == source (A/B) ; SHA-1 pièce 0 == hash torrent Ubuntu (`f228faef…`) |
| **Serveur HTTP + Range 206** | `curl -r` → 206 + `Content-Range` ; octets **byte-for-byte == Canonical** |
| Lecture player | `ffprobe` ouvre l'URL locale (A : h264 320×240 + aac 15 s ; B : format+durée) |
| **Tracker HTTP/HTTPS (BEP-3/23)** | Ubuntu : `torrent.ubuntu.com/announce` → 50 peers |
| **Web seed BEP-19** (Range 206 **et** fallback 200 fichier entier) | Test A (200 non-Range) + Ubuntu CDN (206 Range, 42 MiB @ 1,5 MiB/s) |
| Multitracker BEP-12 | tiers itérés (Ubuntu tier #1/#2) |
| Teardown éphémère | `close()` join propre, aucun fichier écrit |

### Non prouvé / stub / hors périmètre (marqués `// TODO`)

| Élément | Statut |
|---|---|
| **Tracker UDP (BEP-15)** | Implémenté, **non prouvé** (pas de tracker UDP joignable sous la main ; Ubuntu = HTTPS) |
| **PEX (BEP-11)** | Parsing implémenté, non déclenché dans les tests (peu de pairs) |
| **Web seed multi-fichier** | Mono-fichier seulement (`meta_.files.size()==1`) ; mapping pièce↔fichiers = TODO |
| **Éviction RAM** | Implémentée, non stressée (fichiers de test < budget) |
| **Shims console** (libnx/SceNet/openorbis) | Seams documentés, **non codés** (POSIX only) |
| **µTP (BEP-29)** | ✅ Implémenté (libutp vendoré, `app/vendor/libutp/`) derrière une abstraction de transport (`transport.hpp`) ; **prouvé** : download byte-for-byte via µTP contre `transmission-cli` (transport=µTP loggé + md5), MSE-sur-µTP (transmission `-er`), recompiles Switch/Vita. Voir §4bis. |
| **DHT (BEP-5)** | Hors périmètre PoC ; réutiliser jech/dht plus tard |

### Limite rencontrée (honnête)

Connectivité P2P **internet intermittente** dans cet environnement : un run de 35 s
a connecté 1 pair réel (3 pièces vérifiées), un run de 75 s en a connecté 0 — le
tracker (HTTPS) répond toujours, mais la joignabilité des pairs TCP varie
(sandbox/réseau + absence de MSE/µTP qui rétrécit le pool). C'est exactement le
risque « santé du swarm » de `TORRENT_STREAMING.md` §8. Le chemin P2P reste
**prouvé de bout en bout** par le test déterministe (seeder transmission local) et
le chemin web-seed 206 par le CDN Canonical.

---

## 12. Lancer le PoC

```bash
# build standalone (sans borealis)
cmake -B build-torrent torrent -DCMAKE_BUILD_TYPE=Release
cmake --build build-torrent -j
POC=./build-torrent/torrent_poc

# 1) Test déterministe web seed (BEP-19) — aucun pair requis
transmission-create -s 32 -w "http://127.0.0.1:8000/sample.mp4" -o sample.torrent sample.mp4
python3 -m http.server 8000 --bind 127.0.0.1 &          # sert le fichier
$POC sample.torrent --prebuffer 0 --run-seconds 8       # → READY url=http://127.0.0.1:PORT/sample.mp4
curl -r 0-99 "$URL" -o head.bin                         # 206 Partial Content
ffprobe "$URL"                                          # lit la vidéo

# 2) Test déterministe P2P + metadata-depuis-infoHash (BEP-9) — seeder local
transmission-cli sample.torrent -w . -p 51413 -et -M &  # seed en TCP, chiffrement toléré
$POC "magnet:?xt=urn:btih:<INFOHASH>" --peer 127.0.0.1:51413 --no-webseed --run-seconds 15

# 3) Test internet (gros swarm TCP + tracker Canonical)
curl -O https://releases.ubuntu.com/22.04/ubuntu-22.04.5-desktop-amd64.iso.torrent
$POC ubuntu-22.04.5-desktop-amd64.iso.torrent --run-seconds 60 --max-peers 80
#   robustesse cold-swarm : ajouter le WebSeed CDN
$POC ubuntu…​.torrent --webseed https://releases.ubuntu.com/22.04/ubuntu-22.04.5-desktop-amd64.iso
```

Le PoC imprime `READY url=…` sur stdout puis une ligne de stats/s sur stderr
(`peers | pièces | dl | débit | tête | webseeds`). `--help` : voir `torrent/poc.cpp`.

Depuis le root : `cmake -B build -DPLATFORM_DESKTOP=ON -DENABLE_TORRENT=ON` bâtit
aussi la cible `torrent_poc` (l'app reste inchangée).

---

## 13. Prochaines étapes (par rentabilité)

1. **Portage Switch** (le plus rentable) : implémenter le seam socket libnx (`bsd`,
   `SocketInitConfig` accordé), `-DTORRENT_SHA1_MBEDTLS`, `bind/listen/accept` du
   serveur HTTP. Lancer en application mode (forwarder NSP). RAM/HEVC OK.
2. **Intégration app** (indépendante du portage) : câbler `resolvePlayback` /
   `streamToMedia` (§9), état buffering + indicateur peers/débit, capacité/flag
   plateforme.
3. **Robustesse** : **MSE/PE** (élargit fortement le pool de pairs réels — priorité
   après Switch), web seed **multi-fichier**, stress de l'éviction RAM, tracker UDP
   validé sur un vrai tracker.
4. **Vita / PS4** : seam SceNet (Vita, vraie réécriture) / openorbis (PS4). Vita
   cantonnée au H.264 ≤720p bien-seedé.
5. **DHT** (BEP-5) : réutiliser jech/dht **seulement si** nécessaire (source de
   crash sur nxTransmission ; Torrentio fournit déjà des trackers).
```
