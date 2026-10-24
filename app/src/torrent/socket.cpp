#include "torrent/socket.hpp"

#include "torrent/log.hpp"

#if defined(__vita__)
// SceNet is not BSD. vitasdk's newlib has a BSD-over-SceNet layer, but this
// backend calls SceNet directly to own non-blocking mode and polling.
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <cstring>
#elif !defined(_WIN32)
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace torrent {
namespace net {

#if defined(_WIN32)
// TODO(win): winsock backend (WSAStartup, closesocket, ioctlsocket FIONBIO).
Handle invalidHandle() { return (Handle)~0ull; }
bool globalInit() { return false; }
void globalShutdown() {}
std::vector<std::string> resolve(const std::string&) { return {}; }

#elif defined(__vita__)

// A failing sceNet* call returns the SCE_NET_ERROR_* code itself (negative);
// there is no errno to read.

static inline bool sceWouldBlock(int r) {
    unsigned e = (unsigned)r;
    return e == (unsigned)SCE_NET_ERROR_EAGAIN || e == (unsigned)SCE_NET_ERROR_EWOULDBLOCK ||
           e == (unsigned)SCE_NET_ERROR_EINTR;
}

Handle invalidHandle() { return -1; }

#if defined(TORRENT_VITA_OWN_SOCKET)
// Net pool for sceNetInit: the official sample uses 1 MiB, newlib ~140 KiB.
static uint8_t s_vitaNetPool[512 * 1024];
static bool s_vitaOwned = false;
#endif

bool globalInit() {
#if defined(TORRENT_VITA_OWN_SOCKET)
    // Only for a standalone harness where nothing else brought SceNet up.
    if (s_vitaOwned) return true;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
    }
    SceNetInitParam param;
    param.memory = s_vitaNetPool;
    param.size = (int)sizeof(s_vitaNetPool);
    param.flags = 0;
    int r = sceNetInit(&param);
    // EBUSY: already up (newlib's lazy _vita_net_init ran first).
    if (r < 0 && (unsigned)r != (unsigned)SCE_NET_ERROR_EBUSY) return false;
    sceNetCtlInit();
    s_vitaOwned = true;
    return true;
#else
    // newlib's socket() (used by curl) already brought SceNet up. The engine
    // lives for one playback and must not own the stack: a sceNetTerm would
    // tear it down under curl and mpv. SceNet raises no SIGPIPE.
    return true;
#endif
}

void globalShutdown() {
#if defined(TORRENT_VITA_OWN_SOCKET)
    if (s_vitaOwned) {
        sceNetCtlTerm();
        sceNetTerm();
        s_vitaOwned = false;
    }
#endif
}

std::vector<std::string> resolve(const std::string& host) {
    std::vector<std::string> out;
    SceNetInAddr probe;
    if (sceNetInetPton(SCE_NET_AF_INET, host.c_str(), &probe) == 1) {
        out.push_back(host);
        return out;
    }
    // Blocking, off the event loop. The timeout keeps teardown from hanging on DNS.
    int rid = sceNetResolverCreate("gmca-dns", nullptr, 0);
    if (rid < 0) return out;
    SceNetInAddr addr;
    int r = sceNetResolverStartNtoa(rid, host.c_str(), &addr, /*timeout µs*/ 10 * 1000 * 1000, /*retry*/ 2, 0);
    if (r >= 0) {
        char buf[32];
        if (sceNetInetNtop(SCE_NET_AF_INET, &addr, buf, sizeof(buf))) out.emplace_back(buf);
    }
    sceNetResolverDestroy(rid);
    return out;
}

static bool setNonBlocking(int fd) {
    int nb = 1;
    return sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb)) >= 0;
}

static bool makeSceAddr(const std::string& ip, uint16_t port, SceNetSockaddrIn* addr) {
    std::memset(addr, 0, sizeof(*addr));
    addr->sin_family = SCE_NET_AF_INET;
    addr->sin_port = sceNetHtons(port);
    return sceNetInetPton(SCE_NET_AF_INET, ip.c_str(), &addr->sin_addr) == 1;
}

TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd(o.fd) { o.fd = -1; }
TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd = o.fd;
        o.fd = -1;
    }
    return *this;
}
bool TcpSocket::valid() const { return fd >= 0; }

bool TcpSocket::open() {
    fd = sceNetSocket("gmca-tcp", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        fd = -1;
        return false;
    }
    if (!setNonBlocking(fd)) {
        close();
        return false;
    }
    int one = 1;
    sceNetSetsockopt(fd, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));
    return true;
}

bool TcpSocket::startConnect(const std::string& ip, uint16_t port) {
    SceNetSockaddrIn addr;
    if (!makeSceAddr(ip, port, &addr)) return false;
    int r = sceNetConnect(fd, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
    if (r == 0) return true;
    unsigned e = (unsigned)r;
    return e == (unsigned)SCE_NET_ERROR_EINPROGRESS || e == (unsigned)SCE_NET_ERROR_EINTR;
}

int TcpSocket::checkConnected() {
    int err = 0;
    unsigned len = sizeof(err);
    if (sceNetGetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &err, &len) < 0) return -1;
    if (err == 0) return 1;
    return -1;
}

int TcpSocket::send(const void* data, size_t len) {
    int n = sceNetSend(fd, data, (unsigned)len, 0);
    if (n >= 0) return n;
    if (sceWouldBlock(n)) return 0;
    return -1;
}

int TcpSocket::recv(void* buf, size_t len) {
    int n = sceNetRecv(fd, buf, (unsigned)len, 0);
    if (n > 0) return n;
    if (n == 0) return -1;
    if (sceWouldBlock(n)) return 0;
    return -1;
}

void TcpSocket::close() {
    if (fd >= 0) {
        sceNetSocketClose(fd);
        fd = -1;
    }
}

UdpSocket::~UdpSocket() { close(); }
bool UdpSocket::valid() const { return fd >= 0; }

bool UdpSocket::open() {
    fd = sceNetSocket("gmca-udp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (fd < 0) {
        fd = -1;
        return false;
    }
    if (!setNonBlocking(fd)) {
        close();
        return false;
    }
    return true;
}

int UdpSocket::sendTo(const std::string& ip, uint16_t port, const void* data, size_t len) {
    SceNetSockaddrIn addr;
    if (!makeSceAddr(ip, port, &addr)) return -1;
    int n = sceNetSendto(fd, data, (unsigned)len, 0, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr));
    if (n >= 0) return n;
    if (sceWouldBlock(n)) return 0;
    return -1;
}

int UdpSocket::recvFrom(void* buf, size_t len, std::string* fromIp, uint16_t* fromPort) {
    SceNetSockaddrIn addr;
    unsigned alen = sizeof(addr);
    int n = sceNetRecvfrom(fd, buf, (unsigned)len, 0, reinterpret_cast<SceNetSockaddr*>(&addr), &alen);
    if (n >= 0) {
        if (fromIp) {
            char ip[32];
            if (sceNetInetNtop(SCE_NET_AF_INET, &addr.sin_addr, ip, sizeof(ip))) *fromIp = ip;
        }
        if (fromPort) *fromPort = sceNetNtohs(addr.sin_port);
        return n;
    }
    if (sceWouldBlock(n)) return 0;
    return -1;
}

void UdpSocket::close() {
    if (fd >= 0) {
        sceNetSocketClose(fd);
        fd = -1;
    }
}

int poll(std::vector<PollItem>& items, int timeoutMs) {
    // SceNet has no select(). One epoll per call is fine for a handful of fds;
    // the wait timeout is in microseconds.
    for (auto& it : items) it.readable = it.writable = it.error = false;

    int eid = sceNetEpollCreate("gmca-poll", 0);
    if (eid < 0) {
        // Sleep the timeout anyway so the caller's loop keeps its pace.
        sceKernelDelayThread((SceUInt)timeoutMs * 1000);
        return 0;
    }

    int registered = 0;
    for (auto& it : items) {
        if (it.fd < 0) continue;
        SceNetEpollEvent ev;
        std::memset(&ev, 0, sizeof(ev));
        if (it.wantRead) ev.events |= SCE_NET_EPOLLIN;
        if (it.wantWrite) ev.events |= SCE_NET_EPOLLOUT;
        ev.data.fd = it.fd;
        if (sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, it.fd, &ev) >= 0) registered++;
    }

    if (registered == 0) {
        sceNetEpollDestroy(eid);
        sceKernelDelayThread((SceUInt)timeoutMs * 1000);
        return 0;
    }

    std::vector<SceNetEpollEvent> evs(items.size());
    int nev = sceNetEpollWait(eid, evs.data(), (int)evs.size(), timeoutMs * 1000);
    if (nev > 0) {
        for (int i = 0; i < nev; i++) {
            int fd = evs[i].data.fd;
            unsigned e = evs[i].events;
            for (auto& it : items) {
                if (it.fd != fd) continue;
                if (e & SCE_NET_EPOLLIN) it.readable = true;
                if (e & SCE_NET_EPOLLOUT) it.writable = true;
                if (e & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) it.error = true;
            }
        }
    }
    sceNetEpollDestroy(eid);
    return nev < 0 ? -1 : nev;
}

#else

Handle invalidHandle() { return -1; }

bool globalInit() {
#if defined(__SWITCH__)
    // libnx: the BSD socket stack is process-global. A second socketInitialize()
    // fails and socketExit() kills it for curl and mpv too. borealis brings it up
    // before main() (switch_wrapper.c, num_bsd_sessions = 12, sb_efficiency = 8);
    // more sessions belong there, not here. libnx raises no SIGPIPE.
#if defined(TORRENT_SWITCH_OWN_SOCKET)
    // Only for a standalone harness without borealis.
    static bool s_inited = false;
    if (s_inited) return true;
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.num_bsd_sessions = 12;
    cfg.sb_efficiency = 8;
    s_inited = R_SUCCEEDED(socketInitialize(&cfg));
    return s_inited;
#else
    return true;
#endif
#else
    // Desktop and PS4 (FreeBSD sockets, SceNet brought up by borealis and the
    // app): a peer closing mid-send must not kill the process.
    signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void globalShutdown() {
#if defined(__SWITCH__) && defined(TORRENT_SWITCH_OWN_SOCKET)
    socketExit();
#endif
}

std::vector<std::string> resolve(const std::string& host) {
    std::vector<std::string> out;
    struct in_addr tmp;
    if (inet_pton(AF_INET, host.c_str(), &tmp) == 1) {
        out.push_back(host);
        return out;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return out;
    for (auto* p = res; p; p = p->ai_next) {
        auto* a = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) out.push_back(buf);
    }
    freeaddrinfo(res);
    return out;
}

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd(o.fd) { o.fd = -1; }
TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd = o.fd;
        o.fd = -1;
    }
    return *this;
}
bool TcpSocket::valid() const { return fd >= 0; }

bool TcpSocket::open() {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    if (!setNonBlocking(fd)) {
        close();
        return false;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

bool TcpSocket::startConnect(const std::string& ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
    int r = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (r == 0) return true;
    return errno == EINPROGRESS || errno == EINTR;
}

int TcpSocket::checkConnected() {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return -1;
    if (err == 0) return 1;
    return -1;
}

int TcpSocket::send(const void* data, size_t len) {
    ssize_t n = ::send(fd, data, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int TcpSocket::recv(void* buf, size_t len) {
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void TcpSocket::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

UdpSocket::~UdpSocket() { close(); }
bool UdpSocket::valid() const { return fd >= 0; }

bool UdpSocket::open() {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    if (!setNonBlocking(fd)) {
        close();
        return false;
    }
    return true;
}

int UdpSocket::sendTo(const std::string& ip, uint16_t port, const void* data, size_t len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return -1;
    ssize_t n = ::sendto(fd, data, len, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int UdpSocket::recvFrom(void* buf, size_t len, std::string* fromIp, uint16_t* fromPort) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    ssize_t n = ::recvfrom(fd, buf, len, 0, reinterpret_cast<struct sockaddr*>(&addr), &alen);
    if (n >= 0) {
        if (fromIp) {
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip))) *fromIp = ip;
        }
        if (fromPort) *fromPort = ntohs(addr.sin_port);
        return (int)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void UdpSocket::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

int poll(std::vector<PollItem>& items, int timeoutMs) {
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    int maxfd = -1;
    for (auto& it : items) {
        it.readable = it.writable = it.error = false;
        if (it.fd < 0) continue;
        if (it.wantRead) FD_SET(it.fd, &rfds);
        if (it.wantWrite) FD_SET(it.fd, &wfds);
        FD_SET(it.fd, &efds);
        if (it.fd > maxfd) maxfd = it.fd;
    }
    if (maxfd < 0) {
        // Nothing to wait on: select() with no fds just sleeps the timeout.
        struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        return ::select(0, nullptr, nullptr, nullptr, &tv);
    }
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = ::select(maxfd + 1, &rfds, &wfds, &efds, &tv);
    if (r <= 0) return r;
    for (auto& it : items) {
        if (it.fd < 0) continue;
        it.readable = FD_ISSET(it.fd, &rfds);
        it.writable = FD_ISSET(it.fd, &wfds);
        it.error = FD_ISSET(it.fd, &efds);
    }
    return r;
}

#endif

}  // namespace net
}  // namespace torrent
