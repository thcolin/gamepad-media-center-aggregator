/*
    GMCA — POSIX socket shim implementation (see torrent/socket.hpp).

    POSIX path only. The console backends reuse most of this (libnx/openorbis are
    BSD sockets) or reimplement it against SceNet (Vita); each divergence is
    flagged with a TODO seam. Kept free of engine types so it is trivially
    liftable to another codebase.
*/

#include "torrent/socket.hpp"

#include "torrent/log.hpp"

#if defined(__vita__)
// PS Vita: SceNet is NOT BSD — it has its own API (sceNet*) and a memory pool.
// We deliberately do NOT pull the POSIX socket headers here and implement the
// whole shim against SceNet in the #elif defined(__vita__) block below. Note:
// vitasdk's newlib DOES ship a working BSD-over-SceNet layer (socket()/connect()/
// select()->sceNetEpoll*/getaddrinfo, verified in newlib libc.a), so the POSIX
// path would also compile and run on Vita; this backend talks to SceNet directly
// so it owns non-blocking mode, the epoll poll(), and the pool lifecycle without
// depending on newlib internals. Confirmed predefined macro: arm-vita-eabi-g++
// -dM defines __vita__ (and only that), analogous to __SWITCH__.
#include <psp2/kernel/threadmgr.h>  // sceKernelDelayThread (idle poll)
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <cstring>
#elif !defined(_WIN32)
#if defined(__SWITCH__)
// libnx exposes the socket lifecycle (socketInitialize/socketExit,
// socketGetDefaultInitConfig, SocketInitConfig) through <switch.h>; the BSD
// sockets themselves come from the standard headers below (libnx `bsd`).
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
// The PoC targets POSIX; Windows desktop would wire this branch.
Handle invalidHandle() { return (Handle)~0ull; }
bool globalInit() { return false; }
void globalShutdown() {}
std::vector<std::string> resolve(const std::string&) { return {}; }

#elif defined(__vita__)

// ---- PS Vita (SceNet) -------------------------------------------------------
//
// SceNet is Sony's own sockets API — NOT BSD. Every POSIX call in the #else
// branch has a sceNet* twin with different names/types: SceNetSockaddrIn instead
// of sockaddr_in, sceNetHtons/sceNetInetPton for byte order/parsing,
// SCE_NET_SO_NBIO for non-blocking, and sceNetEpoll* instead of select() (SceNet
// exposes no select). Error convention (verified against vitasdk newlib
// socket.c:116-124): a sceNet* call returns < 0 on failure and the return value
// IS the SCE_NET_ERROR_* code (0x8041xxxx, negative as int32) — there is no
// separate errno to read. SCE_NET_ERROR_EWOULDBLOCK == EAGAIN (0x80410123).

// Would-block / retry classification for a negative sceNet return.
static inline bool sceWouldBlock(int r) {
    unsigned e = (unsigned)r;
    return e == (unsigned)SCE_NET_ERROR_EAGAIN || e == (unsigned)SCE_NET_ERROR_EWOULDBLOCK ||
           e == (unsigned)SCE_NET_ERROR_EINTR;
}

Handle invalidHandle() { return -1; }

#if defined(TORRENT_VITA_OWN_SOCKET)
// Net pool for sceNetInit. The official sample uses 1 MiB; newlib's shim uses
// ~140 KiB. 512 KiB comfortably covers the console peer cap (EngineConfig::
// maxPeers, ~4-8) plus the loopback HTTP server. Tunable.
static uint8_t s_vitaNetPool[512 * 1024];
static bool s_vitaOwned = false;
#endif

bool globalInit() {
#if defined(TORRENT_VITA_OWN_SOCKET)
    // Opt-in ONLY for a standalone harness that runs the engine without borealis
    // and without curl (nobody brought SceNet up). Never define this in the app.
    if (s_vitaOwned) return true;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
    }
    SceNetInitParam param;
    param.memory = s_vitaNetPool;
    param.size = (int)sizeof(s_vitaNetPool);
    param.flags = 0;
    int r = sceNetInit(&param);
    // EBUSY == already up (e.g. newlib's lazy _vita_net_init already ran); benign.
    if (r < 0 && (unsigned)r != (unsigned)SCE_NET_ERROR_EBUSY) return false;
    sceNetCtlInit();
    s_vitaOwned = true;
    return true;
#else
    // Default: NO-OP — ride the SceNet stack the app already brought up. curl on
    // Vita goes through newlib's BSD sockets, whose socket() lazily runs
    // _vita_net_init() (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) + sceNetInit +
    // pool), so by playback time (after API traffic) SceNet is guaranteed up. The
    // ephemeral engine must NOT own that global lifecycle (the Switch lesson): a
    // paired sceNetTerm would tear the stack down under curl/mpv. SceNet raises no
    // SIGPIPE, so no guard is needed either.
    return true;
#endif
}

void globalShutdown() {
#if defined(TORRENT_VITA_OWN_SOCKET)
    // Only the standalone harness (which called sceNetInit itself) may tear down.
    if (s_vitaOwned) {
        sceNetCtlTerm();
        sceNetTerm();
        s_vitaOwned = false;
    }
#endif
    // App-hosted Vita: nothing — the process owns the SceNet stack.
}

std::vector<std::string> resolve(const std::string& host) {
    std::vector<std::string> out;
    // A dotted-quad passes straight through (sceNetInetPton returns 1 on success,
    // matching inet_pton — verified in newlib inet_pton.c).
    SceNetInAddr probe;
    if (sceNetInetPton(SCE_NET_AF_INET, host.c_str(), &probe) == 1) {
        out.push_back(host);
        return out;
    }
    // Off the event loop (announcer thread), so a blocking resolve is fine. The
    // newlib reference (gethostbyname.c) uses create("resolver",NULL,0) +
    // StartNtoa. We pass a bounded timeout so teardown cannot hang on a slow DNS.
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

// Build a SceNetSockaddrIn for a dotted-quad IPv4 + port. False if ip is invalid.
static bool makeSceAddr(const std::string& ip, uint16_t port, SceNetSockaddrIn* addr) {
    std::memset(addr, 0, sizeof(*addr));
    addr->sin_family = SCE_NET_AF_INET;
    addr->sin_port = sceNetHtons(port);
    return sceNetInetPton(SCE_NET_AF_INET, ip.c_str(), &addr->sin_addr) == 1;
}

// ---- TcpSocket --------------------------------------------------------------

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
    if (r == 0) return true;  // immediate (localhost)
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
    if (n == 0) return -1;  // peer closed
    if (sceWouldBlock(n)) return 0;
    return -1;
}

void TcpSocket::close() {
    if (fd >= 0) {
        sceNetSocketClose(fd);
        fd = -1;
    }
}

// ---- UdpSocket --------------------------------------------------------------

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

// ---- poll -------------------------------------------------------------------

int poll(std::vector<PollItem>& items, int timeoutMs) {
    // SceNet has no select(); it exposes epoll. newlib's select() maps to exactly
    // this (verified in newlib select.c): create -> CTL_ADD each fd -> wait with a
    // MICROSECOND timeout -> map events back. We build/tear an epoll per call,
    // matching the engine's ephemeral, low-fd (~4-8 peers) event loop.
    for (auto& it : items) it.readable = it.writable = it.error = false;

    int eid = sceNetEpollCreate("gmca-poll", 0);
    if (eid < 0) {
        // Nothing usable; emulate the timeout so the caller's loop paces itself.
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
        // EPOLLERR/EPOLLHUP are reported regardless; nothing to request for them.
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
    // Nintendo Switch (libnx `bsd` sockets).
    //
    // The BSD socket stack is a PROCESS-GLOBAL singleton here: a second
    // socketInitialize() on an already-initialised service fails, and socketExit()
    // tears the stack down for the WHOLE process (curl, mpv, everything). The
    // engine is ephemeral — one instance per playback, destroyed on stop/sleep — so
    // it must never own that global lifecycle. Owning it is exactly the sleep/exit
    // fragility that plagued nxTransmission (broken-pipe on sleep, crash on exit).
    //
    // In the shipping app, borealis already brings the stack up in userAppInit()
    // (library/borealis/.../platforms/switch/switch_wrapper.c) BEFORE main(), with a
    // swarm-friendly config for Application/title-takeover mode:
    //     num_bsd_sessions = 12  (libnx default 3)
    //     sb_efficiency    = 8   (libnx default 4)
    // keeping the default 256 KiB (0x40000) TCP tx/rx max buffers. The bsd transfer
    // memory is sized sb_efficiency * pageRoundUp(tcp_tx_max + tcp_rx_max + udp_tx +
    // udp_rx) = 8 * 0x8D000 ~= 4.4 MiB (vs ~2.2 MiB at the default sb_efficiency=4)
    // — a fixed one-time cost the app already pays, negligible against the ~3.2 GiB
    // the forwarder title takeover grants. That budget comfortably covers the
    // engine's console peer cap (EngineConfig::maxPeers, 8-16) plus curl and the
    // loopback HTTP server; 12 BSD sessions is ample concurrency for our
    // non-blocking select() peer loop alongside the blocking curl / HTTP-server
    // threads. If a future profile needs more, the fix belongs in borealis'
    // switch_wrapper.c (via scripts/patches/borealis-fixes.patch), NOT a concurrent
    // init here.
    //
    // Default path: NO-OP — ride the app-owned stack. libnx BSD sockets never raise
    // SIGPIPE, so no guard is needed either. Everything below (socket/connect/send/
    // recv/select/getaddrinfo/setsockopt) is plain BSD, shared verbatim with POSIX.
#if defined(TORRENT_SWITCH_OWN_SOCKET)
    // Opt-in ONLY for a standalone Switch harness that runs the engine without
    // borealis (nobody else called socketInitialize). Never define this in the app.
    static bool s_inited = false;
    if (s_inited) return true;
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.num_bsd_sessions = 12;  // headroom for a peer swarm + curl + HTTP server
    cfg.sb_efficiency = 8;      // double the pooled socket buffers (throughput)
    // (default 256 KiB tcp_tx/rx_buf_max_size kept -> transfer memory ~= 4.4 MiB)
    s_inited = R_SUCCEEDED(socketInitialize(&cfg));
    return s_inited;
#else
    return true;
#endif
#else
    // POSIX (desktop) AND PS4 (openorbis): ignore SIGPIPE so a peer vanishing
    // mid-send does not kill us. PS4 is FreeBSD BSD sockets — this whole branch
    // compiles and runs verbatim (verified: socket.cpp compiles under the
    // openorbis clang, __ORBIS__ set by its toolchain), and the SceNet stack is
    // already brought up by borealis (ps4_platform sceNetCtlInit) + the app
    // (config.cpp loads ORBIS_SYSMODULE_INTERNAL_NET), so no init is owned here.
    // Vita is NOT here: SceNet is not BSD — see the #elif defined(__vita__) block.
    signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void globalShutdown() {
#if defined(__SWITCH__) && defined(TORRENT_SWITCH_OWN_SOCKET)
    // Only the standalone harness (which called socketInitialize itself) may tear
    // the stack down. In the app, socketExit() is owned by borealis' userAppExit().
    socketExit();
#endif
    // POSIX / app-hosted Switch: nothing to do — the process owns the socket stack.
}

std::vector<std::string> resolve(const std::string& host) {
    std::vector<std::string> out;
    // A dotted-quad passes straight through.
    struct in_addr tmp;
    if (inet_pton(AF_INET, host.c_str(), &tmp) == 1) {
        out.push_back(host);
        return out;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // PoC: IPv4 only
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

// ---- TcpSocket --------------------------------------------------------------

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
    if (r == 0) return true;                        // immediate (localhost)
    return errno == EINPROGRESS || errno == EINTR;  // async in progress
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
    if (n == 0) return -1;  // peer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void TcpSocket::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// ---- UdpSocket --------------------------------------------------------------

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

// ---- poll -------------------------------------------------------------------

int poll(std::vector<PollItem>& items, int timeoutMs) {
    // POSIX / Switch (libnx bsd) / PS4 (openorbis FreeBSD) all expose select().
    // Vita has no select() and uses sceNetEpoll* — see the #elif __vita__ poll().
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
        // Nothing to wait on; emulate the timeout with a short sleep-free return.
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

#endif  // backend select (_WIN32 / __vita__ / POSIX+Switch+PS4)

}  // namespace net
}  // namespace torrent
