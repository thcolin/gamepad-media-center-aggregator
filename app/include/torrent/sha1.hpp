/*
    GMCA — SHA-1 behind a thin abstraction.

    Every downloaded piece is verified against the 20-byte SHA-1 the metadata
    pins for it (BEP-3). The digest is also how an infohash is derived from an
    info dictionary. SHA-1 is cryptographically dead but it is what the BitTorrent
    wire protocol mandates, so it is used strictly as a content checksum here.

    Backend is picked at compile time so the console ports swap the primitive
    without touching call sites:
      - Apple desktop  -> CommonCrypto (system, no extra link)         [default]
      - TORRENT_SHA1_OPENSSL -> OpenSSL libcrypto                       (desktop)
      - TORRENT_SHA1_MBEDTLS -> mbedtls  (already linked via curl on   [console
                                 Switch/Vita/PS4 — the console seam)     seam]
      - otherwise      -> a small bundled public-domain implementation
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace torrent {

using Sha1Digest = std::array<uint8_t, 20>;

/// One-shot SHA-1 of a byte range. Pieces are fully buffered in RAM before
/// hashing (the engine holds no partial-piece state), so a one-shot API is
/// enough and keeps the abstraction minimal.
Sha1Digest sha1(const void* data, size_t len);

inline Sha1Digest sha1(const std::string& s) { return sha1(s.data(), s.size()); }

/// Lowercase hex of a digest (for logs / debug).
std::string toHex(const Sha1Digest& d);

}  // namespace torrent
