/*
    GMCA — portable crypto primitives for MSE/PE (see torrent/crypto.hpp).
*/

#include "torrent/crypto.hpp"

#include <cstring>
#include <utility>
#include <vector>

#if defined(TORRENT_SHA1_MBEDTLS)
#include <mbedtls/bignum.h>
#endif

namespace torrent {

// ---- RC4 (ARC4) -------------------------------------------------------------

void Rc4::init(const uint8_t* key, size_t len) {
    for (int i = 0; i < 256; i++) s_[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + s_[i] + key[i % len]);
        std::swap(s_[i], s_[j]);
    }
    i_ = 0;
    j_ = 0;
    ready_ = true;
}

void Rc4::discard(size_t n) {
    for (size_t k = 0; k < n; k++) {
        i_ = (uint8_t)(i_ + 1);
        j_ = (uint8_t)(j_ + s_[i_]);
        std::swap(s_[i_], s_[j_]);
    }
}

void Rc4::process(uint8_t* data, size_t len) {
    for (size_t k = 0; k < len; k++) {
        i_ = (uint8_t)(i_ + 1);
        j_ = (uint8_t)(j_ + s_[i_]);
        std::swap(s_[i_], s_[j_]);
        data[k] ^= s_[(uint8_t)(s_[i_] + s_[j_])];
    }
}

namespace mse {

// The MSE Diffie-Hellman group: generator g = 2 and this specific 768-bit prime
// P (Vuze "Message Stream Encryption" spec — note the distinctive
// ...A63A3621 00000000 00090563 tail that differs from the RFC-2409 MODP-768
// prime). Stored big-endian, 96 bytes.
static const uint8_t kPrimeMse[kKeyLen] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2, 0x34,
    0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1, 0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74,
    0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D, 0xF2, 0x5F, 0x14, 0x37,
    0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45, 0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6,
    0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x3A, 0x36, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x05, 0x63};

#if defined(TORRENT_SHA1_MBEDTLS)

// ---- Console backend: mbedtls big-number modexp -----------------------------
// mbedtls_mpi is always present in mbedcrypto (RSA/ECC depend on it), regardless
// of whether the RC4 module is compiled in — so this is the reliable console path.
static void modExp(const uint8_t* base, size_t baseLen, const uint8_t* exp, size_t expLen, uint8_t out[kKeyLen]) {
    std::memset(out, 0, kKeyLen);
    mbedtls_mpi B, E, P, R;
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&R);
    if (mbedtls_mpi_read_binary(&B, base, baseLen) == 0 && mbedtls_mpi_read_binary(&E, exp, expLen) == 0 &&
        mbedtls_mpi_read_binary(&P, kPrimeMse, kKeyLen) == 0 &&
        mbedtls_mpi_exp_mod(&R, &B, &E, &P, nullptr) == 0) {
        // write_binary right-justifies into the fixed 96-byte field (leading zeros).
        mbedtls_mpi_write_binary(&R, out, kKeyLen);
    }
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&R);
}

#else

// ---- Desktop backend: bundled fixed-width bignum ----------------------------
// CommonCrypto (Apple default) and OpenSSL's SHA-1 path expose no modular-exp
// API we depend on, so carry a minimal big-integer here. Limbs are uint32,
// little-endian (limb 0 = least significant). Correctness over speed: a full DH
// exchange is two modexps per peer, once, off the event loop.
namespace {
using Bn = std::vector<uint32_t>;

void trim(Bn& a) {
    while (a.size() > 1 && a.back() == 0) a.pop_back();
}

Bn fromBE(const uint8_t* p, size_t len) {
    Bn a;
    // Big-endian bytes -> little-endian 32-bit limbs.
    size_t i = len;
    while (i > 0) {
        uint32_t limb = 0;
        for (int b = 0; b < 4 && i > 0; b++) limb |= (uint32_t)p[--i] << (8 * b);
        a.push_back(limb);
    }
    if (a.empty()) a.push_back(0);
    trim(a);
    return a;
}

// Compare: -1 if a<b, 0 if equal, 1 if a>b.
int cmp(const Bn& a, const Bn& b) {
    size_t na = a.size(), nb = b.size();
    if (na != nb) return na < nb ? -1 : 1;
    for (size_t i = na; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// a -= b, assuming a >= b.
void subInPlace(Bn& a, const Bn& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        uint64_t bi = i < b.size() ? b[i] : 0;
        uint64_t cur = (uint64_t)a[i] - bi - borrow;
        a[i] = (uint32_t)cur;
        borrow = (cur >> 63) & 1;  // borrow occurred if the subtraction went negative
    }
    trim(a);
}

Bn mul(const Bn& a, const Bn& b) {
    Bn r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.size(); j++) {
            uint64_t cur = (uint64_t)a[i] * b[j] + r[i + j] + carry;
            r[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        r[i + b.size()] += (uint32_t)carry;
    }
    trim(r);
    return r;
}

// x mod m via binary long division (MSB-first). Simple and easy to verify.
Bn mod(const Bn& x, const Bn& m) {
    Bn r(1, 0);
    for (size_t limb = x.size(); limb-- > 0;) {
        for (int bit = 31; bit >= 0; bit--) {
            // r <<= 1
            uint32_t carry = 0;
            for (size_t i = 0; i < r.size(); i++) {
                uint32_t next = r[i] >> 31;
                r[i] = (r[i] << 1) | carry;
                carry = next;
            }
            if (carry) r.push_back(1);
            // r |= current bit of x
            if ((x[limb] >> bit) & 1) r[0] |= 1;
            if (cmp(r, m) >= 0) subInPlace(r, m);
        }
    }
    trim(r);
    return r;
}

Bn modExpBn(const Bn& base, const uint8_t* exp, size_t expLen, const Bn& m) {
    Bn result(1, 1);
    Bn b = mod(base, m);
    // Square-and-multiply, scanning the exponent MSB-first.
    for (size_t i = 0; i < expLen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            result = mod(mul(result, result), m);
            if ((exp[i] >> bit) & 1) result = mod(mul(result, b), m);
        }
    }
    return result;
}
}  // namespace

static void modExp(const uint8_t* base, size_t baseLen, const uint8_t* exp, size_t expLen, uint8_t out[kKeyLen]) {
    Bn m = fromBE(kPrimeMse, kKeyLen);
    Bn b = fromBE(base, baseLen);
    Bn r = modExpBn(b, exp, expLen, m);
    // Serialize big-endian, right-justified into the fixed 96-byte field.
    std::memset(out, 0, kKeyLen);
    for (size_t i = 0; i < r.size(); i++) {
        uint32_t limb = r[i];
        for (int byte = 0; byte < 4; byte++) {
            size_t pos = i * 4 + byte;               // byte offset from the little end
            if (pos >= kKeyLen) break;
            out[kKeyLen - 1 - pos] = (uint8_t)(limb >> (8 * byte));
        }
    }
}

#endif  // backend

void dhPublic(const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]) {
    const uint8_t g = 2;
    modExp(&g, 1, priv, privLen, out);
}

void dhSecret(const uint8_t peerPub[kKeyLen], const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]) {
    modExp(peerPub, kKeyLen, priv, privLen, out);
}

}  // namespace mse
}  // namespace torrent
