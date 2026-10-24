// Mainline DHT (BEP-5) over jech/dht, on its own UDP socket. jech/dht keeps its
// state in globals, so only one DhtManager can be active per process. It is not
// thread-safe: every dht_* call runs on the engine loop thread, except dht_init
// (before the loop starts) and dht_uninit (after it joins).

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "torrent/socket.hpp"
#include "torrent/types.hpp"

namespace torrent {

class DhtManager {
public:
    /// Called on the engine loop thread.
    using PeersFn = std::function<void(const std::vector<PeerAddr>&)>;

    DhtManager();
    ~DhtManager();

    DhtManager(const DhtManager&) = delete;
    DhtManager& operator=(const DhtManager&) = delete;

    /// False if UDP is unavailable or another DhtManager is already active.
    bool start(PeersFn peersFn);
    void stop();  // only once the engine loop has joined

    bool active() const { return active_; }

    net::Handle udpHandle() const { return udp_.handle(); }

    /// Blocking DNS, meant to run off the loop; periodic() pings the results.
    void resolveBootstrap();

    /// Idempotent: jech/dht merges a re-issue with the search in progress.
    void search(const InfoHash& infoHash);

    /// Call when the UDP fd is readable.
    void serviceReadable();

    void periodic(int64_t nowMs);

    /// Good + dubious nodes in the routing table.
    int nodeCount() const;

    // For the jech/dht callbacks.
    net::UdpSocket& udp() { return udp_; }
    void onValues(const uint8_t* data, size_t len);

private:
    // A null buf is a timer tick with no datagram.
    void pump(const uint8_t* buf, size_t len, const std::string& fromIp, uint16_t fromPort);

    net::UdpSocket udp_;
    bool active_ = false;
    PeersFn peersFn_;

    InfoHash searchHash_{};
    bool haveSearch_ = false;
    int64_t lastSearchMs_ = 0;
    int64_t nextPeriodicMs_ = 0;

    // Written by resolveBootstrap off the loop, read by periodic on the loop.
    std::mutex bootstrapMutex_;
    std::vector<PeerAddr> pendingBootstrap_;
    bool bootstrapDone_ = false;
};

}  // namespace torrent
