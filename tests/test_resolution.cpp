// Standalone logic test — max playback resolution (issue #67).
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_resolution.cpp -o /tmp/t && /tmp/t
//
// Exercises the PURE box logic (api/media/resolution.hpp): which sources a
// limit forces into a transcode, and at which bitrate.

#include <cstdio>
#include <api/media/resolution.hpp>

using media::exceedsResolution;
using media::resolutionBitrate;
using media::resolutionBoxWidth;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    CHECK(resolutionBoxWidth(2160) == 3840);
    CHECK(resolutionBoxWidth(1440) == 2560);
    CHECK(resolutionBoxWidth(1080) == 1920);
    CHECK(resolutionBoxWidth(720) == 1280);

    // Auto and unknown source size never force a transcode
    CHECK(!exceedsResolution(7680, 4320, 0));
    CHECK(!exceedsResolution(0, 0, 1080));

    // 1080p box
    CHECK(!exceedsResolution(1920, 1080, 1080));
    CHECK(!exceedsResolution(1920, 800, 1080));   // 2.40:1 scope
    CHECK(!exceedsResolution(1440, 1080, 1080));  // 4:3
    CHECK(exceedsResolution(3840, 2160, 1080));
    CHECK(exceedsResolution(3840, 1600, 1080));  // 4K scope: too wide, fits in height
    CHECK(exceedsResolution(1920, 1088, 1080));
    CHECK(exceedsResolution(7680, 4320, 2160));  // the issue's 8K test file on a 4K limit

    CHECK(resolutionBitrate(2160) == 20000000);
    CHECK(resolutionBitrate(1440) == 15000000);
    CHECK(resolutionBitrate(1080) == 8000000);
    CHECK(resolutionBitrate(720) == 4000000);
    CHECK(resolutionBitrate(480) == 1500000);

    if (failures) {
        printf("test_resolution: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_resolution: OK\n");
    return 0;
}
