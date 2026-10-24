// The byte stream under a PeerConnection: TCP, or µTP (utp.hpp). MSE runs over
// either unchanged. Engine loop thread only.

#pragma once

#include <cstddef>

#include "torrent/socket.hpp"
#include "torrent/types.hpp"

namespace torrent {

class PeerTransport {
public:
    virtual ~PeerTransport() = default;

    /// `addr.host` is dotted IPv4.
    virtual bool startConnect(const PeerAddr& addr) = 0;

    /// 1 connected, 0 in progress, -1 failed.
    virtual int checkConnected() = 0;

    /// >0 bytes, 0 would-block, -1 closed or error (same for recv).
    virtual int send(const void* data, size_t len) = 0;

    virtual int recv(void* buf, size_t len) = 0;

    virtual void close() = 0;

    virtual net::Handle handle() const = 0;

    /// False for µTP: it has no fd and is serviced by the UtpManager.
    virtual bool pollable() const = 0;
};

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
