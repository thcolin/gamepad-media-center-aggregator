#include "torrent/session.hpp"

#include <algorithm>
#include <cctype>

#include <borealis/core/logger.hpp>

#include "torrent/engine.hpp"
#include "torrent/log.hpp"

namespace torrent {

namespace {

// parseMagnet() splits on '&' and '=' before percent-decoding, so a tracker URL keeps them
std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Addons often send a bare infoHash with no tracker hint, and a cold DHT search
// rarely finds metadata in time. List from amansingh909's PS4 build, reliable first:
// trackers are announced one after the other.
const char* const kDefaultTrackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://explodie.org:6969/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://tracker.tiny-vps.com:6969/announce",
    "udp://tracker.dler.org:6969/announce",
    "udp://opentracker.io:6969/announce",
    "udp://tracker.theoks.net:6969/announce",
    "udp://tracker-udp.gbitt.info:80/announce",
    "udp://tracker.qu.ax:6969/announce",
    "udp://tracker.filemail.com:6969/announce",
    "http://tracker.opentrackr.org:1337/announce",
    "http://tracker.bt4g.com:2095/announce",
};

std::string buildMagnet(const std::string& infoHash, const std::vector<std::string>& sources) {
    std::string magnet = "magnet:?xt=urn:btih:" + infoHash;
    std::vector<std::string> trackers;
    for (const auto& src : sources) {
        if (src.rfind("tracker:", 0) == 0) {
            std::string url = src.substr(std::string("tracker:").size());
            if (!url.empty()) trackers.push_back(url);
        }
        // "dht:" hints are ignored: the engine searches the DHT by infohash (off on Vita)
    }
    for (const char* t : kDefaultTrackers)
        if (std::find(trackers.begin(), trackers.end(), t) == trackers.end()) trackers.push_back(t);
    for (const auto& url : trackers) magnet += "&tr=" + percentEncode(url);
    return magnet;
}

}  // namespace

EngineSession::EngineSession() {
    // engine logs go to the app log, the one a bug report carries
    setLogSink([](LogLevel level, const std::string& msg) {
        switch (level) {
        case LogLevel::Debug: brls::Logger::debug("torrent: {}", msg); break;
        case LogLevel::Info: brls::Logger::info("torrent: {}", msg); break;
        case LogLevel::Warning: brls::Logger::warning("torrent: {}", msg); break;
        case LogLevel::Error: brls::Logger::error("torrent: {}", msg); break;
        }
    });
}

EngineSession& EngineSession::instance() {
    static EngineSession s;
    return s;
}

EngineSession::~EngineSession() {
    close();
    joinPending();
}

uint64_t EngineSession::expect() {
    std::lock_guard<std::mutex> lk(mutex_);
    return ++generation_;
}

std::string EngineSession::open(
    uint64_t generation, const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources) {
    teardown();
    joinPending();

    auto engine = std::make_shared<TorrentEngine>(EngineConfig{});
    // published before the blocking open() so close() and stats() can reach it
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // the player closed or moved to another source before this task ran
        if (generation != generation_) return "";
        engine_ = engine;
    }

    // no session lock held here: a concurrent close() stops the engine, which
    // unblocks open(), and the local shared_ptr keeps it alive meanwhile
    std::string url = engine->open(buildMagnet(infoHash, sources), fileIdx);
    bool current;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        current = engine_ == engine;
        if (url.empty() && current) engine_.reset();
    }
    // closed meanwhile: its teardown thread owns it, and the URL is dead
    if (!current) return "";
    if (url.empty()) engine->close();
    return url;
}

void EngineSession::close() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++generation_;
    }
    teardown();
}

void EngineSession::teardown() {
    std::shared_ptr<TorrentEngine> engine;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        engine.swap(engine_);
    }
    if (!engine) return;
    // the teardown waits on any tracker announce in flight, so it runs on its own
    // thread, after the previous one; the next close(), open() or the destructor joins it
    std::lock_guard<std::mutex> lk(mutex_);
    pending_ = std::thread([engine, prev = std::move(pending_)]() mutable {
        if (prev.joinable()) prev.join();
        engine->close();
    });
}

void EngineSession::joinPending() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t.swap(pending_);
    }
    if (t.joinable()) t.join();
}

Stats EngineSession::stats() const {
    std::shared_ptr<TorrentEngine> engine;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        engine = engine_;
    }
    return engine ? engine->stats() : Stats{};
}

bool EngineSession::active() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<bool>(engine_);
}

}  // namespace torrent
