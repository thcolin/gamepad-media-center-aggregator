/*
    GMCA — SHA-1 implementation dispatch (see torrent/sha1.hpp).
*/

#include "torrent/sha1.hpp"

#if defined(TORRENT_SHA1_OPENSSL)
#include <openssl/sha.h>
#elif defined(TORRENT_SHA1_MBEDTLS)
#include <mbedtls/sha1.h>
#elif defined(__APPLE__)
// CommonCrypto ships with macOS/iOS; no extra link flag needed. The SHA1 API is
// deprecated (SecTransform is the blessed replacement) but remains available and
// is the lightest way to get a hardware-friendly SHA-1 on desktop.
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>
#else
#define TORRENT_SHA1_BUILTIN 1
#endif

namespace torrent {

#if defined(TORRENT_SHA1_BUILTIN)
// -------- Bundled SHA-1 (public domain, Steve Reid's reference, compacted) -----
namespace {
struct Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

inline uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (buffer[i * 4] << 24) | (buffer[i * 4 + 1] << 16) | (buffer[i * 4 + 2] << 8) | (buffer[i * 4 + 3]);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void init(Ctx& c) {
    c.state[0] = 0x67452301;
    c.state[1] = 0xEFCDAB89;
    c.state[2] = 0x98BADCFE;
    c.state[3] = 0x10325476;
    c.state[4] = 0xC3D2E1F0;
    c.count = 0;
}

void update(Ctx& c, const uint8_t* data, size_t len) {
    size_t idx = (size_t)(c.count % 64);
    c.count += len;
    size_t part = 64 - idx;
    size_t i = 0;
    if (len >= part) {
        for (size_t k = 0; k < part; k++) c.buffer[idx + k] = data[k];
        transform(c.state, c.buffer);
        for (i = part; i + 63 < len; i += 64) transform(c.state, data + i);
        idx = 0;
    }
    for (; i < len; i++) c.buffer[idx++] = data[i];
}

void final(Ctx& c, uint8_t out[20]) {
    uint64_t bits = c.count * 8;
    uint8_t pad = 0x80;
    update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.count % 64 != 56) update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(c, lenb, 8);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(c.state[i / 4] >> (24 - 8 * (i % 4)));
}
}  // namespace
#endif

Sha1Digest sha1(const void* data, size_t len) {
    Sha1Digest out{};
#if defined(TORRENT_SHA1_OPENSSL)
    SHA1(reinterpret_cast<const unsigned char*>(data), len, out.data());
#elif defined(TORRENT_SHA1_MBEDTLS)
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(data), len, out.data());
#elif defined(__APPLE__)
    CC_SHA1(data, (CC_LONG)len, out.data());
#else
    Ctx c;
    init(c);
    update(c, reinterpret_cast<const uint8_t*>(data), len);
    final(c, out.data());
#endif
    return out;
}

std::string toHex(const Sha1Digest& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(40);
    for (uint8_t b : d) {
        s += h[b >> 4];
        s += h[b & 0xF];
    }
    return s;
}

}  // namespace torrent
