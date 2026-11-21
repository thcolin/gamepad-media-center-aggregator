// Standalone logic test — redaction of mpv log lines shown in the playback-error dialog,
// and of the credentials and token parameters in the URLs of the video info screen.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_log_redact.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <utils/log_redact.hpp>

using misc::redactLogLine;
using misc::redactTokens;
using misc::redactUrl;

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

    CHECK_EQ(redactLogLine("tcp: Failed to resolve hostname 82-64-12-34.abc.plex.direct: nodename nor servname"),
        "tcp: Failed to resolve hostname <host>: nodename nor servname");

    CHECK_EQ(redactLogLine("tcp: Connection to tcp://82.64.12.34:32400 failed, retry 82.64.12.34."),
        "tcp: Connection to tcp://<host>:32400 failed, retry <host>.");

    CHECK_EQ(redactLogLine("ffmpeg 7.1.5 HTTP/1.1"), "ffmpeg 7.1.5 HTTP/1.1");

    CHECK_EQ(redactLogLine("ftp://bob:hunter2@nas.example.org/Films/a.mkv"), "ftp://<host>/…");

    CHECK_EQ(redactLogLine("ftp://bob:hunter2@nas.example.org:2121/a.mkv"), "ftp://<host>:2121/…");

    CHECK_EQ(redactLogLine("http://[fe80::1]/a"), "http://<host>/…");

    CHECK_EQ(redactLogLine("ApiKey=K x-plex-token=T X-Plex-Token%3DT%26a=1 token=abc;"),
        "ApiKey=*** x-plex-token=*** X-Plex-Token%3D***%26a=1 token=***;");

    CHECK_EQ(redactLogLine("a\x01" "b\x1b[31mc"), "ab[31mc");

    {
        std::string longLine(300, 'x');
        longLine.replace(199, 2, "\xc3\xa9");
        std::string out = redactLogLine(longLine);
        CHECK_EQ(out, std::string(199, 'x') + "…");
    }

    CHECK_EQ(redactTokens("http://192.168.1.10:8097/emby/Videos/42/stream.mkv"
                          "?Static=true&MediaSourceId=abc&api_key=SECRET"),
        "http://192.168.1.10:8097/emby/Videos/42/stream.mkv?Static=true&MediaSourceId=abc&api_key=***");

    CHECK_EQ(redactTokens("https://1-2-3-4.abcdef.plex.direct:32400/library/parts/1/2/file.mkv?X-Plex-Token=SECRET"),
        "https://1-2-3-4.abcdef.plex.direct:32400/library/parts/1/2/file.mkv?X-Plex-Token=***");

    CHECK_EQ(redactTokens("https://h:32400/video/:/transcode/universal/start.m3u8?path=%2Flibrary%2Fmetadata%2F7"
                          "%3FX-Plex-Token%3DSECRET%26a%3D1&protocol=hls&X-Plex-Token=SECRET"),
        "https://h:32400/video/:/transcode/universal/start.m3u8?path=%2Flibrary%2Fmetadata%2F7"
        "%3FX-Plex-Token%3D***%26a%3D1&protocol=hls&X-Plex-Token=***");

    CHECK_EQ(redactTokens("http://h:8096/videos/9/master.m3u8?DeviceId=d&ApiKey=SECRET&TranscodeReasons=AudioCodec"),
        "http://h:8096/videos/9/master.m3u8?DeviceId=d&ApiKey=***&TranscodeReasons=AudioCodec");

    CHECK_EQ(redactTokens("/Volumes/Films/Movie (2020)/movie.mkv"), "/Volumes/Films/Movie (2020)/movie.mkv");

    CHECK_EQ(redactTokens(""), "");

    CHECK_EQ(redactUrl("ftp://bob:hunter2@nas.lan:21/Films/a.mkv"), "ftp://***@nas.lan:21/Films/a.mkv");

    CHECK_EQ(redactUrl("sftp://bob@nas.lan:22/a.mkv"), "sftp://***@nas.lan:22/a.mkv");

    CHECK_EQ(redactUrl("http://h:8096/a@b.mkv?api_key=K"), "http://h:8096/a@b.mkv?api_key=***");

    CHECK_EQ(redactUrl("sftp://bob:p ss@nas.lan:22/a.mkv"), "sftp://***@nas.lan:22/a.mkv");

    CHECK_EQ(redactUrl("ftp://bob:p\"ss@nas.lan"), "ftp://***@nas.lan");

    CHECK_EQ(redactUrl("/Volumes/Films/a.mkv"), "/Volumes/Films/a.mkv");

    CHECK_EQ(redactUrl("network-timeout=30,http-header-fields=\"Authorization: Basic Ym9iOmh1bnRlcjI=\",user_agent=\"x\""),
        "network-timeout=30,http-header-fields=\"Authorization: Basic ***\",user_agent=\"x\"");

    CHECK_EQ(redactTokens("authorization:SECRET"), "authorization:***");

    CHECK_EQ(redactTokens("Authorization: MediaBrowser Token=abc, Client=x"), "Authorization: MediaBrowser ***, Client=x");

    if (failures == 0) {
        printf("test_log_redact: OK\n");
        return 0;
    }
    printf("test_log_redact: %d FAILURE(S)\n", failures);
    return 1;
}
