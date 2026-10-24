#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/types.hpp"

namespace torrent {

struct AnnounceContext {
    InfoHash infoHash{};
    std::string peerId;    // 20 bytes
    uint16_t port = 6881;  // advertised only: the engine accepts no incoming connection
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

/// http(s) (BEP-3, BEP-23) or udp (BEP-15). Blocks: never call from the peer loop.
AnnounceResult announce(const std::string& trackerUrl, const AnnounceContext& ctx, int timeoutMs = 15000);

}  // namespace torrent
