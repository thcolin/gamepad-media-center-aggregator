// Standalone logic test — origin of a URL, used to drop the custom headers of a
// redirect to another server and to match the servers allowed a self-signed
// certificate.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_url_origin.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include <utils/url_origin.hpp>

using misc::urlOrigin;

static int failures = 0;
#define CHECK_EQ(got, want)                                                              \
    do {                                                                                 \
        std::string g = (got), w = (want);                                               \
        if (g != w) {                                                                    \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str()); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

int main() {
    CHECK_EQ(urlOrigin("https://example.com/a/b?c=d"), "https://example.com:443");
    CHECK_EQ(urlOrigin("http://example.com"), "http://example.com:80");
    CHECK_EQ(urlOrigin("HTTPS://Example.COM:8920/x"), "https://example.com:8920");
    CHECK_EQ(urlOrigin("https://example.com:443/"), urlOrigin("https://example.com/"));
    CHECK_EQ(urlOrigin("https://user:pass@nas.lan:5006/dav/"), "https://nas.lan:5006");
    CHECK_EQ(urlOrigin("https://a@b@nas.lan/"), "https://nas.lan:443");
    CHECK_EQ(urlOrigin("webdavs://nas.lan:5006/dav/"), "https://nas.lan:5006");
    CHECK_EQ(urlOrigin("webdav://nas.lan/"), "http://nas.lan:80");
    CHECK_EQ(urlOrigin("https://[fe80::1]:8096/x"), "https://[fe80::1]:8096");
    CHECK_EQ(urlOrigin("https://[fe80::1]/x"), "https://[fe80::1]:443");
    CHECK_EQ(urlOrigin("https://host?q=1"), "https://host:443");
    CHECK_EQ(urlOrigin("https://host#f"), "https://host:443");
    CHECK_EQ(urlOrigin("ftp://host/x"), "ftp://host");

    // a different port, scheme or host is another origin
    bool samePort = urlOrigin("https://h:8920/") == urlOrigin("https://h:8096/");
    bool sameScheme = urlOrigin("http://h/") == urlOrigin("https://h/");
    bool sameHost = urlOrigin("https://plex.tv/") == urlOrigin("https://evil.example/");
    CHECK_EQ(samePort || sameScheme || sameHost ? "same" : "different", "different");

    CHECK_EQ(urlOrigin("not a url"), "");
    CHECK_EQ(urlOrigin("://host"), "");
    CHECK_EQ(urlOrigin("https:///path"), "");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_url_origin: all passed\n");
    return 0;
}
