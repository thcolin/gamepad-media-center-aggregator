// Standalone logic test — redaction of mpv log lines shown in the playback-error dialog.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_log_redact.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <utils/log_redact.hpp>

using misc::redactLogLine;

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
    CHECK_EQ(redactLogLine("https: HTTP error 500 Internal Server Error\n"),
        "https: HTTP error 500 Internal Server Error");

    CHECK_EQ(redactLogLine("Failed to open https://1-2-3-4.abcdef.plex.direct:8443/library/parts/1/file.mkv"
                           "?download=1&X-Plex-Token=SECRET."),
        "Failed to open https://<host>:8443/…");

    CHECK_EQ(redactLogLine("Failed to open 'https://torrentio.strem.fun/resolve/realdebrid/APIKEY/hash/1/f.mkv'"),
        "Failed to open 'https://<host>/…'");

    CHECK_EQ(redactLogLine("http://192.168.1.10:8096 refused"), "http://<host>:8096 refused");

    CHECK_EQ(redactLogLine("a http://h/x b https://k/y"), "a http://<host>/… b https://<host>/…");

    CHECK_EQ(redactLogLine("header X-Plex-Token=SECRET&x=1 api_key=K"), "header X-Plex-Token=***&x=1 api_key=***");

    CHECK_EQ(redactLogLine("no url here"), "no url here");

    if (failures == 0) {
        printf("test_log_redact: OK\n");
        return 0;
    }
    printf("test_log_redact: %d FAILURE(S)\n", failures);
    return 1;
}
