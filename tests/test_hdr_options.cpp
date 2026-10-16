// Standalone logic test: what an HDR file gets on the PS4 (view/hdr_options.hpp).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_hdr_options.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include "view/hdr_options.hpp"

static int failures = 0;
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    using namespace hdr;
    CHECK_EQ(retagFilter(), "@hdr:format=gamma=bt.1886:primaries=bt.709:sig-peak=1");

    // the mirror setting: the re-tag filter first, in the same list
    CHECK_EQ(filterChain(0, true), retagFilter());
    CHECK_EQ(filterChain(1, true), retagFilter() + ",hflip");
    CHECK_EQ(filterChain(2, true), retagFilter() + ",vflip");
    CHECK_EQ(filterChain(0, false), "");
    CHECK_EQ(filterChain(1, false), "hflip");
    CHECK_EQ(filterChain(2, false), "vflip");

    // only PQ gets a boost: HLG and SDR transfers keep the viewer's values
    CHECK_EQ(describe(planFor("pq")), "saturation +15, contrast +10");
    for (const char* gamma : {"hlg", "bt.1886", "srgb", "gamma2.2", "linear", "", "PQ"})
        CHECK_EQ(describe(planFor(gamma)), "none");

    CHECK_EQ(std::to_string(clampEqualizer(95 + 15)), "100");
    CHECK_EQ(std::to_string(clampEqualizer(-95 - 30)), "-100");
    CHECK_EQ(std::to_string(clampEqualizer(5 + 15)), "20");

    if (failures) return 1;
    printf("hdr_options: all checks passed\n");
    return 0;
}
