// A Stremio streaming server set in Settings (api/stremio/streaming_server.hpp): the requests to
// it.
#include "api/stremio/streaming_server.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <borealis.hpp>

#include "api/http.hpp"

using namespace brls::literals;

namespace stremio::streaming {

namespace {
std::mutex mutex;
std::string server;  // configure()'s, normalized
}  // namespace

std::string configured() {
    std::lock_guard<std::mutex> lock(mutex);
    return server;
}

void configure(const std::string& typed) {
    std::lock_guard<std::mutex> lock(mutex);
    server = normalizeServerUrl(typed);
}

bool reachable(const std::string& url) {
    try {
        return nlohmann::json::parse(HTTP::get(url + "/settings", HTTP::Timeout{2000})).is_object();
    } catch (const std::exception& e) {
        brls::Logger::warning("Stremio streaming server {}: {}", url, e.what());
        return false;
    }
}

std::string open(const std::string& key) {
    TorrentStream t;
    if (!parseStreamKey(key, t)) return key;
    const std::string url = configured();
    if (url.empty()) throw std::runtime_error("main/stremio/server/none"_i18n);
    if (!reachable(url)) throw std::runtime_error(brls::getStr("main/stremio/server/unreachable", url));

    nlohmann::json stats;
    try {
        stats = nlohmann::json::parse(HTTP::post(url + "/" + t.infoHash + "/create", createBody(t).dump(),
            HTTP::Header{"Content-Type: application/json"}, HTTP::Timeout{15000}));
    } catch (const std::exception& e) {
        brls::Logger::error("Stremio streaming server {}: create {}: {}", url, t.infoHash, e.what());
        throw std::runtime_error(brls::getStr("main/stremio/server/open_failed", e.what()));
    }
    int file = chooseFile(t, stats);
    // the file list comes with the torrent's metadata: until then, ask again
    for (int i = 0; file < 0 && i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            stats = nlohmann::json::parse(HTTP::get(url + "/" + t.infoHash + "/stats.json", HTTP::Timeout{5000}));
        } catch (const std::exception&) {
            continue;
        }
        file = chooseFile(t, stats);
    }
    if (file < 0) throw std::runtime_error(brls::getStr("main/stremio/server/open_failed", "no file list after 30 s"));
    brls::Logger::info("Stremio streaming server {}: {} file {}", url, t.infoHash, file);
    return streamUrl(url, t.infoHash, file);
}

}  // namespace stremio::streaming
