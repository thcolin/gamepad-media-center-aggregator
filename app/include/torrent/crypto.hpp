/*
    GMCA — portable crypto primitives for MSE/PE (Message Stream Encryption).

    MSE needs three things the plain BEP-3 wire never did: RC4 (the obfuscation
    stream cipher), a 768-bit Diffie-Hellman modular exponentiation, and SHA-1
    (already provided by sha1.hpp). Like the SHA-1 dispatch, the heavy primitive
    (bignum modexp) is picked at compile time so the console ports swap the backend
    without touching call sites:
      - TORRENT_SHA1_MBEDTLS -> mbedtls_mpi_exp_mod (console: Switch/Vita/PS4)
      - otherwise            -> a small bundled fixed-width bignum (desktop:
                                CommonCrypto/OpenSSL builds have no modexp API)

    RC4 is implemented by hand on every target: mbedtls_arc4 is frequently compiled
    out of modern mbedtls configs, and RC4 here is obfuscation (not real security),
    so a portable ~20-line implementation is both simplest and safest. None of this
    is secret-critical: MSE only defeats naive plaintext DPI, it is not a security
    boundary — see TORRENT_STREAMING.md.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace torrent {

/// RC4 (ARC4) keystream cipher — MSE obfuscation only. State is trivially
/// copyable (no heap), so the peer connection can lift the cipher out of the
/// handshake object once negotiation resolves.
class Rc4 {
public:
    Rc4() = default;

    /// Key-schedule from `len` key bytes and reset the stream position.
    void init(const uint8_t* key, size_t len);
    /// Advance the keystream by `n` bytes without touching data (MSE discards the
    /// first 1024 bytes of every RC4 stream to shed the weak-key prefix).
    void discard(size_t n);
    /// XOR `len` bytes in place with the keystream (encrypt == decrypt).
    void process(uint8_t* data, size_t len);

    bool ready() const { return ready_; }

private:
    uint8_t s_[256] = {0};
    uint8_t i_ = 0;
    uint8_t j_ = 0;
    bool ready_ = false;
};

namespace mse {

constexpr int kKeyLen = 96;  // 768-bit DH public key / shared secret, big-endian

/// Our DH public key: (2 ^ priv) mod P_MSE, written big-endian into out[96]
/// (leading zeros preserved — the peer expects a fixed 96-byte field).
void dhPublic(const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]);

/// The shared secret S = (peerPub ^ priv) mod P_MSE, big-endian into out[96].
void dhSecret(const uint8_t peerPub[kKeyLen], const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]);

}  // namespace mse

}  // namespace torrent
