/*
    GMCA — BitTorrent peer connection implementation (see torrent/peer.hpp).
*/

#include "torrent/peer.hpp"

#include <sys/socket.h>  // AF_INET (POSIX puts it here; glibc leaks it via arpa/inet.h, newlib/libnx does not)
#include <arpa/inet.h>   // inet_ntop for compact PEX peers

#include <algorithm>
#include <cstring>

#include "torrent/bencode.hpp"
#include "torrent/log.hpp"
#include "torrent/storage.hpp"
#include "torrent/util.hpp"

namespace torrent {

namespace {
constexpr int kMsgChoke = 0;
constexpr int kMsgUnchoke = 1;
constexpr int kMsgInterested = 2;
constexpr int kMsgHave = 4;
constexpr int kMsgBitfield = 5;
constexpr int kMsgRequest = 6;
constexpr int kMsgPiece = 7;
constexpr int kMsgExtended = 20;
constexpr int kExtHandshakeId = 0;
constexpr int64_t kConnectTimeoutMs = 8000;
constexpr int64_t kKeepAliveMs = 110000;
constexpr int kMaxMessageLen = 1 << 20;  // 1 MiB guard (piece blocks are 16 KiB)

void put32(std::string& s, uint32_t v) {
    s += (char)(v >> 24);
    s += (char)(v >> 16);
    s += (char)(v >> 8);
    s += (char)v;
}
uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
}  // namespace

PeerConnection::PeerConnection(
    const PeerAddr& addr, PeerHost* host, std::unique_ptr<PeerTransport> transport, Encryption enc)
    : addr_(addr), host_(host), transport_(std::move(transport)), enc_(enc) {}

bool PeerConnection::startConnect() {
    if (!transport_) return false;
    buildHandshake();  // fills btHandshake_ (the MSE "IA" / the plaintext handshake)
    if (enc_ == Encryption::Plaintext) {
        queue(btHandshake_);  // legacy clear-text handshake, flushed once connected
    } else {
        // MSE/PE: queue step 1 (DH pubkey + pad) RAW; the BitTorrent handshake is
        // carried encrypted as the IA of step 3 once we have the peer's key. Runs
        // over µTP exactly as over TCP — the transport is a plain byte stream.
        mse_ = std::make_unique<MseHandshake>(host_->hostInfoHash(), btHandshake_, enc_ == Encryption::Forced);
        outbuf_ += mse_->firstFlight();
    }
    // The handshake bytes are already queued in outbuf_ and are flushed once the
    // transport reports connected (writable) — TCP connect, or the µTP SYN-ACK.
    if (!transport_->startConnect(addr_)) {
        transport_->close();
        return false;
    }
    state_ = State::Connecting;
    connectStartMs_ = nowMs();
    lastSendMs_ = connectStartMs_;
    return true;
}

void PeerConnection::close() {
    if (transport_) transport_->close();
    state_ = State::Closed;
}

bool PeerConnection::wantWrite() const {
    if (state_ == State::Connecting) return true;
    return outSent_ < outbuf_.size();
}

void PeerConnection::queue(const std::string& bytes) {
    if (sendEncrypted_) {
        // Post-MSE RC4 wire: encrypt the BitTorrent message before it hits outbuf_.
        // (MSE handshake bytes bypass this — they carry their own encryption and are
        // appended to outbuf_ directly while sendEncrypted_ is still false.)
        std::string enc = bytes;
        sendCipher_.process(reinterpret_cast<uint8_t*>(&enc[0]), enc.size());
        outbuf_ += enc;
    } else {
        outbuf_ += bytes;
    }
}

void PeerConnection::flush() {
    if (state_ == State::Connecting || state_ == State::Closed) return;
    while (outSent_ < outbuf_.size()) {
        int n = transport_->send(outbuf_.data() + outSent_, outbuf_.size() - outSent_);
        if (n > 0) {
            outSent_ += (size_t)n;
            lastSendMs_ = nowMs();
        } else if (n == 0) {
            break;  // would-block; retry on next writable
        } else {
            close();
            return;
        }
    }
    if (outSent_ == outbuf_.size() && outSent_ > 0) {
        outbuf_.clear();
        outSent_ = 0;
    }
}

void PeerConnection::buildHandshake() {
    static const std::string pstr = "BitTorrent protocol";
    std::string h;
    h += (char)pstr.size();
    h += pstr;
    char reserved[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    reserved[5] = 0x10;  // BEP-10 extension protocol
    h.append(reserved, 8);
    const InfoHash& ih = host_->hostInfoHash();
    h.append(reinterpret_cast<const char*>(ih.data()), ih.size());
    h += host_->hostPeerId();
    btHandshake_ = h;  // sent as-is (plaintext) or as the MSE IA (encrypted)
}

void PeerConnection::onWritable() {
    if (state_ == State::Connecting) {
        int r = transport_->checkConnected();
        if (r == 1) {
            // MSE peers enter the crypto handshake (step 1 already queued); plaintext
            // peers go straight to waiting for the BitTorrent handshake reply.
            state_ = mse_ ? State::MseHandshake : State::HandshakeWait;
        } else if (r < 0) {
            close();
            return;
        } else {
            return;
        }
    }
    flush();
}

void PeerConnection::onReadable() {
    char buf[65536];
    std::string raw;
    for (;;) {
        int n = transport_->recv(buf, sizeof(buf));
        if (n > 0) {
            raw.append(buf, (size_t)n);
            if (raw.size() > (size_t)kMaxMessageLen * 4) {  // runaway guard
                close();
                return;
            }
        } else if (n == 0) {
            break;  // would-block
        } else {
            close();
            return;
        }
    }
    if (raw.empty()) return;

    if (state_ == State::MseHandshake) {
        std::string toSend, appData;
        MseHandshake::Status st =
            mse_->feed(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), toSend, appData);
        if (!toSend.empty()) {
            outbuf_ += toSend;  // step 3 — carries its own encryption, send RAW
            flush();
        }
        if (st == MseHandshake::Status::Failed) {
            logDebug("peer %s: MSE handshake failed", addr_.str().c_str());
            close();  // engine may retry this peer in plaintext (Prefer policy)
            return;
        }
        if (st == MseHandshake::Status::NeedMore) return;
        onMseComplete(appData);  // Done: install ciphers, feed leftover to the BT parser
        return;
    }

    // Plaintext, or post-MSE RC4: decrypt in stream order as bytes arrive.
    if (recvEncrypted_) recvCipher_.process(reinterpret_cast<uint8_t*>(&raw[0]), raw.size());
    inbuf_ += raw;
    parseInbound();
}

void PeerConnection::onMseComplete(const std::string& appData) {
    if (mse_->rc4()) {
        // Lift the advanced RC4 states out of the handshake; the wire stays RC4.
        sendCipher_ = mse_->sendCipher();
        recvCipher_ = mse_->recvCipher();
        sendEncrypted_ = recvEncrypted_ = true;
        logDebug("peer %s: MSE negotiated RC4", addr_.str().c_str());
    } else {
        logDebug("peer %s: MSE negotiated plaintext payload", addr_.str().c_str());
    }
    mse_.reset();
    state_ = State::HandshakeWait;
    if (!appData.empty()) inbuf_ += appData;  // already decrypted by the handshake
    parseInbound();
}

void PeerConnection::parseInbound() {
    if (state_ == State::Closed) return;
    if (!handshakeParsed_) {
        if (!parseHandshake()) return;  // need more bytes, or closed
    }
    const uint8_t* d = reinterpret_cast<const uint8_t*>(inbuf_.data());
    size_t avail = inbuf_.size();
    size_t pos = 0;
    while (avail - pos >= 4) {
        uint32_t len = get32(d + pos);
        if (len == 0) {  // keep-alive
            pos += 4;
            continue;
        }
        if (len > (uint32_t)kMaxMessageLen) {
            close();
            return;
        }
        if (avail - pos < 4 + len) break;  // wait for the rest
        handleMessage(d + pos + 4, (int)len);
        if (state_ == State::Closed) return;
        pos += 4 + len;
    }
    if (pos > 0) inbuf_.erase(0, pos);
}

bool PeerConnection::parseHandshake() {
    if (inbuf_.size() < 1) return false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(inbuf_.data());
    int pstrlen = d[0];
    size_t need = 1 + (size_t)pstrlen + 8 + 20 + 20;
    if (inbuf_.size() < need) return false;
    if (pstrlen != 19) {  // only BitTorrent v1 wire
        close();
        return false;
    }
    const uint8_t* reserved = d + 1 + pstrlen;
    extSupported_ = (reserved[5] & 0x10) != 0;
    const uint8_t* ih = reserved + 8;
    const InfoHash& mine = host_->hostInfoHash();
    if (std::memcmp(ih, mine.data(), 20) != 0) {
        logWarn("peer %s: infohash mismatch, dropping", addr_.str().c_str());
        close();
        return false;
    }
    inbuf_.erase(0, need);
    handshakeParsed_ = true;
    state_ = State::Ready;

    if (extSupported_) sendExtendedHandshake();
    sendInterested();
    flush();
    host_->onPeerHandshake(this);
    return true;
}

void PeerConnection::sendExtendedHandshake() {
    bencode::Value m;
    m.type = bencode::Value::Type::Dict;
    bencode::Value utm;
    utm.type = bencode::Value::Type::Int;
    utm.i = kUtMetadataLocalId;
    bencode::Value utp;
    utp.type = bencode::Value::Type::Int;
    utp.i = kUtPexLocalId;
    m.dict["ut_metadata"] = utm;
    m.dict["ut_pex"] = utp;

    bencode::Value root;
    root.type = bencode::Value::Type::Dict;
    root.dict["m"] = m;
    bencode::Value ver;
    ver.type = bencode::Value::Type::Str;
    ver.s = "GMCA 0.1";
    root.dict["v"] = ver;

    std::string payload = bencode::encode(root);
    std::string msg;
    put32(msg, (uint32_t)(2 + payload.size()));  // id(1) + extId(1) + payload
    msg += (char)kMsgExtended;
    msg += (char)kExtHandshakeId;
    msg += payload;
    queue(msg);
}

void PeerConnection::sendInterested() {
    if (amInterested_) return;
    std::string msg;
    put32(msg, 1);
    msg += (char)kMsgInterested;
    queue(msg);
    amInterested_ = true;
    flush();
}

void PeerConnection::sendRequest(int piece, int32_t begin, int32_t length) {
    std::string msg;
    put32(msg, 13);
    msg += (char)kMsgRequest;
    put32(msg, (uint32_t)piece);
    put32(msg, (uint32_t)begin);
    put32(msg, (uint32_t)length);
    queue(msg);
    outstanding_++;
    flush();
}

void PeerConnection::sendMetadataRequest(int piece) {
    if (peerUtMetadataId_ == 0) return;
    bencode::Value req;
    req.type = bencode::Value::Type::Dict;
    bencode::Value t, p;
    t.type = bencode::Value::Type::Int;
    t.i = 0;  // msg_type 0 = request
    p.type = bencode::Value::Type::Int;
    p.i = piece;
    req.dict["msg_type"] = t;
    req.dict["piece"] = p;
    std::string payload = bencode::encode(req);
    std::string msg;
    put32(msg, (uint32_t)(2 + payload.size()));
    msg += (char)kMsgExtended;
    msg += (char)peerUtMetadataId_;
    msg += payload;
    queue(msg);
    flush();
}

void PeerConnection::sendPex(const std::vector<PeerAddr>& current) {
    if (peerUtPexId_ == 0 || state_ != State::Ready) return;

    auto compact = [](const PeerAddr& a, std::string& out) -> bool {
        struct in_addr in;
        if (inet_pton(AF_INET, a.host.c_str(), &in) != 1) return false;  // skip hostnames / non-IPv4
        out.append(reinterpret_cast<const char*>(&in), 4);
        out += (char)(a.port >> 8);
        out += (char)(a.port & 0xFF);
        return true;
    };

    // added = endpoints not yet advertised to this peer; dropped = previously
    // advertised endpoints no longer present (BEP-11 delta semantics).
    std::vector<std::string> wantKeys;
    std::string added, addedF, dropped;
    wantKeys.reserve(current.size());
    for (const auto& a : current) {
        std::string key = a.str();
        wantKeys.push_back(key);
        if (std::find(pexAdvertised_.begin(), pexAdvertised_.end(), key) == pexAdvertised_.end()) {
            if (compact(a, added)) addedF += (char)0x00;  // no peer flags advertised
        }
    }
    for (const auto& key : pexAdvertised_) {
        if (std::find(wantKeys.begin(), wantKeys.end(), key) != wantKeys.end()) continue;
        size_t c = key.rfind(':');
        if (c == std::string::npos) continue;
        compact({key.substr(0, c), (uint16_t)std::stoi(key.substr(c + 1))}, dropped);
    }
    if (added.empty() && dropped.empty()) return;  // nothing changed this round

    bencode::Value root;
    root.type = bencode::Value::Type::Dict;
    auto strVal = [](const std::string& s) {
        bencode::Value v;
        v.type = bencode::Value::Type::Str;
        v.s = s;
        return v;
    };
    if (!added.empty()) {
        root.dict["added"] = strVal(added);
        root.dict["added.f"] = strVal(addedF);
    }
    if (!dropped.empty()) root.dict["dropped"] = strVal(dropped);

    std::string payload = bencode::encode(root);
    std::string msg;
    put32(msg, (uint32_t)(2 + payload.size()));
    msg += (char)kMsgExtended;
    msg += (char)peerUtPexId_;
    msg += payload;
    queue(msg);
    flush();
    logDebug("peer %s: sent ut_pex (added %zu, dropped %zu)", addr_.str().c_str(), added.size() / 6,
        dropped.size() / 6);
    pexAdvertised_ = std::move(wantKeys);
}

void PeerConnection::applyHave(int index) {
    if (!peerHas_.empty()) {
        if (index >= 0 && index < (int)peerHas_.size()) peerHas_[index] = 1;
    } else {
        pendingHaves_.push_back(index);
    }
}

void PeerConnection::onMetadataReady(int numPieces) {
    if ((int)peerHas_.size() == numPieces) return;
    peerHas_.assign(numPieces, 0);
    if (!pendingBitfield_.empty()) {
        const uint8_t* bf = reinterpret_cast<const uint8_t*>(pendingBitfield_.data());
        for (int i = 0; i < numPieces; i++)
            if ((size_t)(i / 8) < pendingBitfield_.size() && ((bf[i / 8] >> (7 - (i % 8))) & 1)) peerHas_[i] = 1;
        pendingBitfield_.clear();
    }
    for (int idx : pendingHaves_)
        if (idx >= 0 && idx < numPieces) peerHas_[idx] = 1;
    pendingHaves_.clear();
}

void PeerConnection::handleMessage(const uint8_t* data, int len) {
    if (len < 1) return;
    int id = data[0];
    const uint8_t* payload = data + 1;
    int plen = len - 1;
    switch (id) {
    case kMsgChoke:
        peerChoking_ = true;
        break;
    case kMsgUnchoke:
        peerChoking_ = false;
        break;
    case kMsgHave:
        if (plen >= 4) applyHave((int)get32(payload));
        break;
    case kMsgBitfield:
        if (!peerHas_.empty()) {
            for (int i = 0; i < (int)peerHas_.size(); i++)
                if (i / 8 < plen && ((payload[i / 8] >> (7 - (i % 8))) & 1)) peerHas_[i] = 1;
        } else {
            pendingBitfield_.assign(reinterpret_cast<const char*>(payload), (size_t)plen);
        }
        break;
    case kMsgPiece: {
        if (plen < 8) break;
        int piece = (int)get32(payload);
        int32_t begin = (int32_t)get32(payload + 4);
        const uint8_t* block = payload + 8;
        int blen = plen - 8;
        if (outstanding_ > 0) outstanding_--;
        host_->onPeerBlock(this, piece, begin, block, blen);
        break;
    }
    case kMsgRequest:
        // leech-only: we never upload piece data. Silently ignore.
        break;
    case kMsgExtended:
        handleExtended(payload, plen);
        break;
    default:
        break;  // not-interested, cancel, port(DHT), etc. — ignored
    }
}

void PeerConnection::handleExtended(const uint8_t* data, int len) {
    if (len < 1) return;
    int extId = data[0];
    const uint8_t* body = data + 1;
    int blen = len - 1;

    if (extId == kExtHandshakeId) {
        bencode::Value v;
        size_t pos = 0;
        if (!bencode::decode(body, (size_t)blen, pos, v) || !v.isDict()) return;
        const bencode::Value* m = v.find("m");
        if (m && m->isDict()) {
            peerUtMetadataId_ = (int)m->intAt("ut_metadata", 0);
            peerUtPexId_ = (int)m->intAt("ut_pex", 0);
        }
        peerMetadataSize_ = v.intAt("metadata_size", 0);
        return;
    }

    if (extId == kUtMetadataLocalId) {
        // BEP-9: bencoded dict, then (for msg_type 1) the raw metadata bytes.
        bencode::Value v;
        size_t pos = 0;
        if (!bencode::decode(body, (size_t)blen, pos, v) || !v.isDict()) return;
        int msgType = (int)v.intAt("msg_type", -1);
        int piece = (int)v.intAt("piece", -1);
        if (msgType == 1 && piece >= 0) {  // data
            int64_t totalSize = v.intAt("total_size", peerMetadataSize_);
            const uint8_t* mdata = body + pos;
            int mlen = blen - (int)pos;
            if (mlen > 0) host_->onMetadataPiece(piece, totalSize, mdata, mlen);
        }
        return;
    }

    if (extId == kUtPexLocalId) {
        bencode::Value v;
        size_t pos = 0;
        if (!bencode::decode(body, (size_t)blen, pos, v) || !v.isDict()) return;
        const bencode::Value* added = v.find("added");  // compact IPv4 peers, 6 bytes each
        if (added && added->isStr()) {
            std::vector<PeerAddr> peers;
            const std::string& s = added->s;
            for (size_t i = 0; i + 6 <= s.size(); i += 6) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data() + i);
                char ip[INET_ADDRSTRLEN];
                struct in_addr a;
                std::memcpy(&a, p, 4);
                if (!inet_ntop(AF_INET, &a, ip, sizeof(ip))) continue;
                uint16_t port = (uint16_t)((p[4] << 8) | p[5]);
                if (port) peers.push_back({ip, port});
            }
            if (!peers.empty()) host_->onPexPeers(peers);
        }
        return;
    }
}

void PeerConnection::onTick(int64_t now) {
    if (state_ == State::Connecting && now - connectStartMs_ > kConnectTimeoutMs) {
        logDebug("peer %s: connect timeout", addr_.str().c_str());
        close();
        return;
    }
    if (state_ == State::Ready && now - lastSendMs_ > kKeepAliveMs) {
        std::string ka;
        put32(ka, 0);
        queue(ka);
        flush();
    }
}

}  // namespace torrent
