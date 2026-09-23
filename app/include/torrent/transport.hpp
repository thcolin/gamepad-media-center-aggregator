/*
    GMCA — Peer transport abstraction (TCP wire vs µTP / BEP-29).

    A PeerConnection speaks the BitTorrent wire protocol (handshake, MSE, length-
    prefixed messages) over an ORDERED, RELIABLE byte stream. That stream can ride
    on two carriers:

      - TCP        (TcpTransport) — the original, unchanged path: one non-blocking
                     TCP socket per peer, polled by the engine loop via its fd.
      - µTP        (UtpTransport, see utp.hpp) — LEDBAT over UDP (BEP-29). All µTP
                     peers share ONE UDP socket, multiplexed by libutp; a µTP
                     transport has no pollable fd of its own and is serviced by the
                     UtpManager through libutp callbacks instead.

    The abstraction keeps the peer state machine transport-agnostic: peer.cpp only
    ever sends/receives bytes and asks "connected yet?". This is why MSE/PE (which
    is a pure byte-stream handshake) works verbatim over µTP as it does over TCP.

    Everything here runs on the single engine-loop thread (like the peers): no lock.
*/

#pragma once

#include <cstddef>

#include "torrent/socket.hpp"
#include "torrent/types.hpp"

namespace torrent {

/// Ordered, reliable byte-stream transport under a PeerConnection.
class PeerTransport {
public:
    virtual ~PeerTransport() = default;

    /// Begin an outgoing connection to `addr` (dotted IPv4 + port). Returns false
    /// if the connection could not even be started.
    virtual bool startConnect(const PeerAddr& addr) = 0;

    /// Connection progress: 1 connected, 0 still in progress, -1 failed.
    virtual int checkConnected() = 0;

    /// Non-blocking send: >0 bytes accepted, 0 would-block (retry later), -1 error.
    virtual int send(const void* data, size_t len) = 0;

    /// Non-blocking recv: >0 bytes, 0 would-block (nothing buffered), -1 closed/error.
    virtual int recv(void* buf, size_t len) = 0;

    virtual void close() = 0;

    /// The fd the engine's select() loop watches, or net::invalidHandle() for µTP
    /// (which is driven through the shared UDP socket + libutp callbacks instead).
    virtual net::Handle handle() const = 0;

    /// True for TCP (has its own pollable fd); false for µTP (serviced by the
    /// UtpManager, never placed in the peer poll set).
    virtual bool pollable() const = 0;
};

/// TCP transport — a thin wrapper over the existing non-blocking TCP socket shim.
/// The behaviour is byte-for-byte the pre-µTP path; nothing about TCP changed.
class TcpTransport : public PeerTransport {
public:
    bool startConnect(const PeerAddr& addr) override;
    int checkConnected() override { return sock_.checkConnected(); }
    int send(const void* data, size_t len) override { return sock_.send(data, len); }
    int recv(void* buf, size_t len) override { return sock_.recv(buf, len); }
    void close() override { sock_.close(); }
    net::Handle handle() const override { return sock_.handle(); }
    bool pollable() const override { return true; }

private:
    net::TcpSocket sock_;
};

}  // namespace torrent
