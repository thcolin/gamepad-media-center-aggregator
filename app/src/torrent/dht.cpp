#include "torrent/dht.hpp"

// The sockaddr built here never reaches a platform call: jech/dht only hands it
// back to dht_sendto, which sends through net::UdpSocket.
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

// Last: dht.h expects FILE, struct sockaddr, size_t and time_t already declared.
#include "dht.h"

#if defined(TORRENT_SHA1_OPENSSL)
#include <openssl/rand.h>
#elif defined(TORRENT_SHA1_MBEDTLS)
#include <mbedtls/ctr_drbg.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace torrent {

namespace {

// For the C callbacks. Set before the engine loop starts, cleared after it joins.
DhtManager* g_dht = nullptr;

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

// Node id and jech/dht's token secret: must be unpredictable.
bool fillCryptoRandom(void* buf, size_t size) {
#if defined(TORRENT_SHA1_OPENSSL)
    return RAND_bytes(reinterpret_cast<unsigned char*>(buf), (int)size) == 1;
#elif defined(TORRENT_SHA1_MBEDTLS)
    // mbedtls' entropy module may have no source on console toolchains, so the
    // DRBG is seeded from osRandom mixed with a clock, a counter and an address.
    struct Seed {
        static int gather(void*, unsigned char* out, size_t len) {
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
    arc4random_buf(buf, size);
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
    osRandom(buf, size);
    return true;
#endif
}

}  // namespace

// Callbacks jech/dht leaves to the application. extern "C" keeps the C symbol
// names despite the enclosing namespace.
extern "C" {

// jech/dht never uses the fd itself, so sends go through the platform shim.
int dht_sendto(int /*sockfd*/, const void* buf, int len, int /*flags*/, const struct sockaddr* to, int /*tolen*/) {
    DhtManager* mgr = g_dht;
    if (!mgr || !to) {
        errno = EINVAL;
        return -1;
    }
    if (to->sa_family != AF_INET) {
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
    // A would-block counts as sent: jech/dht retries on its own timers.
    return len;
}

int dht_blacklisted(const struct sockaddr* /*sa*/, int /*salen*/) { return 0; }

// SHA-1 of v1||v2||v3, truncated or zero-padded to hash_size.
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

int dht_random_bytes(void* buf, size_t size) { return fillCryptoRandom(buf, size) ? (int)size : -1; }

}  // extern "C"

extern "C" void gmcaDhtEvent(void* closure, int event, const unsigned char* /*info_hash*/, const void* data,
    size_t data_len) {
    DhtManager* mgr = static_cast<DhtManager*>(closure);
    if (!mgr) return;
    // IPv4 only; SEARCH_DONE needs nothing since periodic() re-issues the search.
    if (event == DHT_EVENT_VALUES) mgr->onValues(reinterpret_cast<const uint8_t*>(data), data_len);
}

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
        // Never hand jech/dht a degenerate id.
        char material[32];
        int m = snprintf(material, sizeof(material), "%lld:%p", (long long)nowMs(), (void*)this);
        Sha1Digest d = sha1(material, m > 0 ? (size_t)m : 0);
        std::memcpy(nodeId, d.data(), sizeof(nodeId));
    }
    // jech/dht uses random() for timing jitter only.
    srandom((unsigned)(nowMs() ^ (int64_t)(uintptr_t)this));

    peersFn_ = std::move(peersFn);
    g_dht = this;  // before any dht_* call can send

    const unsigned char version[4] = {'G', 'M', 0x00, 0x01};
    int rc = dht_init((int)udp_.handle(), -1, nodeId, version);
    if (rc < 0) {
        logWarn("dht: dht_init failed (errno %d) — DHT disabled", errno);
        g_dht = nullptr;
        peersFn_ = nullptr;
        udp_.close();
        return false;
    }
    active_ = true;
    // DHT_DEBUG=1 makes jech/dht trace KRPC to stderr.
    if (const char* dbg = std::getenv("DHT_DEBUG"))
        if (dbg[0] && dbg[0] != '0') dht_debug = stderr;
    nextPeriodicMs_ = nowMs();
    logInfo("dht: BEP-5 mainline DHT active (node id %02x%02x…%02x%02x, dedicated UDP socket)", nodeId[0], nodeId[1],
        nodeId[18], nodeId[19]);
    return true;
}

void DhtManager::stop() {
    // The engine loop has joined: no dht_* call can run concurrently.
    if (active_) {
        dht_uninit();
        active_ = false;
    }
    g_dht = nullptr;
    peersFn_ = nullptr;
    udp_.close();
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
        auto ips = net::resolve(n.host);
        if (ips.empty()) {
            logWarn("dht: bootstrap resolve failed: %s", n.host);
            continue;
        }
        resolved.push_back({ips.front(), n.port});
        logInfo("dht: bootstrap %s -> %s:%u", n.host, ips.front().c_str(), n.port);
    }
    std::lock_guard<std::mutex> lk(bootstrapMutex_);
    for (auto& r : resolved) pendingBootstrap_.push_back(r);
    if (!resolved.empty()) bootstrapDone_ = true;  // all failed: retry next round
}

void DhtManager::search(const InfoHash& infoHash) {
    if (!active_) return;
    searchHash_ = infoHash;
    haveSearch_ = true;
    lastSearchMs_ = nowMs();
    // port 0: search without announcing, the engine does not seed.
    int rc = dht_search(infoHash.data(), 0, AF_INET, &gmcaDhtEvent, this);
    if (rc < 0)
        logWarn("dht: dht_search failed (errno %d)", errno);
    else
        logInfo("dht: search %s (%s)", toHex(infoHash).c_str(), rc == 1 ? "new" : "refreshed");
}

void DhtManager::serviceReadable() {
    if (!active_) return;
    uint8_t buf[2048];
    std::string ip;
    uint16_t port = 0;
    for (;;) {
        int n = udp_.recvFrom(buf, sizeof(buf) - 1, &ip, &port);
        if (n <= 0) break;
        buf[n] = 0;  // dht_periodic rejects a buffer that is not NUL-terminated
        pump(buf, (size_t)n, ip, port);
    }
}

void DhtManager::periodic(int64_t now) {
    if (!active_) return;

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

    if (haveSearch_ && now - lastSearchMs_ > 120000) search(searchHash_);

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
