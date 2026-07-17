/*
    GMCA — Tracker clients: HTTP (BEP-3 + BEP-23 compact) and UDP (BEP-15).

    Peer discovery for the PoC is trackers + PEX only (DHT is deferred — it is the
    documented crash source on nxTransmission, and Torrentio already supplies
    trackers). Multitracker (BEP-12) is handled by the engine iterating the tiers.

    Announces block (DNS + round-trips), so the engine calls these from a
    dedicated announcer thread, never from the peer event loop. HTTP goes through
    libcurl; UDP goes through the socket shim.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/types.hpp"

namespace torrent {

struct AnnounceContext {
    InfoHash infoHash{};
    std::string peerId;    // 20 bytes
    uint16_t port = 6881;  // advertised port (we do not actually accept incoming in the PoC)
    int64_t downloaded = 0;
    int64_t left = 0;
    int64_t uploaded = 0;
    int numWant = 80;
    int event = 2;  // 0 none, 1 completed, 2 started, 3 stopped
};

struct AnnounceResult {
    bool ok = false;
    std::string error;
    int intervalSec = 1800;
    std::vector<PeerAddr> peers;
};

/// Announce to a single tracker URL (http/https/udp), dispatched by scheme.
AnnounceResult announce(const std::string& trackerUrl, const AnnounceContext& ctx, int timeoutMs = 15000);

}  // namespace torrent
