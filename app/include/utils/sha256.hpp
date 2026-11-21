#pragma once

/*
    SHA-256 of a downloaded update, checked against the `digest` GitHub gives
    for each release asset ("sha256:<hex>"). Plain C++ so every platform runs
    the same code and tests/test_sha256.cpp can exercise it.
*/

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace misc {

class Sha256 {
public:
    Sha256() { reset(); }

    void update(const void* data, size_t len) {
        auto* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len > 0) {
            size_t n = std::min(len, sizeof(block) - used);
            std::memcpy(block + used, p, n);
            used += n;
            p += n;
            len -= n;
            if (used == sizeof(block)) {
                compress(block);
                used = 0;
            }
        }
    }

    std::string hex() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (used != 56) update(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);

        std::string out;
        char buf[9];
        for (uint32_t v : h) {
            std::snprintf(buf, sizeof(buf), "%08x", v);
            out += buf;
        }
        reset();
        return out;
    }

private:
    uint32_t h[8];
    uint8_t block[64];
    size_t used;
    uint64_t total;

    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
            0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h, init, sizeof(h));
        used = 0;
        total = 0;
    }

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t* b) {
        static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c,
            0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += bb;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
};

inline std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    Sha256 sha;
    std::vector<char> buf(64 * 1024);  // off the stack: the console worker threads have small ones
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) sha.update(buf.data(), size_t(in.gcount()));
    return sha.hex();
}

/// Throws unless `digest` is "sha256:<hex>" and matches the file. A release
/// without a digest is refused too: the update must not install unchecked.
inline void verifyDigest(const std::string& path, const std::string& digest) {
    const std::string prefix = "sha256:";
    if (digest.compare(0, prefix.size(), prefix) != 0 || digest.size() != prefix.size() + 64)
        throw std::runtime_error("the release has no SHA-256 digest for this file");
    std::string expected = digest.substr(prefix.size());
    for (char& c : expected) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (sha256File(path) != expected) throw std::runtime_error("the downloaded file does not match the release digest");
}

}  // namespace misc
