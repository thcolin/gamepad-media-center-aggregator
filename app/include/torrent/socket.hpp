/*
    GMCA — Platform socket shim (the console-portability seam).

    Only the P2P sockets go through this shim: non-blocking TCP for peer wire
    connections and UDP for BEP-15 trackers. HTTP trackers (BEP-3/23) and web
    seeds (BEP-19) go through libcurl instead (already linked on every target,
    handles TLS/DNS/redirects) — see http_client.hpp.

    Backends (selected at compile time):
      - POSIX (macOS/Linux/desktop)       -> implemented here                 [PoC]
      - libnx `bsd` (Switch)              -> BSD sockets; needs socketInitialize
                                             with a tuned SocketInitConfig +
                                             transfer memory. Mostly reuses the
                                             POSIX path once initialised.       [TODO]
      - openorbis BSD (PS4)               -> FreeBSD BSD sockets, near-POSIX.   [TODO]
      - Vita SceNet (sceNetSocket…)       -> DIFFERENT API (sceNet* + a memory
                                             pool). This is the one real rewrite;
                                             every call below has a SceNet twin. [TODO]

    globalInit()/globalShutdown() are where each platform's network stack is
    brought up (libnx socketInitializeDefault / Vita sceNetInit + sceNetCtlInit /
    openorbis sceNetInit). On POSIX they are no-ops (plus a SIGPIPE guard).
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace torrent {
namespace net {

#if defined(_WIN32)
using Handle = uintptr_t;  // SOCKET
#else
using Handle = int;  // POSIX / libnx / openorbis fd
#endif

Handle invalidHandle();

/// One-time network stack bring-up / teardown. Idempotent.
bool globalInit();
void globalShutdown();

/// Blocking DNS resolution -> dotted IPv4 strings. Call off the event loop
/// (the engine resolves trackers/peers on the announcer thread). IPv6 is out of
/// scope for the PoC (console stacks vary); only A records are returned.
std::vector<std::string> resolve(const std::string& host);

/// Non-blocking TCP socket for peer connections.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;

    bool open();  // create + set non-blocking + disable Nagle
    bool startConnect(const std::string& ip, uint16_t port);
    int checkConnected();  // 1 connected, 0 in progress, -1 failed

    // >0 bytes moved, 0 would-block, -1 closed/error.
    int send(const void* data, size_t len);
    int recv(void* buf, size_t len);

    void close();
    Handle handle() const { return fd; }
    bool valid() const;

private:
    Handle fd = invalidHandle();
};

/// UDP socket for BEP-15 (announce/connect over datagrams).
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();  // create non-blocking UDP socket
    int sendTo(const std::string& ip, uint16_t port, const void* data, size_t len);
    // >0 bytes, 0 would-block, -1 error. Fills sender endpoint when provided.
    int recvFrom(void* buf, size_t len, std::string* fromIp = nullptr, uint16_t* fromPort = nullptr);

    void close();
    Handle handle() const { return fd; }
    bool valid() const;

private:
    Handle fd = invalidHandle();
};

/// select()-based readiness poll for the event loop. libnx and openorbis both
/// expose select(); Vita has sceNetSelect with the same shape.
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
