// Standalone logic test: loadfile options (view/mpv_options.hpp).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_mpv_options.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include "view/mpv_options.hpp"

static int failures = 0;
#define CHECK_EQ(got, want)                                                                    \
    do {                                                                                       \
        std::string g = (got), w = (want);                                                     \
        if (g != w) {                                                                          \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str()); \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

int main() {
    CHECK_EQ(mpvopt::add("network-timeout=30", "user-agent", "Mozilla/5.0 (X11; Linux x86_64) Gecko, like"),
             "network-timeout=30,user-agent=%43%Mozilla/5.0 (X11; Linux x86_64) Gecko, like");
    // no leading comma: mpv rejects an empty key
    CHECK_EQ(mpvopt::add("", "user-agent", "a=b\"c"), "user-agent=%5%a=b\"c");
    // the length is in bytes, as mpv reads it
    CHECK_EQ(mpvopt::add("", "user-agent", "é"), "user-agent=%2%é");

    if (failures) return 1;
    printf("mpv_options: all checks passed\n");
    return 0;
}
