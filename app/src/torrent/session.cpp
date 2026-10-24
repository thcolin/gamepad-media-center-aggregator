#include "torrent/session.hpp"

#include <cctype>

#include "torrent/engine.hpp"

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

std::string buildMagnet(const std::string& infoHash, const std::vector<std::string>& sources) {
    std::string magnet = "magnet:?xt=urn:btih:" + infoHash;
    for (const auto& src : sources) {
        if (src.rfind("tracker:", 0) == 0) {
            std::string url = src.substr(std::string("tracker:").size());
            if (!url.empty()) magnet += "&tr=" + percentEncode(url);
        }
        // "dht:" hints are ignored: the engine searches the DHT by infohash (off on Vita)
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
    close();
    joinPending();

    auto engine = std::make_shared<TorrentEngine>(EngineConfig{});
    // published before the blocking open() so close() and stats() can reach it
    {
        std::lock_guard<std::mutex> lk(mutex_);
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
    std::shared_ptr<TorrentEngine> engine;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        engine.swap(engine_);
    }
    if (!engine) return;
    // the teardown waits on any tracker announce in flight, so it runs on its own
    // thread; the next close(), open() or the destructor joins it
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
