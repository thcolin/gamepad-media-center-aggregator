/*
    GMCA — mainline DHT manager (BEP-5, on top of jech/dht).

    Why DHT: trackers can be dead or censored and PEX only spreads peers you
    already reached — a magnet with no live tracker has no cold-start otherwise.
    The mainline DHT (Kademlia) discovers peers for an infohash with no tracker at
    all, by querying a global overlay of nodes. It is the single most valuable peer
    source for trackerless magnets, and complements trackers/PEX/µTP.

    Design — mirrors UtpManager (utp.hpp) deliberately:
      - The DhtManager owns ONE libjech-dht instance and ONE dedicated UDP socket
        (torrent::net::UdpSocket), separate from the µTP carrier so KRPC and µTP are
        never multiplexed on one socket.
      - jech/dht is callback-driven, not poll-driven. The engine loop adds the DHT
        UDP fd to its select() set; when readable, serviceReadable() drains the
        datagrams into dht_periodic(). jech/dht then:
          • calls dht_sendto (our shim) to emit queries/replies,
          • fires our search callback with compact peer lists (DHT_EVENT_VALUES),
            which we hand to the engine's enqueuePeers (same path as trackers/PEX).
      - periodic() must be pumped on the cadence jech/dht asks for (it returns the
        seconds-until-next-call from dht_periodic); it also drives bootstrap pings.

    Singleton: jech/dht keeps ALL state in file-scope globals (no context handle),
    so only ONE DhtManager can be active per process. dht_init returns EBUSY if a
    second one starts; start() surfaces that as failure. The ephemeral model (one
    engine per playback) means this is never a real constraint.

    Threading: jech/dht is NOT thread-safe (like libutp). Every dht_* call runs on
    the single engine-loop thread only — dht_init on the caller thread strictly
    before the loop spawns, dht_periodic/dht_search/dht_ping_node on the loop, and
    dht_uninit at close() after the loop has joined. The one exception is DNS
    resolution of the bootstrap hostnames, which is NOT a dht_* call and is done off
    the loop (resolveBootstrap, run on the announcer thread) so slow DNS never stalls
    the peer loop; the resolved endpoints are pinged on the loop thread.

    Portability: this jech/dht revision routes every send through the user-provided
    dht_sendto and never reads the socket itself, so both I/O directions ride
    torrent::net::UdpSocket — DHT compiles and runs on every target. It is gated OFF
    by default on Vita (EngineConfig::enableDht) for RAM/constraint reasons, not
    portability — see types.hpp.
*/

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
    /// Called (on the engine-loop thread) with peers a search discovered.
    using PeersFn = std::function<void(const std::vector<PeerAddr>&)>;

    DhtManager();
    ~DhtManager();

    DhtManager(const DhtManager&) = delete;
    DhtManager& operator=(const DhtManager&) = delete;

    /// Bring up the UDP socket + dht_init with a fresh random node id. `peersFn` is
    /// invoked whenever a search yields peers. False if UDP is unavailable or another
    /// DhtManager is already active (jech/dht is a process singleton) — DHT then stays
    /// disabled and the engine keeps using trackers/PEX/µTP.
    bool start(PeersFn peersFn);
    void stop();  // dht_uninit + close socket — impeccable, single-threaded teardown

    bool active() const { return active_; }

    /// The dedicated UDP fd to place (once) in the engine's select() set.
    net::Handle udpHandle() const { return udp_.handle(); }

    /// Resolve the bootstrap hostnames to endpoints. Does NO dht_* call — safe (and
    /// meant) to run OFF the engine loop (announcer thread) so slow DNS never stalls
    /// the peer loop. The resolved endpoints are pinged later, on the loop thread.
    void resolveBootstrap();

    /// Start/continue a search for `infoHash`. Idempotent — re-issuing keeps the
    /// search fresh (jech/dht merges it with the in-progress one). Loop thread only.
    void search(const InfoHash& infoHash);

    /// Drain pending UDP datagrams into dht_periodic (call when the fd is readable).
    /// Loop thread only.
    void serviceReadable();

    /// Pump jech/dht's timers on the cadence it requests, and flush any bootstrap
    /// pings resolved by resolveBootstrap(). Loop thread only.
    void periodic(int64_t nowMs);

    /// good + dubious nodes in the routing table (for logs / search-readiness).
    int nodeCount() const;

    // ---- accessed by the jech/dht C callbacks (same translation unit) ----
    net::UdpSocket& udp() { return udp_; }
    /// Parse a DHT_EVENT_VALUES compact-peer blob and deliver it via peersFn_.
    void onValues(const uint8_t* data, size_t len);

private:
    // Feed one datagram (or a bare maintenance tick when buf is null) to
    // dht_periodic and refresh the next-tick deadline.
    void pump(const uint8_t* buf, size_t len, const std::string& fromIp, uint16_t fromPort);

    net::UdpSocket udp_;
    bool active_ = false;
    PeersFn peersFn_;

    InfoHash searchHash_{};
    bool haveSearch_ = false;
    int64_t lastSearchMs_ = 0;
    int64_t nextPeriodicMs_ = 0;

    // Bootstrap endpoints resolved off-loop (resolveBootstrap) and consumed on-loop
    // (periodic -> dht_ping_node). Guarded because the two run on different threads.
    std::mutex bootstrapMutex_;
    std::vector<PeerAddr> pendingBootstrap_;
    bool bootstrapDone_ = false;
};

}  // namespace torrent
