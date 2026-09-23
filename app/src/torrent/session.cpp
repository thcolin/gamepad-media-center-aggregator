/*
    GMCA — TorrentEngine session holder (see torrent/session.hpp).

    This file is app-integration glue, NOT part of the standalone engine target:
    it is compiled into the app only when ENABLE_TORRENT is set (desktop/switch), and is
    excluded from the app source GLOB otherwise (root CMakeLists). It therefore
    lives under app/src/torrent/ (already carved out of the GLOB) and is added to
    the app target explicitly.
*/

#include "torrent/session.hpp"

#include <cctype>

#include "torrent/engine.hpp"

namespace torrent {

namespace {

/// Percent-encode a magnet parameter value (tracker URL). parseMagnet() splits
/// the query on '&'/'=' and percent-decodes each value, so encoding here keeps a
/// tracker URL that contains those bytes intact.
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

/// Build a magnet URI from a raw infoHash and the addon's discovery hints. Only
/// `tracker:` hints are turned into `tr=` params (DHT is out of the engine's PoC
/// scope, TORRENT_STREAMING.md §7). The engine also accepts a bare infoHash, but
/// then it has no way to find peers — the trackers are what make it work.
std::string buildMagnet(const std::string& infoHash, const std::vector<std::string>& sources) {
    std::string magnet = "magnet:?xt=urn:btih:" + infoHash;
    for (const auto& src : sources) {
        if (src.rfind("tracker:", 0) == 0) {
            std::string url = src.substr(std::string("tracker:").size());
            if (!url.empty()) magnet += "&tr=" + percentEncode(url);
        }
        // "dht:<id>" and any other scheme are ignored (no DHT in the engine yet).
    }
    return magnet;
}

}  // namespace

EngineSession& EngineSession::instance() {
    static EngineSession s;
    return s;
}

EngineSession::~EngineSession() {
    close();
    joinPending();
}

std::string EngineSession::open(const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources) {
    // One playback at a time: finish tearing down any previous engine first.
    close();
    joinPending();

    auto engine = std::make_shared<TorrentEngine>(EngineConfig{});
    // Publish before the (blocking) open() so close()/stats() can reach it.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        engine_ = engine;
    }

    // open() blocks on metadata (magnet) but holds no session lock — a concurrent
    // close() can still swap the pointer out and stop this engine, which unblocks
    // open() (waitForMetadata() checks running_). The local shared_ptr keeps the
    // object alive across that race.
    std::string url = engine->open(buildMagnet(infoHash, sources), fileIdx);
    if (url.empty()) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (engine_ == engine) engine_.reset();
        }
        engine->close();
        return "";
    }
    return url;
}

void EngineSession::close() {
    std::shared_ptr<TorrentEngine> engine;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        engine.swap(engine_);
    }
    if (!engine) return;
    // Detach the teardown (tracker round-trips can make close() take a beat) so a
    // UI-thread caller never stalls. joinPending() (in open()/dtor) reaps it.
    joinPending();
    std::lock_guard<std::mutex> lk(mutex_);
    pending_ = std::thread([engine]() mutable { engine->close(); });
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
