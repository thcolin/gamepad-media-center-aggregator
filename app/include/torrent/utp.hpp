// µTP (BEP-29) over libutp. Part of the swarm is only reachable over UDP.
// One UDP socket carries every µTP connection. libutp is not thread-safe: every
// utp_* call and callback must run on the engine loop thread.

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

    /// False if UDP is unavailable; the engine then uses TCP only.
    bool start();
    void stop();

    bool active() const { return ctx_ != nullptr && udp_.valid(); }

    std::unique_ptr<PeerTransport> createTransport(const PeerAddr& addr);

    net::Handle udpHandle() const { return udp_.handle(); }

    /// Call when the shared UDP fd is readable.
    void serviceReadable();

    /// Throttled to libutp's recommended 500 ms.
    void checkTimeouts(int64_t nowMs);

    // For the libutp callbacks.
    void* context() const { return ctx_; }
    net::UdpSocket& udp() { return udp_; }

private:
    void* ctx_ = nullptr;  // utp_context*, opaque to keep utp.h out of this header
    net::UdpSocket udp_;
    int64_t lastTimeoutMs_ = 0;
};

}  // namespace torrent
