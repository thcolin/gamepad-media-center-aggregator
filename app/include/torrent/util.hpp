#pragma once

#include <cstdint>
#include <string>

#include "torrent/types.hpp"

namespace torrent {

/// Monotonic.
int64_t nowMs();

/// Use this, never std::random_device: it crashes on Switch and Vita.
void osRandom(void* buf, size_t n);

std::string randomBytes(size_t n);

/// 20 bytes: Azureus-style `prefix` (e.g. "-GM0001-") then random bytes.
std::string makePeerId(const std::string& prefix);

}  // namespace torrent
