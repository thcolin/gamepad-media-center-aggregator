#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/sha1.hpp"

namespace torrent {

/// SHA-1 of the bencoded info dict (BEP-3).
using InfoHash = Sha1Digest;

struct PeerAddr {
    std::string host;  // dotted IPv4 or a hostname
    uint16_t port = 0;

    bool operator==(const PeerAddr& o) const { return host == o.host && port == o.port; }
    std::string str() const { return host + ":" + std::to_string(port); }
};

/// MSE/PE policy. Prefer offers RC4 or plaintext, then reconnects in the clear once if the handshake fails.
enum class Encryption { Plaintext, Prefer, Forced };

struct EngineConfig {
    uint16_t httpPort = 0;                       // 0 = pick an ephemeral port for the local HTTP server
    int blockPipelineDepth = 16;                 // outstanding block requests per peer
    int readAheadPieces = 24;
    // The piece store lives in RAM next to mpv's own demuxer cache.
#if defined(__vita__)
    int maxPeers = 12;
    int64_t ramBudgetBytes = 32 * 1024 * 1024;
#elif defined(__SWITCH__) || defined(__PS4__)
    int maxPeers = 24;
    int64_t ramBudgetBytes = 64 * 1024 * 1024;
#else
    int maxPeers = 40;
    int64_t ramBudgetBytes = 128 * 1024 * 1024;
#endif
    bool enableWebSeed = true;                   // BEP-19
    bool enablePex = true;                       // BEP-11
    // BEP-5, off on Vita: its routing table and query traffic cost RAM and CPU the Vita lacks
#if defined(__vita__)
    bool enableDht = false;
#else
    bool enableDht = true;
#endif
    // TCP first; a peer that dies before its handshake is retried over µTP (BEP-29),
    // which reaches peers behind NAT and ISPs that throttle BitTorrent over TCP
    bool enableTcp = true;
    bool enableUtp = true;
    // many peers and ISPs require or prefer encryption
    Encryption encryption = Encryption::Prefer;
    std::string peerIdPrefix = "-GM0001-";  // Azureus-style, 8 chars
};

struct Stats {
    int peersConnected = 0;
    int peersKnown = 0;
    int64_t downloadedBytes = 0;
    double downloadRateBps = 0.0;
    int piecesTotal = 0;
    int piecesHave = 0;
    bool metadataReady = false;
    int64_t contiguousReadyBytes = 0;  // from the start of the file
    int webSeeds = 0;
    int dhtNodes = 0;  // good and dubious routing table nodes
};

}  // namespace torrent
