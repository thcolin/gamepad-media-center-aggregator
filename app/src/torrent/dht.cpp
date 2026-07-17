/*
    GMCA — mainline DHT manager + jech/dht glue (see torrent/dht.hpp).

    This is the only translation unit that talks to jech/dht directly. It:
      - provides the four callbacks jech/dht leaves to the application (dht_sendto,
        dht_blacklisted, dht_hash, dht_random_bytes);
      - drives dht_init / dht_periodic / dht_search / dht_ping_node / dht_uninit;
      - routes ALL socket I/O through torrent::net::UdpSocket (the platform shim), so
        the vendored C source builds and runs unmodified on every target.

    jech/dht keeps its whole state in file-scope globals (it is a process singleton),
    so the callbacks reach "the" manager through a single file-scope pointer set at
    start() — the same shape jech/dht itself uses. Everything here runs on the engine
    loop thread except resolveBootstrap() (DNS only, no dht_* call).
*/

#include "torrent/dht.hpp"

// sockaddr types + inet_pton/inet_ntop + htons/ntohs. Present on desktop, libnx
// (Switch), openorbis (PS4) and vitasdk newlib. The sockaddr we build is only ever
// handed back to us in dht_sendto (decoded to dotted-quad for net::UdpSocket) or
// fed to dht_periodic as the datagram source — it never reaches a raw platform call.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <atomic>
#include <chrono>
#include <random>

#include "torrent/log.hpp"
#include "torrent/sha1.hpp"
#include "torrent/util.hpp"

// Vendored jech/dht (C). Included last so FILE / struct sockaddr / size_t / time_t
// are already declared. Its declarations are wrapped in extern "C"; the callback
// definitions below therefore acquire C linkage from those prior declarations.
#include "dht.h"

#include "torrent/util.hpp"  // osRandom (console-safe OS entropy; no /dev/urandom)

// RNG backends, picked at compile time the same way sha1.cpp / crypto.cpp dispatch.
#if defined(TORRENT_SHA1_OPENSSL)
#include <openssl/rand.h>
#elif defined(TORRENT_SHA1_MBEDTLS)
#include <mbedtls/ctr_drbg.h>
#elif defined(__APPLE__)
#include <cstdlib>  // arc4random_buf
#else
#include <fcntl.h>   // /dev/urandom
#include <unistd.h>  // open/read/close
#endif

namespace torrent {

namespace {

// The one active manager, so the C callbacks can reach it. jech/dht is a process
// singleton, so at most one is ever set. Written on the caller thread before the
// engine loop starts (start) and cleared after it joins (stop) — never a race.
DhtManager* g_dht = nullptr;

/// Build an AF_INET sockaddr from a dotted-quad IPv4 + port. False if `ip` is not a
/// dotted quad (DHT peers/nodes are always dotted IPv4 here — s6 is disabled).
bool buildSockAddr(const std::string& ip, uint16_t port, sockaddr_in* sa) {
    std::memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &sa->sin_addr) == 1;
}

/// Decode an AF_INET sockaddr back to dotted-quad + port (for the dht_sendto shim).
void decodeSockAddr(const sockaddr* addr, std::string& ip, uint16_t& port) {
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(addr);
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) ip = buf;
    port = ntohs(sin->sin_port);
}

/// Fill `buf` with `size` cryptographically-strong random bytes. Backend chosen at
/// compile time behind the same switch as the SHA-1 dispatch (sha1.cpp): OpenSSL /
/// mbedtls CTR_DRBG on console / CommonCrypto-era Apple arc4random / /dev/urandom.
/// Used for the DHT node id (once) and jech/dht's token secret (~every 30 min), so
/// correctness matters far more than speed.
bool fillCryptoRandom(void* buf, size_t size) {
#if defined(TORRENT_SHA1_OPENSSL)
    return RAND_bytes(reinterpret_cast<unsigned char*>(buf), (int)size) == 1;
#elif defined(TORRENT_SHA1_MBEDTLS)
    // Console: a proper CTR_DRBG. mbedtls' own entropy module may have no hardware
    // source registered on these toolchains, so we seed the DRBG from a best-effort
    // mix (OS CSPRNG via osRandom + a high-resolution clock + a per-call counter +
    // a stack address) rather than trusting a single source. CTR_DRBG then expands
    // that seed into strong output.
    struct Seed {
        static int gather(void*, unsigned char* out, size_t len) {
            // Base entropy from the OS CSPRNG (console-safe: never /dev/urandom, so
            // no NULL-FILE* fault on Switch/Vita), XORed with a high-res clock, a
            // per-call counter and a stack address as defense in depth.
            osRandom(out, len);
            static std::atomic<uint64_t> ctr{0x9E3779B97F4A7C15ULL};
            uint64_t mix = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
            mix ^= ctr.fetch_add(0x9E3779B97F4A7C15ULL);
            mix ^= (uint64_t)(uintptr_t)&out;
            for (size_t i = 0; i < len && i < sizeof(mix); i++) out[i] ^= ((unsigned char*)&mix)[i];
            return 0;
        }
    };
    mbedtls_ctr_drbg_context ctx;
    mbedtls_ctr_drbg_init(&ctx);
    int rc = mbedtls_ctr_drbg_seed(&ctx, &Seed::gather, nullptr, nullptr, 0);
    if (rc == 0) rc = mbedtls_ctr_drbg_random(&ctx, reinterpret_cast<unsigned char*>(buf), size);
    mbedtls_ctr_drbg_free(&ctx);
    return rc == 0;
#elif defined(__APPLE__)
    arc4random_buf(buf, size);  // BSD/macOS CSPRNG, no fd, always available
    return true;
#else
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < size) {
            ssize_t n = ::read(fd, reinterpret_cast<char*>(buf) + got, size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        ::close(fd);
        if (got == size) return true;
    }
    // Fallback: OS entropy via osRandom (console-safe; /dev/urandom on desktop).
    osRandom(buf, size);
    return true;
#endif
}

}  // namespace

// ---- jech/dht application callbacks -----------------------------------------
//
// Declared (extern "C") at global scope in dht.h; the definitions live lexically
// inside namespace torrent (so they can reach the anonymous-namespace helpers and
// torrent:: types), but the `extern "C"` wrapper gives each the plain C linker
// symbol jech/dht's C source resolves against — without it they would be mangled
// C++ functions (torrent::dht_sendto) and the link would fail.
extern "C" {

/// Emit one datagram. jech/dht never touches the socket fd itself — it hands the fd
/// back here as an opaque token and lets us send however we like, which is exactly
/// what makes the platform shim usable. We route to the active manager's UDP socket.
int dht_sendto(int /*sockfd*/, const void* buf, int len, int /*flags*/, const struct sockaddr* to, int /*tolen*/) {
    DhtManager* mgr = g_dht;
    if (!mgr || !to) {
        errno = EINVAL;
        return -1;
    }
    if (to->sa_family != AF_INET) {  // IPv4-only session (dht_init s6 = -1)
        errno = EAFNOSUPPORT;
        return -1;
    }
    std::string ip;
    uint16_t port = 0;
    decodeSockAddr(to, ip, port);
    if (ip.empty()) {
        errno = EINVAL;
        return -1;
    }
    int n = mgr->udp().sendTo(ip, port, buf, (size_t)len);
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    // Treat a would-block (n == 0) as sent: the datagram is best-effort and jech/dht
    // re-queries/re-pings on its periodic timers.
    return len;
}

/// Never blacklist — Kademlia assumes transitive reachability (README).
int dht_blacklisted(const struct sockaddr* /*sa*/, int /*salen*/) { return 0; }

/// Strong hash of v1||v2||v3. README: "SHA-1 should be good enough" — we reuse the
/// engine's SHA-1 (hardware-backed on every target). Only ever asked for TOKEN_SIZE
/// (8) bytes; we copy the leading bytes of the 20-byte digest.
void dht_hash(void* hash_return, int hash_size, const void* v1, int len1, const void* v2, int len2, const void* v3,
    int len3) {
    std::string in;
    in.reserve((size_t)(len1 + len2 + len3));
    if (v1 && len1 > 0) in.append(reinterpret_cast<const char*>(v1), (size_t)len1);
    if (v2 && len2 > 0) in.append(reinterpret_cast<const char*>(v2), (size_t)len2);
    if (v3 && len3 > 0) in.append(reinterpret_cast<const char*>(v3), (size_t)len3);
    Sha1Digest d = sha1(in.data(), in.size());
    int n = hash_size < (int)d.size() ? hash_size : (int)d.size();
    std::memcpy(hash_return, d.data(), (size_t)n);
    if (hash_size > (int)d.size()) std::memset(reinterpret_cast<char*>(hash_return) + d.size(), 0, hash_size - d.size());
}

/// Cryptographically-strong random bytes for the node id + token secret.
int dht_random_bytes(void* buf, size_t size) { return fillCryptoRandom(buf, size) ? (int)size : -1; }

}  // extern "C"

// ---- search event callback --------------------------------------------------
//
// Passed to dht_periodic/dht_search; fired synchronously on the loop thread when a
// search yields peers or completes. extern "C" so its type is exactly dht_callback_t.

extern "C" void gmcaDhtEvent(void* closure, int event, const unsigned char* /*info_hash*/, const void* data,
    size_t data_len) {
    DhtManager* mgr = static_cast<DhtManager*>(closure);
    if (!mgr) return;
    // IPv4 compact peers (6 bytes each). VALUES6/SEARCH_DONE are ignored: we run
    // IPv4-only, and completion is informational (the search re-issues on a timer).
    if (event == DHT_EVENT_VALUES) mgr->onValues(reinterpret_cast<const uint8_t*>(data), data_len);
}

// ---- DhtManager -------------------------------------------------------------

DhtManager::DhtManager() = default;
DhtManager::~DhtManager() { stop(); }

bool DhtManager::start(PeersFn peersFn) {
    if (active_) return true;
    if (g_dht != nullptr) {
        logWarn("dht: another DHT instance is already active — DHT disabled for this engine");
        return false;
    }
    if (!udp_.open()) {
        logWarn("dht: could not open UDP socket — DHT disabled");
        return false;
    }

    uint8_t nodeId[20];
    if (!fillCryptoRandom(nodeId, sizeof(nodeId))) {
        // Should never happen; derive a well-distributed id from clock+address so we
        // never hand jech/dht a zero/degenerate id (README: "SHA-1 of something").
        char material[32];
        int m = snprintf(material, sizeof(material), "%lld:%p", (long long)nowMs(), (void*)this);
        Sha1Digest d = sha1(material, m > 0 ? (size_t)m : 0);
        std::memcpy(nodeId, d.data(), sizeof(nodeId));
    }
    // Seed libc random() for jech/dht's internal timing jitter (not id/token — those
    // use dht_random_bytes). Harmless global state; keeps query timing non-degenerate.
    srandom((unsigned)(nowMs() ^ (int64_t)(uintptr_t)this));

    peersFn_ = std::move(peersFn);
    g_dht = this;  // route dht_sendto here before any dht_* call can send

    const unsigned char version[4] = {'G', 'M', 0x00, 0x01};  // DHT client version tag
    int rc = dht_init((int)udp_.handle(), -1, nodeId, version);
    if (rc < 0) {
        logWarn("dht: dht_init failed (errno %d) — DHT disabled", errno);
        g_dht = nullptr;
        peersFn_ = nullptr;
        udp_.close();
        return false;
    }
    active_ = true;
    // Opt-in KRPC tracing: jech/dht writes a full protocol trace to dht_debug when
    // set. Off unless DHT_DEBUG is a non-empty, non-"0" value (zero cost otherwise).
    // DHT is hard to debug blind; this is the same hook jech/dht's own tools use.
    if (const char* dbg = std::getenv("DHT_DEBUG"))
        if (dbg[0] && dbg[0] != '0') dht_debug = stderr;
    nextPeriodicMs_ = nowMs();  // pump on the first loop iteration
    logInfo("dht: BEP-5 mainline DHT active (node id %02x%02x…%02x%02x, dedicated UDP socket)", nodeId[0], nodeId[1],
        nodeId[18], nodeId[19]);
    return true;
}

void DhtManager::stop() {
    // Runs single-threaded: at close() the engine loop has already joined, so no
    // concurrent dht_* call can be in flight. dht_uninit only frees internal tables
    // (no sends, no socket ops), so this cannot crash — the historical "DHT crashes
    // on exit" was a concurrency/teardown-order bug, avoided by this discipline.
    if (active_) {
        dht_uninit();
        active_ = false;
    }
    g_dht = nullptr;  // stop routing dht_sendto here
    peersFn_ = nullptr;
    udp_.close();  // after dht_uninit (order-safe even though uninit does no I/O)
    haveSearch_ = false;
    std::lock_guard<std::mutex> lk(bootstrapMutex_);
    pendingBootstrap_.clear();
    bootstrapDone_ = false;
}

void DhtManager::resolveBootstrap() {
    if (!active_) return;
    {
        std::lock_guard<std::mutex> lk(bootstrapMutex_);
        if (bootstrapDone_) return;
    }
    // The canonical mainline bootstrap nodes (same set Transmission ships).
    static const struct {
        const char* host;
        uint16_t port;
    } kNodes[] = {
        {"router.bittorrent.com", 6881},
        {"dht.transmissionbt.com", 6881},
        {"router.utorrent.com", 6881},
    };
    std::vector<PeerAddr> resolved;
    for (const auto& n : kNodes) {
        auto ips = net::resolve(n.host);  // blocking DNS — safe: off the loop thread
        if (ips.empty()) {
            logWarn("dht: bootstrap resolve failed: %s", n.host);
            continue;
        }
        resolved.push_back({ips.front(), n.port});
        logInfo("dht: bootstrap %s -> %s:%u", n.host, ips.front().c_str(), n.port);
    }
    std::lock_guard<std::mutex> lk(bootstrapMutex_);
    for (auto& r : resolved) pendingBootstrap_.push_back(r);
    if (!resolved.empty()) bootstrapDone_ = true;  // leave open to retry next round if all failed
}

void DhtManager::search(const InfoHash& infoHash) {
    if (!active_) return;
    searchHash_ = infoHash;
    haveSearch_ = true;
    lastSearchMs_ = nowMs();
    // port = 0: search only, do not announce ourselves (we are a leech and ephemeral).
    int rc = dht_search(infoHash.data(), 0, AF_INET, &gmcaDhtEvent, this);
    if (rc < 0)
        logWarn("dht: dht_search failed (errno %d)", errno);
    else
        logInfo("dht: search %s (%s)", toHex(infoHash).c_str(), rc == 1 ? "new" : "refreshed");
}

void DhtManager::serviceReadable() {
    if (!active_) return;
    uint8_t buf[2048];  // KRPC datagrams are well under the UDP MTU
    std::string ip;
    uint16_t port = 0;
    for (;;) {
        int n = udp_.recvFrom(buf, sizeof(buf) - 1, &ip, &port);
        if (n <= 0) break;  // 0 = would-block, -1 = error
        buf[n] = 0;         // NUL-terminate as cheap insurance for the bencode scanners
        pump(buf, (size_t)n, ip, port);
    }
}

void DhtManager::periodic(int64_t now) {
    if (!active_) return;

    // Flush bootstrap endpoints resolved off-loop (resolveBootstrap). dht_ping_node
    // must run on the loop thread — it sends via dht_sendto.
    std::vector<PeerAddr> boot;
    {
        std::lock_guard<std::mutex> lk(bootstrapMutex_);
        boot.swap(pendingBootstrap_);
    }
    for (const auto& b : boot) {
        sockaddr_in sa;
        if (buildSockAddr(b.host, b.port, &sa)) {
            dht_ping_node(reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            logDebug("dht: ping bootstrap %s:%u", b.host.c_str(), b.port);
        }
    }

    // Keep the search fresh (jech merges a re-issue with the in-progress search).
    if (haveSearch_ && now - lastSearchMs_ > 120000) search(searchHash_);

    // Pump timers on the cadence jech asked for, or right after bootstrap pings.
    if (now >= nextPeriodicMs_ || !boot.empty()) pump(nullptr, 0, std::string(), 0);
}

int DhtManager::nodeCount() const {
    if (!active_) return 0;
    int good = 0, dubious = 0, cached = 0, incoming = 0;
    dht_nodes(AF_INET, &good, &dubious, &cached, &incoming);
    return good + dubious;
}

void DhtManager::onValues(const uint8_t* data, size_t len) {
    if (!data || !peersFn_) return;
    std::vector<PeerAddr> peers;
    for (size_t i = 0; i + 6 <= len; i += 6) {
        char ip[16];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", data[i], data[i + 1], data[i + 2], data[i + 3]);
        uint16_t port = (uint16_t)((data[i + 4] << 8) | data[i + 5]);
        if (port) peers.push_back({ip, port});
    }
    if (peers.empty()) return;
    logInfo("dht: search yielded %d peer(s)", (int)peers.size());
    peersFn_(peers);
}

void DhtManager::pump(const uint8_t* buf, size_t len, const std::string& fromIp, uint16_t fromPort) {
    time_t tosleep = 0;
    sockaddr_in sa;
    if (buf && len > 0 && !fromIp.empty() && buildSockAddr(fromIp, fromPort, &sa))
        dht_periodic(buf, len, reinterpret_cast<sockaddr*>(&sa), sizeof(sa), &tosleep, &gmcaDhtEvent, this);
    else
        dht_periodic(nullptr, 0, nullptr, 0, &tosleep, &gmcaDhtEvent, this);
    nextPeriodicMs_ = nowMs() + (int64_t)tosleep * 1000;
}

}  // namespace torrent
