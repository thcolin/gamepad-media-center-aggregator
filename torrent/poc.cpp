/*
    GMCA — torrent_poc: desktop proof-of-concept driver for the torrent engine.

    Streams a torrent to a local HTTP URL that mpv/ffprobe/curl can open.

    Usage:
      torrent_poc <magnet | infohash | file.torrent> [fileIdx] [options]

    Options:
      --peer host:port     inject a peer directly (deterministic local test)
      --tracker <url>      add an announce URL (http/https/udp)
      --webseed <url>      add a BEP-19 web-seed URL
      --port <n>           local HTTP port (default: ephemeral)
      --prebuffer <MB>     wait for N MiB contiguous from the file head (default 2)
      --run-seconds <n>    serve for N seconds then exit (default 0 = until Ctrl-C)
      --max-peers <n>      cap peer connections (default 40)
      --no-webseed         disable the BEP-19 fallback
      --dht                enable the BEP-5 mainline DHT (default: on)
      --no-dht             disable the BEP-5 mainline DHT (trackerless discovery)
      --encryption <mode>  MSE/PE policy: plain | prefer | force (default prefer)
      --transport <mode>   carrier: tcp | utp | both (default both = TCP + µTP
                           fallback). utp = µTP only (BEP-29); tcp = legacy TCP only

    On success it prints a line   READY url=<local url>   on stdout; test scripts
    parse that, then curl -r / ffprobe the URL.
*/

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "torrent/engine.hpp"
#include "torrent/log.hpp"

using namespace torrent;

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string humanBytes(int64_t b) {
    char buf[64];
    const char* u[] = {"B", "KiB", "MiB", "GiB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 3) {
        v /= 1024.0;
        i++;
    }
    snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <magnet|infohash|file.torrent> [fileIdx] [options]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // Timestamped stderr logging.
    setLogSink([](LogLevel lvl, const std::string& msg) {
        using namespace std::chrono;
        auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() % 100000;
        const char* tag = lvl == LogLevel::Error     ? "ERR"
                          : lvl == LogLevel::Warning ? "WRN"
                          : lvl == LogLevel::Info    ? "INF"
                                                     : "DBG";
        fprintf(stderr, "[%5lld][%s] %s\n", (long long)ms, tag, msg.c_str());
        fflush(stderr);
    });

    std::string target = argv[1];
    int fileIdx = -1;
    EngineConfig cfg;
    int prebufferMB = 2;
    int runSeconds = 0;
    std::vector<PeerAddr> peers;
    std::vector<std::string> trackers, webseeds;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--peer") {
            std::string hp = next("--peer");
            size_t c = hp.rfind(':');
            if (c != std::string::npos) peers.push_back({hp.substr(0, c), (uint16_t)std::stoi(hp.substr(c + 1))});
        } else if (a == "--tracker") {
            trackers.push_back(next("--tracker"));
        } else if (a == "--webseed") {
            webseeds.push_back(next("--webseed"));
        } else if (a == "--port") {
            cfg.httpPort = (uint16_t)std::stoi(next("--port"));
        } else if (a == "--prebuffer") {
            prebufferMB = std::stoi(next("--prebuffer"));
        } else if (a == "--run-seconds") {
            runSeconds = std::stoi(next("--run-seconds"));
        } else if (a == "--max-peers") {
            cfg.maxPeers = std::stoi(next("--max-peers"));
        } else if (a == "--no-webseed") {
            cfg.enableWebSeed = false;
        } else if (a == "--dht") {
            cfg.enableDht = true;
        } else if (a == "--no-dht") {
            cfg.enableDht = false;
        } else if (a == "--encryption") {
            std::string m = next("--encryption");
            if (m == "plain") {
                cfg.encryption = Encryption::Plaintext;
            } else if (m == "prefer") {
                cfg.encryption = Encryption::Prefer;
            } else if (m == "force") {
                cfg.encryption = Encryption::Forced;
            } else {
                fprintf(stderr, "unknown --encryption mode: %s (plain|prefer|force)\n", m.c_str());
                return 2;
            }
        } else if (a == "--transport") {
            std::string m = next("--transport");
            if (m == "tcp") {
                cfg.enableTcp = true;
                cfg.enableUtp = false;
            } else if (m == "utp") {
                cfg.enableTcp = false;
                cfg.enableUtp = true;
            } else if (m == "both") {
                cfg.enableTcp = true;
                cfg.enableUtp = true;
            } else {
                fprintf(stderr, "unknown --transport mode: %s (tcp|utp|both)\n", m.c_str());
                return 2;
            }
        } else if (!a.empty() && a[0] != '-') {
            fileIdx = std::stoi(a);
        } else {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }

    TorrentEngine engine(cfg);
    for (auto& t : trackers) engine.addTracker(t);
    for (auto& w : webseeds) engine.addWebSeed(w);
    for (auto& p : peers) engine.addPeer(p.host, p.port);

    // A .torrent file argument vs a magnet/infohash.
    std::string url;
    bool looksLikeFile = target.size() > 8 && target.compare(target.size() - 8, 8, ".torrent") == 0;
    std::string raw;
    if (looksLikeFile && readFile(target, raw)) {
        url = engine.openTorrentFile(raw, fileIdx);
    } else {
        url = engine.open(target, fileIdx);
    }
    if (url.empty()) {
        fprintf(stderr, "FAILED to open / acquire metadata\n");
        return 1;
    }
    printf("READY url=%s\n", url.c_str());
    fflush(stdout);

    // Prebuffer from the head so a player can start immediately.
    int64_t prebuffer = (int64_t)prebufferMB * 1024 * 1024;
    fprintf(stderr, "prebuffering %s from head...\n", humanBytes(prebuffer).c_str());
    engine.waitForContiguous(prebuffer, 120000);

    int64_t start =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    while (!g_stop) {
        Stats s = engine.stats();
        fprintf(stderr, "peers %d/%d | pieces %d/%d | dl %s | rate %.0f KiB/s | head %s | webseeds %d | dht %d\n",
            s.peersConnected, s.peersKnown, s.piecesHave, s.piecesTotal, humanBytes(s.downloadedBytes).c_str(),
            s.downloadRateBps / 1024.0, humanBytes(s.contiguousReadyBytes).c_str(), s.webSeeds, s.dhtNodes);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (runSeconds > 0) {
            int64_t now =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (now - start >= runSeconds) break;
        }
    }

    fprintf(stderr, "shutting down...\n");
    engine.close();
    return 0;
}
