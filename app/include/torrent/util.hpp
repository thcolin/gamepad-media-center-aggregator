/*
    GMCA — small engine utilities (monotonic clock, randomness, peer id).
*/

#pragma once

#include <cstdint>
#include <string>

#include "torrent/types.hpp"

namespace torrent {

/// Monotonic milliseconds (steady_clock) for timeouts/rates.
int64_t nowMs();

/// Fill `buf` with `n` bytes of OS entropy. Portable and — crucially — never
/// touches /dev/urandom on the consoles (libnx randomGet / Vita SceKernel RNG /
/// BSD arc4random), where std::random_device NULL-derefs on a missing device
/// node and crashes the app (Data Abort). Desktop uses /dev/urandom then
/// std::random_device as a last resort.
void osRandom(void* buf, size_t n);

/// Fill `n` random bytes (peer id / tracker transaction ids). Uses osRandom.
std::string randomBytes(size_t n);

/// A 20-byte peer id: `prefix` (Azureus style, e.g. "-GM0001-") + random tail.
std::string makePeerId(const std::string& prefix);

}  // namespace torrent
