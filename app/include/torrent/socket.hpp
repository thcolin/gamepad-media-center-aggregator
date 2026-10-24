// Platform socket shim for peer TCP and UDP (trackers, DHT, uTP). HTTP goes
// through libcurl (http_client.hpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace torrent {
namespace net {

#if defined(_WIN32)
using Handle = uintptr_t;
#else
using Handle = int;
#endif

Handle invalidHandle();

/// Idempotent. In the app the network stack is owned by the platform layer.
bool globalInit();
void globalShutdown();

/// Blocking, call off the event loop. IPv4 only.
std::vector<std::string> resolve(const std::string& host);

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;

    bool open();  // non-blocking, TCP_NODELAY
    bool startConnect(const std::string& ip, uint16_t port);
    int checkConnected();  // 1 connected, 0 in progress, -1 failed; call once writable

    // >0 bytes moved, 0 would-block, -1 closed/error.
    int send(const void* data, size_t len);
    int recv(void* buf, size_t len);

    void close();
    Handle handle() const { return fd; }
    bool valid() const;

private:
    Handle fd = invalidHandle();
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    int sendTo(const std::string& ip, uint16_t port, const void* data, size_t len);
    // >0 bytes, 0 would-block, -1 error.
    int recvFrom(void* buf, size_t len, std::string* fromIp = nullptr, uint16_t* fromPort = nullptr);

    void close();
    Handle handle() const { return fd; }
    bool valid() const;

private:
    Handle fd = invalidHandle();
};

/// select() everywhere except Vita, which uses sceNetEpoll.
struct PollItem {
    Handle fd;
    bool wantRead = false;
    bool wantWrite = false;
    bool readable = false;
    bool writable = false;
    bool error = false;
};
int poll(std::vector<PollItem>& items, int timeoutMs);

}  // namespace net
}  // namespace torrent
