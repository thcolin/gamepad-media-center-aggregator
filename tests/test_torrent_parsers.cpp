// Standalone logic test — torrent engine parsers fed hostile or malformed input
// (bencode, info dict, magnet). The engine is not header-only, so its parser
// sources are compiled straight into this test. See tests/run.sh.

#include <cstdio>
#include <string>

#include "../app/src/torrent/bencode.cpp"
#include "../app/src/torrent/log.cpp"
#include "../app/src/torrent/metadata.cpp"
#include "../app/src/torrent/sha1.cpp"
#include "../app/src/torrent/util.cpp"

using namespace torrent;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static bool decodes(const std::string& s) {
    bencode::Value v;
    return bencode::decode(s, v);
}

static std::string str(const std::string& s) { return std::to_string(s.size()) + ":" + s; }

static std::string pieces(int n) { return str(std::string((size_t)n * 20, 'h')); }

static bool parsesInfo(const std::string& info) {
    TorrentMetadata m;
    return parseInfoDict(info, InfoHash{}, m);
}

int main() {
    setLogSink([](LogLevel, const std::string&) {});

    // well-formed values round-trip to canonical form
    {
        std::string s = "d3:bar4:spam3:fooi42e4:listli1ei-2eee";
        bencode::Value v;
        CHECK(bencode::decode(s, v));
        CHECK(v.intAt("foo") == 42);
        CHECK(v.strAt("bar") == "spam");
        CHECK(bencode::encode(v) == s);
    }

    // truncated or malformed input is refused
    CHECK(!decodes(""));
    CHECK(!decodes("i42"));
    CHECK(!decodes("ie"));
    CHECK(!decodes("5:abc"));
    CHECK(!decodes("l"));
    CHECK(!decodes("di1ei2ee"));  // non-string key
    CHECK(!decodes("x"));

    // a length prefix too large for the buffer, or for int64, is refused
    CHECK(!decodes("99999999999999999999999:x"));
    CHECK(!decodes("18446744073709551615:x"));

    // nesting deep enough to exhaust a console thread stack is refused
    CHECK(!decodes(std::string(100000, 'l') + std::string(100000, 'e')));
    CHECK(decodes(std::string(32, 'l') + std::string(32, 'e')));

    // a sane single-file info dict parses
    {
        std::string info = "d6:lengthi40000e4:name5:a.mkv12:piece lengthi16384e6:pieces" + pieces(3) + "e";
        TorrentMetadata m;
        CHECK(parseInfoDict(info, InfoHash{}, m));
        CHECK(m.numPieces() == 3);
        CHECK(m.files.size() == 1 && m.files[0].length == 40000);
    }

    // the infohash of the raw dict must match the expected one
    {
        std::string info = "d6:lengthi16384e4:name1:a12:piece lengthi16384e6:pieces" + pieces(1) + "e";
        InfoHash wrong{};
        wrong[0] = 1;
        TorrentMetadata m;
        CHECK(!parseInfoDict(info, wrong, m));
    }

    // piece count must cover the total length exactly
    CHECK(!parsesInfo("d6:lengthi40000e4:name1:a12:piece lengthi16384e6:pieces" + pieces(2) + "e"));
    CHECK(!parsesInfo("d6:lengthi40000e4:name1:a12:piece lengthi16384e6:pieces" + pieces(4) + "e"));

    // negative lengths and absurd piece sizes are refused
    CHECK(!parsesInfo("d6:lengthi-1e4:name1:a12:piece lengthi16384e6:pieces" + pieces(1) + "e"));
    CHECK(!parsesInfo("d5:filesld6:lengthi-16384eed6:lengthi32768eee4:name1:a12:piece lengthi16384e6:pieces" +
                      pieces(1) + "e"));
    CHECK(!parsesInfo("d6:lengthi1099511627776e4:name1:a12:piece lengthi1099511627776e6:pieces" + pieces(1) + "e"));

    // magnet links: hex and base32 infohash, trackers, garbage
    {
        MagnetInfo mi;
        CHECK(parseMagnet("magnet:?xt=urn:btih:08ada5a7a6183aae1e09d831df6748d566095a10&dn=Sintel"
                          "&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce",
                  mi));
        CHECK(mi.hasInfoHash);
        CHECK(infoHashHex(mi.infoHash) == "08ada5a7a6183aae1e09d831df6748d566095a10");
        CHECK(mi.trackers.size() == 1 && mi.trackers[0] == "udp://tracker.opentrackr.org:1337/announce");
        CHECK(mi.displayName == "Sintel");
    }
    {
        MagnetInfo mi;
        CHECK(parseMagnet("08ada5a7a6183aae1e09d831df6748d566095a10", mi));
        CHECK(mi.hasInfoHash);
    }
    {
        MagnetInfo mi;
        CHECK(!parseMagnet("magnet:?xt=urn:btih:zz", mi));
        CHECK(!parseMagnet("not a magnet", mi));
    }

    if (failures == 0) printf("test_torrent_parsers: all passed\n");
    return failures ? 1 : 0;
}
