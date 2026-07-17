/*
    GMCA — µTP transport + manager (see torrent/utp.hpp).

    This is the only translation unit that talks to libutp directly; everything
    else sees the generic PeerTransport (transport.hpp). It wires libutp's callback
    API onto our multi-platform UDP shim (torrent::net::UdpSocket) so a µTP byte
    stream looks exactly like a TCP byte stream to the peer state machine — which is
    why the MSE/PE handshake runs over µTP unchanged.
*/

#include "torrent/utp.hpp"

// libutp's sockaddr we build here is only ever an address KEY handed back to us in
// the SENDTO callback — it never reaches a raw platform socket (the datagram itself
// goes out through net::UdpSocket). These POSIX headers exist on desktop, libnx
// (Switch) and openorbis (PS4); on Vita, vitasdk's newlib ships the same BSD types
// (inet_pton/inet_ntop/sockaddr_in verified in its libc), and the actual send still
// rides SceNet via net::UdpSocket.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include "torrent/log.hpp"
#include "utp.h"

namespace torrent {

namespace {

/// Build an AF_INET sockaddr from a dotted-quad IPv4 + port. False if `ip` is not
/// a dotted quad (peers are always dotted IPv4 in this engine — see socket.hpp).
bool buildSockAddr(const std::string& ip, uint16_t port, sockaddr_in* sa) {
    std::memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &sa->sin_addr) == 1;
}

/// Decode an AF_INET sockaddr back to dotted-quad + port (for the SENDTO shim).
void decodeSockAddr(const sockaddr* addr, std::string& ip, uint16_t& port) {
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(addr);
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) ip = buf;
    port = ntohs(sin->sin_port);
}

uint64_t monotonicMicros() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---- UtpTransport -----------------------------------------------------------
//
// One per µTP peer. Wraps a libutp `utp_socket`; incoming application bytes are
// buffered in rx_ by the ON_READ callback and drained by recv(). No pollable fd:
// the engine services it via the shared UDP socket + the manager.
class UtpTransport : public PeerTransport {
public:
    UtpTransport(UtpManager* mgr, utp_socket* sock) : mgr_(mgr), sock_(sock) {}
    ~UtpTransport() override { closeInternal(); }

    bool startConnect(const PeerAddr& addr) override {
        sockaddr_in sa;
        if (!buildSockAddr(addr.host, addr.port, &sa)) return false;
        if (!sock_) return false;
        // Sends the µTP SYN synchronously through the SENDTO callback.
        utp_connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        return true;
    }

    int checkConnected() override {
        if (failed_) return -1;
        if (connected_) return 1;
        if (eof_) return -1;  // torn down before it ever connected
        return 0;
    }

    int send(const void* data, size_t len) override {
        if (!sock_ || failed_ || eof_) return -1;
        if (!connected_) return 0;  // not writable yet: would-block, retry later
        ssize_t n = utp_write(sock_, const_cast<void*>(data), len);
        if (n < 0) return -1;
        return (int)n;  // 0 => congestion window full (would-block)
    }

    int recv(void* buf, size_t len) override {
        if (!rx_.empty()) {
            size_t n = std::min(len, rx_.size());
            std::memcpy(buf, rx_.data(), n);
            rx_.erase(0, n);
            return (int)n;
        }
        if (eof_ || failed_) return -1;  // drained then closed/errored
        return 0;                        // would-block
    }

    void close() override { closeInternal(); }
    net::Handle handle() const override { return net::invalidHandle(); }
    bool pollable() const override { return false; }

    // ---- libutp callback sinks (invoked on the engine-loop thread) ----
    void onRead(const uint8_t* d, size_t n) { rx_.append(reinterpret_cast<const char*>(d), n); }
    void onConnect() { connected_ = true; }
    void onEof() { eof_ = true; }
    void onDestroying() { sock_ = nullptr; }  // libutp is freeing the socket
    void onError(int) { failed_ = true; }
    size_t rxBuffered() const { return rx_.size(); }

private:
    void closeInternal() {
        if (!sock_) return;  // already destroyed by libutp
        // Detach userdata FIRST so a later DESTROYING (during utp_check_timeouts)
        // can never reach this soon-to-be-freed transport, then let libutp tear the
        // socket down (FIN if connected, immediate reap otherwise). For an initiator
        // that never set close_requested, an errored socket is CS_RESET here, so
        // utp_close does not trip libutp's "already destroying" assert.
        utp_set_userdata(sock_, nullptr);
        utp_close(sock_);
        sock_ = nullptr;
    }

    UtpManager* mgr_;
    utp_socket* sock_;
    std::string rx_;
    bool connected_ = false;
    bool eof_ = false;
    bool failed_ = false;
};

// ---- libutp callbacks -------------------------------------------------------
//
// Routed to the manager (context userdata) or the transport (socket userdata).
// A null socket userdata means the transport is gone — the callback no-ops.

namespace {

UtpTransport* transportOf(utp_socket* s) { return s ? static_cast<UtpTransport*>(utp_get_userdata(s)) : nullptr; }

uint64 cbSendto(utp_callback_arguments* a) {
    UtpManager* mgr = static_cast<UtpManager*>(utp_context_get_userdata(a->context));
    if (!mgr || !a->address) return 0;
    std::string ip;
    uint16_t port = 0;
    decodeSockAddr(a->address, ip, port);
    if (!ip.empty()) mgr->udp().sendTo(ip, port, a->buf, a->len);
    return 0;
}

uint64 cbOnRead(utp_callback_arguments* a) {
    if (UtpTransport* t = transportOf(a->socket)) t->onRead(a->buf, a->len);
    // Tell libutp we consumed it (advances the receive window / acks).
    utp_read_drained(a->socket);
    return 0;
}

uint64 cbOnStateChange(utp_callback_arguments* a) {
    UtpTransport* t = transportOf(a->socket);
    switch (a->state) {
    case UTP_STATE_CONNECT:
    case UTP_STATE_WRITABLE:
        // Both imply writability; the engine retries flush() each loop iteration.
        if (t) t->onConnect();
        break;
    case UTP_STATE_EOF:
        if (t) t->onEof();
        break;
    case UTP_STATE_DESTROYING:
        if (t) t->onDestroying();
        break;
    default:
        break;
    }
    return 0;
}

uint64 cbOnError(utp_callback_arguments* a) {
    if (UtpTransport* t = transportOf(a->socket)) t->onError(a->error_code);
    return 0;
}

uint64 cbGetReadBufferSize(utp_callback_arguments* a) {
    // How much application data we still hold unconsumed — libutp uses it for flow
    // control (it shrinks the advertised window when we lag). We drain every loop
    // iteration, so this stays small.
    UtpTransport* t = transportOf(a->socket);
    return t ? (uint64)t->rxBuffered() : 0;
}

uint64 cbGetMilliseconds(utp_callback_arguments*) { return (uint64)(monotonicMicros() / 1000); }
uint64 cbGetMicroseconds(utp_callback_arguments*) { return monotonicMicros(); }

}  // namespace

// ---- UtpManager -------------------------------------------------------------

UtpManager::UtpManager() = default;
UtpManager::~UtpManager() { stop(); }

bool UtpManager::start() {
    if (ctx_) return true;
    if (!udp_.open()) {
        logWarn("utp: could not open shared UDP socket — µTP disabled");
        return false;
    }
    utp_context* ctx = utp_init(2);
    if (!ctx) {
        udp_.close();
        logWarn("utp: utp_init failed — µTP disabled");
        return false;
    }
    utp_context_set_userdata(ctx, this);
    utp_set_callback(ctx, UTP_SENDTO, &cbSendto);
    utp_set_callback(ctx, UTP_ON_READ, &cbOnRead);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE, &cbOnStateChange);
    utp_set_callback(ctx, UTP_ON_ERROR, &cbOnError);
    utp_set_callback(ctx, UTP_GET_READ_BUFFER_SIZE, &cbGetReadBufferSize);
    // Override libutp's default clocks so every platform uses our monotonic clock
    // (avoids relying on CLOCK_MONOTONIC availability on the console toolchains).
    utp_set_callback(ctx, UTP_GET_MILLISECONDS, &cbGetMilliseconds);
    utp_set_callback(ctx, UTP_GET_MICROSECONDS, &cbGetMicroseconds);
    ctx_ = ctx;
    logInfo("utp: BEP-29 transport active (shared UDP socket)");
    return true;
}

void UtpManager::stop() {
    if (ctx_) {
        utp_destroy(static_cast<utp_context*>(ctx_));  // frees any sockets still held
        ctx_ = nullptr;
    }
    udp_.close();
}

std::unique_ptr<PeerTransport> UtpManager::createTransport(const PeerAddr&) {
    if (!ctx_) return nullptr;
    utp_socket* s = utp_create_socket(static_cast<utp_context*>(ctx_));
    if (!s) return nullptr;
    auto t = std::make_unique<UtpTransport>(this, s);
    utp_set_userdata(s, t.get());  // route this socket's callbacks back to the transport
    return t;
}

void UtpManager::serviceReadable() {
    if (!ctx_) return;
    utp_context* ctx = static_cast<utp_context*>(ctx_);
    uint8_t buf[2048];  // µTP packets are <= UDP MTU (~1400)
    std::string fromIp;
    uint16_t fromPort = 0;
    for (;;) {
        int n = udp_.recvFrom(buf, sizeof(buf), &fromIp, &fromPort);
        if (n <= 0) break;  // 0 = would-block, -1 = error
        sockaddr_in sa;
        if (!buildSockAddr(fromIp, fromPort, &sa)) continue;
        utp_process_udp(ctx, buf, (size_t)n, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    }
    utp_issue_deferred_acks(ctx);  // flush acks batched during the drain
}

void UtpManager::checkTimeouts(int64_t nowMs) {
    if (!ctx_) return;
    // libutp recommends pumping timers on a ~500 ms cadence (retransmission, LEDBAT,
    // connect timeouts, and reaping of closed sockets -> DESTROYING).
    if (lastTimeoutMs_ != 0 && nowMs - lastTimeoutMs_ < 500) return;
    lastTimeoutMs_ = nowMs;
    utp_check_timeouts(static_cast<utp_context*>(ctx_));
}

}  // namespace torrent
