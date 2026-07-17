/*
    GMCA — Tracker clients (see torrent/tracker.hpp).
*/

#include "torrent/tracker.hpp"

#include <sys/socket.h>  // AF_INET (POSIX puts it here; glibc leaks it via arpa/inet.h, newlib/libnx does not)
#include <arpa/inet.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "torrent/bencode.hpp"
#include "torrent/http_client.hpp"
#include "torrent/log.hpp"
#include "torrent/socket.hpp"
#include "torrent/util.hpp"

namespace torrent {

namespace {

// Parse "scheme://host[:port][/path]" into pieces (path incl. leading '/').
struct Url {
    std::string scheme, host, path;
    uint16_t port = 0;
};
bool parseUrl(const std::string& url, Url& out) {
    size_t s = url.find("://");
    if (s == std::string::npos) return false;
    out.scheme = url.substr(0, s);
    size_t hostStart = s + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string hostport =
        url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = (uint16_t)std::stoi(hostport.substr(colon + 1));
    } else {
        out.host = hostport;
        out.port = (out.scheme == "https") ? 443 : (out.scheme == "http" ? 80 : 0);
    }
    return !out.host.empty();
}

std::vector<PeerAddr> parseCompactPeers(const std::string& blob) {
    std::vector<PeerAddr> peers;
    for (size_t i = 0; i + 6 <= blob.size(); i += 6) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(blob.data() + i);
        char ip[INET_ADDRSTRLEN];
        struct in_addr a;
        std::memcpy(&a, p, 4);
        if (!inet_ntop(AF_INET, &a, ip, sizeof(ip))) continue;
        uint16_t port = (uint16_t)((p[4] << 8) | p[5]);
        if (port) peers.push_back({ip, port});
    }
    return peers;
}

// ---- HTTP tracker (BEP-3 / BEP-23) ----
AnnounceResult announceHttp(const std::string& url, const AnnounceContext& ctx, int timeoutMs) {
    AnnounceResult r;
    std::string ih(reinterpret_cast<const char*>(ctx.infoHash.data()), ctx.infoHash.size());
    const char* evt = ctx.event == 1 ? "completed" : ctx.event == 2 ? "started" : ctx.event == 3 ? "stopped" : "";
    std::string full = url;
    full += (url.find('?') == std::string::npos) ? '?' : '&';
    full += "info_hash=" + http::urlEncode(ih);
    full += "&peer_id=" + http::urlEncode(ctx.peerId);
    full += "&port=" + std::to_string(ctx.port);
    full += "&uploaded=" + std::to_string(ctx.uploaded);
    full += "&downloaded=" + std::to_string(ctx.downloaded);
    full += "&left=" + std::to_string(ctx.left);
    full += "&compact=1&numwant=" + std::to_string(ctx.numWant);
    if (*evt) full += std::string("&event=") + evt;

    http::Response resp = http::get(full, timeoutMs);
    if (!resp.ok()) {
        r.error = resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error;
        return r;
    }
    bencode::Value v;
    if (!bencode::decode(resp.body, v) || !v.isDict()) {
        r.error = "malformed tracker response";
        return r;
    }
    const bencode::Value* fail = v.find("failure reason");
    if (fail && fail->isStr()) {
        r.error = fail->s;
        return r;
    }
    r.intervalSec = (int)v.intAt("interval", 1800);
    const bencode::Value* peers = v.find("peers");
    if (peers) {
        if (peers->isStr()) {
            r.peers = parseCompactPeers(peers->s);
        } else if (peers->isList()) {
            for (const auto& p : peers->list) {
                if (!p.isDict()) continue;
                std::string ip = p.strAt("ip");
                int port = (int)p.intAt("port");
                if (!ip.empty() && port) r.peers.push_back({ip, (uint16_t)port});
            }
        }
    }
    r.ok = true;
    return r;
}

// ---- UDP tracker (BEP-15) ----
uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// BEP-15 is over unreliable UDP, so every request must be retransmitted on
// timeout. We send `req`, await a datagram whose (action, transaction_id) matches
// the expected pair (or an error, action 3), and retransmit with doubling backoff
// until the overall `budgetMs` is spent. Stray/late datagrams (wrong txid) are
// skipped rather than treated as failures. The reference spec uses a 15·2^n s
// schedule; a streaming session cannot wait minutes, so we start at 1 s, double,
// and cap — trading the long tail for responsiveness. Returns bytes received, or
// 0 on give-up / hard socket error.
int udpTransact(net::UdpSocket& s, const std::string& ip, uint16_t port, const void* req, size_t reqLen,
    uint8_t* resp, int respCap, uint32_t expectAction, uint32_t expectTxid, int budgetMs) {
    int64_t deadline = nowMs() + budgetMs;
    int perTry = 1000;  // ms; doubles each retransmission, capped below
    while (nowMs() < deadline) {
        if (s.sendTo(ip, port, req, reqLen) < 0) return 0;  // hard error (would-block returns 0 and still waits)
        int64_t waitUntil = nowMs() + std::min<int64_t>(perTry, deadline - nowMs());
        std::vector<net::PollItem> items{{s.handle(), true, false, false, false, false}};
        while (nowMs() < waitUntil) {
            int step = (int)(waitUntil - nowMs());
            if (step <= 0) break;
            int pr = net::poll(items, step > 500 ? 500 : step);
            if (pr > 0 && items[0].readable) {
                int got = s.recvFrom(resp, respCap);
                if (got < 0) return 0;
                if (got >= 8) {
                    uint32_t act = be32(resp);
                    uint32_t tx = be32(resp + 4);
                    if (tx == expectTxid && (act == expectAction || act == 3)) return got;
                    // else: stray/old datagram — keep waiting within this window
                }
            }
        }
        perTry = std::min(perTry * 2, 4000);
    }
    return 0;
}

AnnounceResult announceUdp(const Url& u, const AnnounceContext& ctx, int timeoutMs) {
    AnnounceResult r;
    auto ips = net::resolve(u.host);
    if (ips.empty()) {
        r.error = "udp resolve failed: " + u.host;
        return r;
    }
    std::string ip = ips.front();
    net::UdpSocket sock;
    if (!sock.open()) {
        r.error = "udp socket open failed";
        return r;
    }
    // Split the caller's budget between the two round-trips.
    int budget = std::max(2000, timeoutMs / 2);

    // 1) connect request (16 bytes). transaction_id is fresh random per BEP-15.
    uint32_t txid = (uint32_t)((nowMs() & 0xFFFFFFFF) ^ (uintptr_t)&r);
    uint8_t req[16];
    uint64_t protoId = 0x41727101980ULL;  // magic connection id for the connect action
    for (int i = 0; i < 8; i++) req[i] = (uint8_t)(protoId >> (56 - 8 * i));
    req[8] = req[9] = req[10] = req[11] = 0;  // action 0 = connect
    req[12] = (uint8_t)(txid >> 24);
    req[13] = (uint8_t)(txid >> 16);
    req[14] = (uint8_t)(txid >> 8);
    req[15] = (uint8_t)txid;

    uint8_t resp[2048];
    int got = udpTransact(sock, ip, u.port, req, sizeof(req), resp, sizeof(resp), /*action*/ 0, txid, budget);
    if (got < 16) {
        r.error = "udp connect timeout";
        return r;
    }
    if (be32(resp) == 3) {  // error action
        r.error = "udp tracker error (connect): " + std::string(reinterpret_cast<char*>(resp + 8), got - 8);
        return r;
    }
    uint8_t connId[8];
    std::memcpy(connId, resp + 8, 8);

    // 2) announce request (98 bytes)
    std::string a;
    a.append(reinterpret_cast<const char*>(connId), 8);
    auto put32 = [&](uint32_t v) {
        a += (char)(v >> 24);
        a += (char)(v >> 16);
        a += (char)(v >> 8);
        a += (char)v;
    };
    auto put64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) a += (char)(v >> (56 - 8 * i));
    };
    txid ^= 0x55AA;  // distinct transaction_id for the announce
    put32(1);        // action 1 = announce
    put32(txid);
    a.append(reinterpret_cast<const char*>(ctx.infoHash.data()), 20);
    a.append(ctx.peerId, 0, 20);
    put64((uint64_t)ctx.downloaded);
    put64((uint64_t)ctx.left);
    put64((uint64_t)ctx.uploaded);
    put32((uint32_t)ctx.event);
    put32(0);                             // ip (0 = tracker uses the source address)
    put32((uint32_t)(nowMs() & 0xFFFF));  // key
    put32((uint32_t)ctx.numWant);         // num_want
    a += (char)(ctx.port >> 8);
    a += (char)(ctx.port & 0xFF);

    got = udpTransact(sock, ip, u.port, a.data(), a.size(), resp, sizeof(resp), /*action*/ 1, txid, budget);
    if (got < 8) {
        r.error = "udp announce timeout";
        return r;
    }
    if (be32(resp) == 3) {  // error action
        r.error = "udp tracker error: " + std::string(reinterpret_cast<char*>(resp + 8), got - 8);
        return r;
    }
    if (got < 20) {
        r.error = "udp announce short response";
        return r;
    }
    r.intervalSec = (int)be32(resp + 8);  // [8..11]=interval, [12..15]=leechers, [16..19]=seeders
    std::string peerBlob(reinterpret_cast<char*>(resp + 20), got - 20);
    r.peers = parseCompactPeers(peerBlob);
    r.ok = true;
    return r;
}

}  // namespace

AnnounceResult announce(const std::string& trackerUrl, const AnnounceContext& ctx, int timeoutMs) {
    Url u;
    if (!parseUrl(trackerUrl, u)) {
        AnnounceResult r;
        r.error = "bad tracker url: " + trackerUrl;
        return r;
    }
    if (u.scheme == "udp") return announceUdp(u, ctx, timeoutMs);
    if (u.scheme == "http" || u.scheme == "https") return announceHttp(trackerUrl, ctx, timeoutMs);
    AnnounceResult r;
    r.error = "unsupported tracker scheme: " + u.scheme;
    return r;
}

}  // namespace torrent
