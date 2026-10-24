# Torrent streaming engine

A small BitTorrent engine that streams one file of a torrent to mpv, in-process,
with no external streaming server. Sources are in `app/include/torrent/` and
`app/src/torrent/`; the build and the `torrent_poc` tool are in `torrent/`.

## What it does

The engine takes a magnet URI, a bare infohash (hex-40 or base32-32), or the raw
bytes of a `.torrent` file. With only an infohash, it fetches the metadata from
peers. It downloads pieces in order from the playhead (the byte offset mpv is
reading), checks each one against its
SHA-1, and keeps them in a RAM buffer. It serves the selected file on
`http://127.0.0.1:PORT/<file>` with `Range` support, so mpv opens it like any other
HTTP source. `close()` tears down threads, sockets and buffer. One instance serves
one playback.

## What it does not do

- It never uploads piece data. Incoming `request` messages are ignored
  (`app/src/torrent/peer.cpp`, `case kMsgRequest`). The engine is leech-only.
- It does not accept incoming peer connections. There is no peer listen socket; it
  always dials out.
- It still talks to the swarm. Trackers, peers and DHT nodes see the device IP, and
  the DHT UDP socket answers queries from other nodes. Leech-only is not anonymous.
- It has no disk cache and no resume. A backward seek past the RAM buffer
  downloads the pieces again.
- It serves a single file per instance.

## App integration

The app does not link the engine yet. Stremio `infoHash` streams are still
classified as `SourceKind::Torrent` and are not playable
(`app/include/api/stremio/types.hpp`, `app/include/api/media/types.hpp`).

`app/src/torrent/session.cpp` holds `torrent::EngineSession`, a singleton that
builds a magnet from the infohash and the addon `tracker:` hints and keeps one
engine open at a time. No target compiles it today: the app GLOB excludes it and
`torrent/CMakeLists.txt` does not list it.

## Architecture

```
TorrentEngine (one per playback)
  announcer thread    trackers HTTP/UDP, DHT bootstrap DNS
  engine loop thread  select() over peers, uTP and DHT sockets; metadata, picker
  web seed thread     BEP-19 HTTP Range requests
        |
        v
PieceStore (RAM, SHA-1 checked)  ->  HttpServer 127.0.0.1:PORT  ->  mpv
```

| Module | Role |
|---|---|
| `bencode` | Bencode decoder and encoder. Keeps byte spans so the raw info dict can be hashed. |
| `sha1` | SHA-1 dispatch: OpenSSL, mbedtls, CommonCrypto on Apple, or a bundled implementation. |
| `crypto`, `mse` | RC4 and the 768-bit Diffie-Hellman group for MSE/PE. Initiator side only. |
| `socket` | Non-blocking TCP, UDP and `select()` shim. One backend per platform. |
| `transport`, `utp` | `PeerTransport` byte stream over TCP or uTP (vendored libutp). |
| `metadata` | Magnet, `.torrent`, info dict, `announce-list` and `url-list` parsing. |
| `http_client` | libcurl wrapper for HTTP trackers and web seeds. |
| `tracker` | HTTP and UDP announces. |
| `dht` | `DhtManager` on top of vendored jech/dht. |
| `peer` | `PeerConnection`: handshake, wire messages, extension protocol, metadata, PEX. |
| `piece_picker` | Sequential picker with read-ahead window, deadlines and endgame. |
| `storage` | `PieceStore`: pieces in RAM, verification, blocking reads, eviction. |
| `http_server` | Local HTTP server for mpv. |
| `engine` | `TorrentEngine`: owns the threads and wires the modules together. |
| `log`, `util` | Log callback (stderr by default), clock, random, peer id. |

`TorrentEngine` exposes `open()`, `openTorrentFile()`, `setPlayhead()`, `stats()`
and `close()`. Tuning goes through `EngineConfig` in `types.hpp`.

### Local HTTP server

The server binds `127.0.0.1` only, on an ephemeral port unless `httpPort` is set. It
answers `GET` and `HEAD`, and `405` to anything else. A `Range: bytes=start-end`
request gets `206 Partial Content` with `Content-Range`. A request without `Range`
gets `200` with the full `Content-Length`. An invalid range gets `416`.

The body is sent in 256 KiB chunks read through `PieceStore::readFile`. That call
blocks on a condition variable until the covering piece is verified. Each read
moves the playhead, so a seek in mpv moves the picker with it.

### Piece picking near the playhead

`PiecePicker::pickForPeer` works in three passes:

1. Missing blocks the peer has, from the playhead piece up to
   `playhead + readAheadPieces` (24 by default), lowest index first.
2. If that is not enough, the same read-ahead window again with duplicate requests allowed
   (endgame). A block is never asked twice from the same peer.
3. If the peer has nothing in the read-ahead window, pieces after it, then pieces
   before the playhead.

`tick()` drops in-flight blocks older than their deadline so they can be asked
again: 3 s within 2 pieces of the playhead, 6 s within 8, 12 s beyond. Each peer
keeps up to `blockPipelineDepth` (16) blocks of 16 KiB in flight. The web seed
thread claims whole pieces in the same order through `pickPieceForWebSeed`.

### RAM buffer

`PieceStore` holds verified pieces in memory up to `ramBudgetBytes` (128 MiB by
default). Past the budget it evicts verified pieces more than 16 pieces behind the
playhead, lowest index first. An evicted piece is marked missing and is fetched
again if a reader needs it.

## Protocols

| Protocol | Where |
|---|---|
| BEP-3 wire protocol and metainfo | `peer.cpp`, `metadata.cpp` |
| BEP-3 / BEP-23 HTTP tracker, compact peers | `tracker.cpp` |
| BEP-15 UDP tracker | `tracker.cpp` |
| BEP-12 multitracker (`announce-list`) | `metadata.cpp` |
| BEP-10 extension protocol | `peer.cpp` |
| BEP-9 `ut_metadata` | `peer.cpp`, `engine.cpp` |
| BEP-11 PEX, received and sent, IPv4 compact | `peer.cpp`, `engine.cpp` |
| BEP-5 mainline DHT, IPv4 only | `dht.cpp`, vendored jech/dht 0.28 |
| BEP-19 web seeds (`url-list`, magnet `ws=`) | `engine.cpp` |
| BEP-29 uTP | `utp.cpp`, vendored libutp |
| MSE/PE (Vuze spec, no BEP), RC4 | `mse.cpp`, `crypto.cpp` |

## Peer discovery

- Trackers from the magnet `tr=` params, the `.torrent` `announce` and
  `announce-list`, and `addTracker()`. The announcer re-announces at the tracker
  interval, capped at 120 s between rounds.
- DHT, bootstrapped from `router.bittorrent.com`, `dht.transmissionbt.com` and
  `router.utorrent.com`. The search uses port 0, so the engine never announces itself.
- PEX from connected peers, and peers added with `addPeer()` or `--peer`.

A new peer is dialed over TCP with MSE preferred (`Encryption::Prefer`). A peer
that dies before its BitTorrent handshake is retried once per step: plaintext on
the same transport, then uTP. `enableTcp`, `enableUtp` and `encryption` in
`EngineConfig` change this.

## Platforms

All platform code sits in `socket.cpp`, plus `util.cpp` for randomness.

- Switch uses libnx BSD sockets, the same code as POSIX. borealis already calls
  `socketInitialize()` with `num_bsd_sessions = 12` and `sb_efficiency = 8`, so the
  engine does not own the socket lifecycle. `TORRENT_SWITCH_OWN_SOCKET` is only for
  a standalone harness. `__SWITCH__` is not a compiler builtin, so
  `torrent/CMakeLists.txt` defines it on `torrent_engine`.
- Vita uses its own SceNet backend: `sceNet*` calls, `SCE_NET_SO_NBIO`, and
  `sceNetEpoll*` in place of `select()`. The app's newlib already initializes
  SceNet; `TORRENT_VITA_OWN_SOCKET` is for a standalone harness. DHT is off by
  default on Vita (`enableDht = false` under `__vita__`) to save RAM and CPU.
- PS4 uses the OpenOrbis FreeBSD sockets through the POSIX path. The network stack
  is brought up by borealis and the app.
- Desktop uses the POSIX path. Windows is not implemented: the winsock branch of
  `socket.cpp` is a TODO and `globalInit()` returns false.

On consoles, `TORRENT_SHA1_MBEDTLS` is forced on and `TORRENT_POC` defaults to off
(on elsewhere).

## Build

The root option `ENABLE_TORRENT` (default `OFF`) only adds `torrent/` as a
subdirectory. `app/src/torrent/` is always excluded from the app sources, so the
app build is the same with the option on or off.

`torrent/CMakeLists.txt` builds three static libraries, `torrent_engine`, `utp` and
`jechdht`, and the `torrent_poc` executable. It can be configured on its own:

```bash
cmake -B build-torrent torrent -DCMAKE_BUILD_TYPE=Release
cmake --build build-torrent -j
./build-torrent/torrent_poc "magnet:?xt=urn:btih:<INFOHASH>&tr=<tracker url>" --run-seconds 60
```

From the root, `cmake -B build -DPLATFORM_DESKTOP=ON -DENABLE_TORRENT=ON` builds the
same targets next to the app.

`-DTORRENT_SHA1_OPENSSL=ON` or `-DTORRENT_SHA1_MBEDTLS=ON` pick the SHA-1 backend.
Without either, Apple uses CommonCrypto and other systems the bundled SHA-1.

`torrent_poc` prints `READY url=<local url>` on stdout, then one stats line per
second on stderr (peers, pieces, downloaded, rate, contiguous head, web seeds, DHT
nodes). Open the URL with mpv, `ffprobe` or `curl -r`. Options: `--peer host:port`,
`--tracker`, `--webseed`, `--port`, `--prebuffer <MiB>` (default 2),
`--run-seconds`, `--max-peers` (default 40), `--no-webseed`, `--dht`, `--no-dht`,
`--encryption plain|prefer|force`, `--transport tcp|utp|both`. A trailing number
selects the file index; by default the largest file is used.

## Limits and known risks

- Playback depends on the swarm. A well-seeded torrent streams, a cold one buffers
  forever.
- `ramBudgetBytes` is not a hard cap. Eviction only drops pieces behind the playhead,
  and the picker's third pass keeps downloading ahead of the read-ahead window when a peer has
  nothing else to give.
- Web seeds work for single-file torrents only (`webSeedLoop`). A server that ignores
  `Range` sends the whole file, which is held in memory before being split into
  pieces.
- Trackers receive `port=6881` (or `httpPort`), where nothing listens for peers.
- jech/dht keeps its state in globals, so only one `DhtManager` can run per process.
- The `EngineConfig` defaults target desktop (40 peers, 128 MiB). Nothing lowers
  them on consoles yet; the header comments suggest 8 to 16 peers and 32 to 64 MiB.
