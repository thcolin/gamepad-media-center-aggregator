// RC4 and 768-bit DH for MSE. MSE only defeats naive DPI, it is not a security
// boundary. RC4 is hand-written because mbedtls configs often compile arc4 out.

#pragma once

#include <cstddef>
#include <cstdint>

namespace torrent {

/// Trivially copyable, so the peer can take the cipher from the handshake.
class Rc4 {
public:
    Rc4() = default;

    void init(const uint8_t* key, size_t len);
    void discard(size_t n);
    void process(uint8_t* data, size_t len);

    bool ready() const { return ready_; }

private:
    uint8_t s_[256] = {0};
    uint8_t i_ = 0;
    uint8_t j_ = 0;
    bool ready_ = false;
};

namespace mse {

constexpr int kKeyLen = 96;  // 768-bit, big-endian

/// (2 ^ priv) mod P, left-padded with zeros to the fixed 96 bytes.
void dhPublic(const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]);

/// (peerPub ^ priv) mod P.
void dhSecret(const uint8_t peerPub[kKeyLen], const uint8_t* priv, size_t privLen, uint8_t out[kKeyLen]);

}  // namespace mse

}  // namespace torrent
