/*
    GMCA — MSE/PE outgoing handshake implementation (see torrent/mse.hpp).
*/

#include "torrent/mse.hpp"

#include <cstring>

#include "torrent/sha1.hpp"
#include "torrent/util.hpp"

namespace torrent {

namespace {
constexpr int kVcLen = 8;           // verification constant: 8 zero bytes
constexpr int kMaxPad = 512;        // PadA/B/C/D length bound (spec)
constexpr int kRc4Discard = 1024;   // shed the RC4 weak-key prefix
constexpr size_t kPrivLen = 20;     // 160-bit DH private exponent (ample for MSE)

constexpr uint32_t kCryptoPlaintext = 0x01;
constexpr uint32_t kCryptoRc4 = 0x02;

void putBE16(std::string& s, uint16_t v) {
    s += (char)(v >> 8);
    s += (char)(v & 0xFF);
}
void putBE32(std::string& s, uint32_t v) {
    s += (char)(v >> 24);
    s += (char)(v >> 16);
    s += (char)(v >> 8);
    s += (char)v;
}
uint16_t getBE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t getBE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// SHA-1 over the concatenation of up to three byte ranges.
Sha1Digest sha1cat(const void* a, size_t la, const void* b = nullptr, size_t lb = 0, const void* c = nullptr,
    size_t lc = 0) {
    std::string s;
    s.append(reinterpret_cast<const char*>(a), la);
    if (b) s.append(reinterpret_cast<const char*>(b), lb);
    if (c) s.append(reinterpret_cast<const char*>(c), lc);
    return sha1(s.data(), s.size());
}
}  // namespace

MseHandshake::MseHandshake(const InfoHash& infoHash, std::string ia, bool requireRc4)
    : infoHash_(infoHash), ia_(std::move(ia)), requireRc4_(requireRc4) {}

std::string MseHandshake::firstFlight() {
    // Random private exponent, high bit set so it stays a large 160-bit value.
    priv_ = randomBytes(kPrivLen);
    priv_[0] = (char)((uint8_t)priv_[0] | 0x80);

    uint8_t ya[mse::kKeyLen];
    mse::dhPublic(reinterpret_cast<const uint8_t*>(priv_.data()), priv_.size(), ya);

    std::string out(reinterpret_cast<const char*>(ya), mse::kKeyLen);
    // PadA: 0..512 random bytes. A small pad is enough to vary the DH field length
    // on the wire; the peer skips it by searching for our req1 hash.
    out += randomBytes((size_t)(nowMs() % 16));
    return out;
}

void MseHandshake::deriveKeys(const uint8_t secret[mse::kKeyLen]) {
    // KeyA = SHA1("keyA", S, SKEY); KeyB = SHA1("keyB", S, SKEY).
    Sha1Digest ka = sha1cat("keyA", 4, secret, mse::kKeyLen, infoHash_.data(), infoHash_.size());
    Sha1Digest kb = sha1cat("keyB", 4, secret, mse::kKeyLen, infoHash_.data(), infoHash_.size());
    encA_.init(ka.data(), ka.size());
    encA_.discard(kRc4Discard);
    decB_.init(kb.data(), kb.size());
    decB_.discard(kRc4Discard);

    // Expected encrypted VC = RC4(KeyB) applied to 8 zero bytes, computed on a COPY
    // so the real decB_ stays pristine for the actual step-4 decryption.
    Rc4 probe = decB_;
    uint8_t vc[kVcLen] = {0};
    probe.process(vc, kVcLen);
    vcPattern_.assign(reinterpret_cast<const char*>(vc), kVcLen);
}

std::string MseHandshake::buildStep3(const uint8_t secret[mse::kKeyLen]) {
    std::string out;
    // HASH('req1', S)
    Sha1Digest req1 = sha1cat("req1", 4, secret, mse::kKeyLen);
    out.append(reinterpret_cast<const char*>(req1.data()), req1.size());
    // HASH('req2', SKEY) xor HASH('req3', S)
    Sha1Digest req2 = sha1cat("req2", 4, infoHash_.data(), infoHash_.size());
    Sha1Digest req3 = sha1cat("req3", 4, secret, mse::kKeyLen);
    for (size_t i = 0; i < req2.size(); i++) out += (char)(req2[i] ^ req3[i]);

    // Encrypted with KeyA: VC | crypto_provide | len(PadC) | PadC | len(IA) | IA.
    std::string enc;
    enc.append(kVcLen, '\0');  // VC
    putBE32(enc, requireRc4_ ? kCryptoRc4 : (kCryptoPlaintext | kCryptoRc4));
    putBE16(enc, 0);  // len(PadC) = 0 (PadC empty)
    putBE16(enc, (uint16_t)ia_.size());
    enc += ia_;
    encA_.process(reinterpret_cast<uint8_t*>(enc.data()), enc.size());
    out += enc;
    return out;
}

MseHandshake::Status MseHandshake::feed(const uint8_t* data, size_t len, std::string& toSend, std::string& appData) {
    rx_.append(reinterpret_cast<const char*>(data), len);

    // Step 2 -> 3: parse peer public key Yb, derive keys, emit step 3.
    if (phase_ == Phase::ExpectYb) {
        if (rx_.size() < (size_t)mse::kKeyLen) return Status::NeedMore;
        uint8_t s[mse::kKeyLen];
        mse::dhSecret(reinterpret_cast<const uint8_t*>(rx_.data()), reinterpret_cast<const uint8_t*>(priv_.data()),
            priv_.size(), s);
        deriveKeys(s);
        if (!step3Sent_) {
            toSend += buildStep3(s);
            step3Sent_ = true;
        }
        rx_.erase(0, mse::kKeyLen);
        phase_ = Phase::SyncVc;
    }

    // Skip PadB by locating the encrypted VC in the peer's stream.
    if (phase_ == Phase::SyncVc) {
        size_t pos = rx_.find(vcPattern_);
        if (pos == std::string::npos) {
            // Bounded search: VC must appear within PadB's max length.
            if (rx_.size() > (size_t)(kMaxPad + kVcLen + 16)) {
                phase_ = Phase::Failed;
                return Status::Failed;
            }
            return Status::NeedMore;
        }
        rx_.erase(0, pos);  // drop PadB; rx_ now begins at the encrypted VC
        phase_ = Phase::Header;
    }

    // Decrypt step-4 header (VC | crypto_select | len(PadD)) then skip PadD.
    if (phase_ == Phase::Header) {
        if (!hdrDecoded_) {
            const int hdrLen = kVcLen + 4 + 2;
            if (rx_.size() < (size_t)hdrLen) return Status::NeedMore;
            uint8_t hdr[kVcLen + 4 + 2];
            std::memcpy(hdr, rx_.data(), hdrLen);
            decB_.process(hdr, hdrLen);  // advances decB_ to the PadD boundary
            for (int i = 0; i < kVcLen; i++) {
                if (hdr[i] != 0) {  // VC must decrypt to zero
                    phase_ = Phase::Failed;
                    return Status::Failed;
                }
            }
            uint32_t sel = getBE32(hdr + kVcLen);
            uint16_t padD = getBE16(hdr + kVcLen + 4);
            rc4_ = (sel & kCryptoRc4) != 0;
            if (sel != kCryptoPlaintext && sel != kCryptoRc4) {  // peer picked nothing valid
                phase_ = Phase::Failed;
                return Status::Failed;
            }
            padDRemaining_ = padD;
            rx_.erase(0, hdrLen);
            hdrDecoded_ = true;
        }
        while (padDRemaining_ > 0) {
            if (rx_.empty()) return Status::NeedMore;
            size_t take = std::min((size_t)padDRemaining_, rx_.size());
            decB_.process(reinterpret_cast<uint8_t*>(&rx_[0]), take);  // advance past PadD
            rx_.erase(0, take);
            padDRemaining_ -= (int)take;
        }
        phase_ = Phase::Body;
    }

    // Remaining bytes are the peer's application payload (start of its BT stream).
    if (phase_ == Phase::Body) {
        if (!rx_.empty()) {
            if (rc4_) decB_.process(reinterpret_cast<uint8_t*>(&rx_[0]), rx_.size());
            appData += rx_;
            rx_.clear();
        }
        phase_ = Phase::Done;
        return Status::Done;
    }

    return Status::NeedMore;
}

}  // namespace torrent
