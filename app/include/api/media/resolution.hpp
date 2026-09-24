/*
    GMCA — max playback resolution, backend-agnostic.

    A limit is a height ("1080p") read as a 16:9 box: 1080p means at most
    1920x1080, so a 1920x800 scope film and a 1440x1080 4:3 one both fit.
*/

#pragma once

#include <cstdint>

namespace media {

inline int resolutionBoxWidth(int maxHeight) { return maxHeight * 16 / 9; }

/// A limit <= 0 (Auto) or an unknown source size (0) never exceeds.
inline bool exceedsResolution(int width, int height, int maxHeight) {
    if (maxHeight <= 0) return false;
    return height > maxHeight || width > resolutionBoxWidth(maxHeight);
}

/// bps, from the presets of the in-player quality menu
inline int64_t resolutionBitrate(int maxHeight) {
    if (maxHeight >= 2160) return 20000000;
    if (maxHeight >= 1440) return 15000000;
    if (maxHeight >= 1080) return 8000000;
    if (maxHeight >= 720) return 4000000;
    return 1500000;
}

}  // namespace media
