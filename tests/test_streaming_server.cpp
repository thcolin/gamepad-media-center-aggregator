// Standalone logic test: a Stremio streaming server for torrent streams
// (api/stremio/streaming_server.hpp): the part key, the address as typed, the create call's body,
// and the file the server streams.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include -Ilibrary/borealis/library/include/borealis/extern tests/test_streaming_server.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>

#include <api/stremio/streaming_server.hpp>

using namespace stremio::streaming;

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
    const std::string hash = "d160b8d8ea35a5b4e52837468fc8f03d55cef1f7";

    // the trackers among an addon's sources
    auto tr = trackersFromSources({"tracker:udp://a:1/announce", "dht:" + hash, "tracker:udp://a:1/announce",
        "tracker:", "tracker:" + std::string(300, 'x'), "tracker:http://b/announce"});
    CHECK_EQ(std::to_string(tr.size()), "2");
    CHECK_EQ(tr[0], "udp://a:1/announce");
    CHECK_EQ(tr[1], "http://b/announce");

    // the part key, and back
    TorrentStream t;
    t.infoHash = "D160B8D8EA35A5B4E52837468FC8F03D55CEF1F7";
    t.fileIdx = 3;
    t.filename = "Show S01E02 [1080p].mkv";
    t.trackers = {"udp://a:1/announce"};
    const std::string key = streamKey(t);
    CHECK_EQ(key, "stremio-torrent:" + hash + "/3/Show%20S01E02%20%5B1080p%5D.mkv?tr=udp%3A%2F%2Fa%3A1%2Fannounce");
    TorrentStream back;
    CHECK_EQ(parseStreamKey(key, back) ? "y" : "n", "y");
    CHECK_EQ(back.infoHash, hash);
    CHECK_EQ(std::to_string(back.fileIdx), "3");
    CHECK_EQ(back.filename, "Show S01E02 [1080p].mkv");
    CHECK_EQ(back.trackers.size() == 1 ? back.trackers[0] : "?", "udp://a:1/announce");

    TorrentStream bare;
    bare.infoHash = hash;
    CHECK_EQ(streamKey(bare), "stremio-torrent:" + hash + "/-1");
    CHECK_EQ(parseStreamKey(streamKey(bare), back) ? std::to_string(back.fileIdx) + back.filename : "n", "-1");

    bare.infoHash = "not-a-hash";
    CHECK_EQ(streamKey(bare), "");
    for (const char* k : {"http://host/x.mkv", "stremio-torrent:", "stremio-torrent:abc/1",
             "stremio-torrent:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7", "stremio-torrent:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7/x"})
        CHECK_EQ(parseStreamKey(k, back) ? "y" : "n", "n");

    // the address as typed
    CHECK_EQ(normalizeServerUrl("http://192.168.1.216:11470"), "http://192.168.1.216:11470");
    CHECK_EQ(normalizeServerUrl("  192.168.1.216:11470/ "), "http://192.168.1.216:11470");
    CHECK_EQ(normalizeServerUrl("HTTPS://stremio.lan/"), "https://stremio.lan");
    CHECK_EQ(normalizeServerUrl(""), "");
    CHECK_EQ(normalizeServerUrl("ftp://192.168.1.216"), "");
    CHECK_EQ(normalizeServerUrl("http://"), "");
    CHECK_EQ(normalizeServerUrl("http://:11470"), "");
    CHECK_EQ(normalizeServerUrl("http://my server:11470"), "");

    // the create call: trackers as sources, DHT, a file guess only without a file
    TorrentStream c;
    c.infoHash = hash;
    c.fileIdx = 2;
    c.trackers = {"udp://tracker.example:1337/announce"};
    CHECK_EQ(createBody(c).dump(),
        R"({"peerSearch":{"max":150,"min":40,"sources":["tracker:udp://tracker.example:1337/announce","dht:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"]},"torrent":{"infoHash":"d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"}})");
    c.fileIdx = -1;
    c.trackers.clear();
    CHECK_EQ(createBody(c).dump(),
        R"({"guessFileIdx":{},"peerSearch":{"max":150,"min":40,"sources":["dht:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"]},"torrent":{"infoHash":"d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"}})");

    // the file: the stream's own, the server's guess, the name, the largest
    const auto stats = nlohmann::json::parse(R"({"guessedFileIdx":-1,"files":[
        {"name":"Show/Sample.mkv","length":20000000},
        {"name":"Show/Show.S01E02.mkv","length":900000000},
        {"name":"Show/Show.S01E01.mkv","length":800000000}]})");
    auto pick = [](const TorrentStream& s, const nlohmann::json& j) { return std::to_string(chooseFile(s, j)); };
    TorrentStream f;
    f.infoHash = hash;
    f.fileIdx = 2;
    CHECK_EQ(pick(f, stats), "2");
    f.fileIdx = -1;
    CHECK_EQ(pick(f, stats), "1");
    f.filename = "show.s01e01.MKV";
    CHECK_EQ(pick(f, stats), "2");
    f.filename = "Other.mkv";
    CHECK_EQ(pick(f, stats), "1");
    auto guessed = stats;
    guessed["guessedFileIdx"] = 0;
    CHECK_EQ(pick(f, guessed), "0");
    CHECK_EQ(pick(f, nlohmann::json::parse(R"({"files":null})")), "-1");
    CHECK_EQ(pick(f, nlohmann::json::object()), "-1");

    CHECK_EQ(streamUrl("http://192.168.1.216:11470", hash, 0), "http://192.168.1.216:11470/" + hash + "/0");

    if (failures == 0) printf("test_streaming_server: all passed\n");
    return failures ? 1 : 0;
}
