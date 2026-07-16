// Standalone logic test — URL host parsing / mDNS detection (utils/hostname.hpp).
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_hostname.cpp -o /tmp/t && /tmp/t
//
// Exercises the PURE helpers behind the "could not resolve hostname" fix: the
// host we name back to the user, and whether it's a `.local` (mDNS) address the
// console can never resolve. No HTTP / Borealis dependency.

#include <cstdio>
#include <utils/hostname.hpp>

using net::hostFromUrl;
using net::isMdnsHost;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    // --- hostFromUrl + isMdnsHost, the real-world cases -------------------
    struct Case {
        const char* url;
        const char* host;
        bool mdns;
    } cases[] = {
        {"http://jellyfin.local:8096", "jellyfin.local", true},
        {"http://raspberrypi.local", "raspberrypi.local", true},
        {"https://JELLYFIN.LOCAL:8920/", "JELLYFIN.LOCAL", true},  // case-insensitive
        {"http://media.local.:8096", "media.local.", true},        // trailing dot tolerated
        {"http://192.168.1.50:8096", "192.168.1.50", false},
        {"http://homeserver:8096", "homeserver", false},
        {"https://plex.example.com", "plex.example.com", false},
        {"http://user:pass@nas.local:5000/path", "nas.local", true},  // userinfo stripped
        {"http://[fe80::1]:8096/x", "fe80::1", false},                // IPv6 literal
        {"http://localhost", "localhost", false},                     // bare 'local' != .local
        {"http://mylocal", "mylocal", false},                         // suffix must be ".local"
        {"webdav://box.local:80/dav/", "box.local", true},
    };
    for (const auto& c : cases) {
        std::string h = hostFromUrl(c.url);
        bool m = isMdnsHost(c.url);
        CHECK(h == c.host);
        CHECK(m == c.mdns);
        if (h != c.host || m != c.mdns)
            printf("  (url=%s) got host=%s mdns=%d, expected host=%s mdns=%d\n", c.url, h.c_str(), m, c.host,
                   c.mdns);
    }

    // --- edge cases: must not crash, best-effort output -------------------
    CHECK(hostFromUrl("") == "");                                  // empty
    CHECK(isMdnsHost("") == false);
    CHECK(hostFromUrl("jellyfin.local:8096") == "jellyfin.local");  // scheme-less
    CHECK(isMdnsHost("jellyfin.local:8096") == true);
    CHECK(hostFromUrl("http://box.local?q=1") == "box.local");      // query, no path
    CHECK(isMdnsHost("http://box.local?q=1") == true);
    CHECK(hostFromUrl("http://[fe80::1%25eth0]:80") == "fe80::1%25eth0");  // IPv6 zone id kept inside []
    CHECK(isMdnsHost("http://[fe80::1%25eth0]:80") == false);
    CHECK(hostFromUrl("http://.local") == ".local");                // pathological: bare ".local"
    CHECK(isMdnsHost("http://.local") == false);                    // size must exceed the suffix

    if (failures == 0)
        printf("test_hostname: all checks passed\n");
    else
        printf("test_hostname: %d FAILED\n", failures);
    return failures ? 1 : 0;
}
