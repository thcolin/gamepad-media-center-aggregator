// Standalone logic test — Stremio torrent streams mapped to source rows when the
// torrent engine is built in (ENABLE_TORRENT). See tests/run.sh.

#define ENABLE_TORRENT
#include <cstdio>
#include <api/stremio/types.hpp>

using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    // seeder counts as addons write them
    CHECK(stremio::parseSeeders("Torrentio\n4k \xF0\x9F\x91\xA4 45 \xF0\x9F\x92\xBE 2.1 GB") == 45);
    CHECK(stremio::parseSeeders("\xF0\x9F\x91\xA5:12") == 12);
    CHECK(stremio::parseSeeders("Seeders: 7") == 7);
    CHECK(stremio::parseSeeders("1080p 300 seeders") == 300);
    CHECK(stremio::parseSeeders("1080p WEB-DL") == -1);
    CHECK(stremio::parseSeeders("The Bad Seed 2018 1080p") == -1);
    CHECK(stremio::parseSeeders("\xF0\x9F\x91\xA4 99999999999999999999") > 0);

    // a Torrentio-style infoHash stream becomes a playable torrent row
    json j = json::parse(R"({"streams":[{
        "name":"Torrentio\n1080p","title":"Movie.2024.1080p.x265\n👤 45 💾 2.1 GB",
        "infoHash":"08ada5a7a6183aae1e09d831df6748d566095a10","fileIdx":2,
        "sources":["tracker:udp://tracker.opentrackr.org:1337/announce","dht:08ada5a7a6183aae1e09d831df6748d566095a10"]
    }]})");
    auto streams = stremio::parseStreams(j);
    CHECK(streams.size() == 1);
    if (!streams.empty()) {
        media::Media m = stremio::streamToMedia(streams[0], "Torrentio");
        CHECK(m.kind == media::SourceKind::Torrent);
        CHECK(m.playable());
        CHECK(m.parts.empty());
        CHECK(m.infoHash == "08ada5a7a6183aae1e09d831df6748d566095a10");
        CHECK(m.torrentFileIdx == 2);
        CHECK(m.torrentSources.size() == 2);
        CHECK(!m.videoCodec.empty());
    }

    if (failures == 0) printf("test_stremio_torrent: all passed\n");
    return failures ? 1 : 0;
}
