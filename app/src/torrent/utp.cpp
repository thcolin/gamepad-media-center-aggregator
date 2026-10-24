#include "torrent/utp.hpp"

// The sockaddr handed to libutp is only an address key returned in SENDTO; the
// datagram goes out through net::UdpSocket. vitasdk's newlib has these headers.
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

bool buildSockAddr(const std::string& ip, uint16_t port, sockaddr_in* sa) {
    std::memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &sa->sin_addr) == 1;
}

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

// No pollable fd: bytes arrive through the manager's UDP socket and are
// buffered in rx_ by ON_READ.
class UtpTransport : public PeerTransport {
public:
    explicit UtpTransport(utp_socket* sock) : sock_(sock) {}
    ~UtpTransport() override { closeInternal(); }

    bool startConnect(const PeerAddr& addr) override {
        sockaddr_in sa;
        if (!buildSockAddr(addr.host, addr.port, &sa)) return false;
        if (!sock_) return false;
        utp_connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        return true;
    }

    int checkConnected() override {
        if (failed_) return -1;
        if (connected_) return 1;
        if (eof_) return -1;
        return 0;
    }

    int send(const void* data, size_t len) override {
        if (!sock_ || failed_ || eof_) return -1;
        if (!connected_) return 0;
        ssize_t n = utp_write(sock_, const_cast<void*>(data), len);
        if (n < 0) return -1;
        return (int)n;  // 0: congestion window full
    }

    int recv(void* buf, size_t len) override {
        if (!rx_.empty()) {
            size_t n = std::min(len, rx_.size());
            std::memcpy(buf, rx_.data(), n);
            rx_.erase(0, n);
            return (int)n;
        }
        if (eof_ || failed_) return -1;
        return 0;
    }

    void close() override { closeInternal(); }
    net::Handle handle() const override { return net::invalidHandle(); }
    bool pollable() const override { return false; }

    void onRead(const uint8_t* d, size_t n) { rx_.append(reinterpret_cast<const char*>(d), n); }
    void onConnect() { connected_ = true; }
    void onEof() { eof_ = true; }
    void onDestroying() { sock_ = nullptr; }
    void onError(int) { failed_ = true; }
    size_t rxBuffered() const { return rx_.size(); }

private:
    void closeInternal() {
        if (!sock_) return;
        // Detach first: a later DESTROYING callback must not reach a freed transport.
        utp_set_userdata(sock_, nullptr);
        utp_close(sock_);
        sock_ = nullptr;
    }

    utp_socket* sock_;
    std::string rx_;
    bool connected_ = false;
    bool eof_ = false;
    bool failed_ = false;
};

// A null socket userdata means the transport is gone.

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
    utp_read_drained(a->socket);
    return 0;
}

uint64 cbOnStateChange(utp_callback_arguments* a) {
    UtpTransport* t = transportOf(a->socket);
    switch (a->state) {
    case UTP_STATE_CONNECT:
    case UTP_STATE_WRITABLE:
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
    // libutp shrinks the advertised window by what we still hold.
    UtpTransport* t = transportOf(a->socket);
    return t ? (uint64)t->rxBuffered() : 0;
}

uint64 cbGetMilliseconds(utp_callback_arguments*) { return (uint64)(monotonicMicros() / 1000); }
uint64 cbGetMicroseconds(utp_callback_arguments*) { return monotonicMicros(); }

}  // namespace

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
    // Our clock: CLOCK_MONOTONIC may be missing on console toolchains.
    utp_set_callback(ctx, UTP_GET_MILLISECONDS, &cbGetMilliseconds);
    utp_set_callback(ctx, UTP_GET_MICROSECONDS, &cbGetMicroseconds);
    ctx_ = ctx;
    logInfo("utp: BEP-29 transport active (shared UDP socket)");
    return true;
}

void UtpManager::stop() {
    if (ctx_) {
        utp_destroy(static_cast<utp_context*>(ctx_));
        ctx_ = nullptr;
    }
    udp_.close();
}

std::unique_ptr<PeerTransport> UtpManager::createTransport(const PeerAddr&) {
    if (!ctx_) return nullptr;
    utp_socket* s = utp_create_socket(static_cast<utp_context*>(ctx_));
    if (!s) return nullptr;
    auto t = std::make_unique<UtpTransport>(s);
    utp_set_userdata(s, t.get());
    return t;
}

void UtpManager::serviceReadable() {
    if (!ctx_) return;
    utp_context* ctx = static_cast<utp_context*>(ctx_);
    uint8_t buf[2048];
    std::string fromIp;
    uint16_t fromPort = 0;
    for (;;) {
        int n = udp_.recvFrom(buf, sizeof(buf), &fromIp, &fromPort);
        if (n <= 0) break;
        sockaddr_in sa;
        if (!buildSockAddr(fromIp, fromPort, &sa)) continue;
        utp_process_udp(ctx, buf, (size_t)n, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    }
    utp_issue_deferred_acks(ctx);
}

void UtpManager::checkTimeouts(int64_t nowMs) {
    if (!ctx_) return;
    if (lastTimeoutMs_ != 0 && nowMs - lastTimeoutMs_ < 500) return;
    lastTimeoutMs_ = nowMs;
    utp_check_timeouts(static_cast<utp_context*>(ctx_));
}

}  // namespace torrent
