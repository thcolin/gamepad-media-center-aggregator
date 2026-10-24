// SHA-1 for piece checks and infohashes. The backend is chosen at compile time:
// TORRENT_SHA1_OPENSSL, TORRENT_SHA1_MBEDTLS (consoles), CommonCrypto on Apple,
// otherwise a bundled implementation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace torrent {

using Sha1Digest = std::array<uint8_t, 20>;

Sha1Digest sha1(const void* data, size_t len);

inline Sha1Digest sha1(const std::string& s) { return sha1(s.data(), s.size()); }

std::string toHex(const Sha1Digest& d);

}  // namespace torrent
