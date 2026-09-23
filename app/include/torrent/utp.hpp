/*
    GMCA — µTP transport manager (BEP-29, on top of libutp).

    Why µTP: the console is always the CONNECTING side (leech-only), but a large
    slice of the swarm is only reachable over µTP — peers/ISPs that block or
    throttle BitTorrent-over-TCP, and NAT/CGNAT paths where UDP flows more
    reliably than TCP. Adding µTP widens the reachable peer pool without touching
    the proven TCP path (see transport.hpp).

    Design — ONE UDP socket, many µTP connections:
      - The UtpManager owns a single libutp `utp_context` and ONE shared UDP socket
        (torrent::net::UdpSocket). libutp multiplexes every µTP peer over that one
        socket, keyed internally by (address, connection id).
      - libutp is callback-driven, not poll-driven. The engine loop adds the shared
        UDP fd to its select() set; when it is readable, serviceReadable() drains
        the datagrams into libutp (utp_process_udp). libutp then calls us back:
          • SENDTO           -> write a datagram out through the shared UDP socket
          • ON_READ          -> hand application bytes to the peer's UtpTransport
          • ON_STATE_CHANGE  -> connect / writable / eof / destroying
          • ON_ERROR         -> connection refused / reset / timed out
      - checkTimeouts() must be pumped periodically (retransmission / LEDBAT).

    Threading: libutp is NOT thread-safe. Every utp_* call and every callback runs
    on the single engine-loop thread only (createTransport from connectPending;
    serviceReadable/checkTimeouts from the loop; transport teardown when peers are
    erased on that same thread, or after the loop has joined at close()).

    Portability: the UDP carrier goes through torrent::net::UdpSocket, which already
    has POSIX / libnx-bsd / openorbis / SceNet backends. libutp itself is pure
    transport (no TLS, no Boost); the sockaddr we build for it is only ever an
    address key handed back to us in SENDTO — it never touches a raw platform socket.
*/

#pragma once

#include <cstdint>
#include <memory>

#include "torrent/socket.hpp"
#include "torrent/transport.hpp"
#include "torrent/types.hpp"

namespace torrent {

class UtpManager {
public:
    UtpManager();
    ~UtpManager();

    UtpManager(const UtpManager&) = delete;
    UtpManager& operator=(const UtpManager&) = delete;

    /// Bring up the libutp context + shared UDP socket. False if UDP is unavailable
    /// (µTP then simply stays disabled and the engine keeps using TCP).
    bool start();
    void stop();

    bool active() const { return ctx_ != nullptr && udp_.valid(); }

    /// Create a µTP transport dialing `addr`. Null if µTP is not active.
    std::unique_ptr<PeerTransport> createTransport(const PeerAddr& addr);

    /// The shared UDP fd to place (once) in the engine's select() set.
    net::Handle udpHandle() const { return udp_.handle(); }

    /// Drain all pending UDP datagrams into libutp and flush deferred acks. Call
    /// when the shared UDP fd is readable.
    void serviceReadable();

    /// Pump libutp's timers (retransmission, LEDBAT, connect timeouts). Internally
    /// throttled to libutp's recommended ~500 ms cadence.
    void checkTimeouts(int64_t nowMs);

    // ---- accessed by the libutp callbacks (same translation unit) ----
    void* context() const { return ctx_; }
    net::UdpSocket& udp() { return udp_; }

private:
    void* ctx_ = nullptr;  // utp_context* (opaque here to keep libutp out of the
                           // engine's include graph)
    net::UdpSocket udp_;   // single UDP socket shared by all µTP connections
    int64_t lastTimeoutMs_ = 0;
};

}  // namespace torrent
