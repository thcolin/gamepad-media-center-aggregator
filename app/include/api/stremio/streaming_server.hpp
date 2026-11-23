// A Stremio streaming server set in Settings (Network, "Stremio streaming server"): torrent
// streams play through it, as in Stremio's own apps, on every platform. Without one they stay
// unplayable as before (no torrent client on the device).
//
// A torrent row's part key is "stremio-torrent:<info hash>/<file index>[/<file name>][?tr=<tracker>
// ...]": no server in it (the setting can change while a page is open), the file name last so a
// download gets its extension from the path. At play or download time open() registers the
// torrent on the server and returns where the server streams it.
//
// The server's API, as Stremio's apps use it (checked against stremio/server 4.22): GET /settings
// answers when it is there; POST /<hash>/create with the stream's trackers registers the torrent
// and, when the stream names no file, guesses one; GET /<hash>/<file index> streams it. File index
// -1 isn't one it takes (HTTP 500), and it ignores a URL's tr=, so the trackers go in the create
// call. The pure parts are here (tests/test_streaming_server.cpp); the requests are in
// streaming_server.cpp.
#pragma once

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace stremio::streaming {

/// One torrent stream of an addon: what the server needs to play it.
struct TorrentStream {
    std::string infoHash;               // 40 lowercase hex
    int fileIdx = -1;                   // -1: the addon named none
    std::vector<std::string> trackers;  // announce URLs
    std::string filename;               // the addon's behaviorHints.filename, or ""
};

constexpr const char* keyScheme = "stremio-torrent:";

/// More trackers than this in one stream is an addon bug, not more peers; a tracker or a file
/// name longer than this is left out, so a key stays short.
constexpr size_t maxTrackers = 16;
constexpr size_t maxFieldLength = 200;

inline bool isInfoHash(const std::string& h) {
    if (h.size() != 40) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline std::string percentEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

inline std::string percentDecode(const std::string& s) {
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += char(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/// The tracker URLs among a stream's `sources` ("tracker:<url>", "dht:<info hash>"), without
/// duplicates or over-long ones, at most maxTrackers.
inline std::vector<std::string> trackersFromSources(const std::vector<std::string>& sources) {
    const std::string prefix = "tracker:";
    std::vector<std::string> out;
    for (const auto& s : sources) {
        if (s.compare(0, prefix.size(), prefix) != 0) continue;
        std::string url = s.substr(prefix.size());
        if (url.empty() || url.size() > maxFieldLength) continue;
        bool seen = false;
        for (const auto& o : out) seen = seen || o == url;
        if (seen) continue;
        out.push_back(url);
        if (out.size() == maxTrackers) break;
    }
    return out;
}

/// A torrent row's part key; "" when the info hash isn't one (the row then stays unplayable).
inline std::string streamKey(TorrentStream t) {
    for (char& c : t.infoHash) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!isInfoHash(t.infoHash)) return "";
    std::string key = std::string(keyScheme) + t.infoHash + "/" + std::to_string(t.fileIdx < 0 ? -1 : t.fileIdx);
    if (!t.filename.empty() && t.filename.size() <= maxFieldLength) key += "/" + percentEncode(t.filename);
    char sep = '?';
    for (const auto& tr : t.trackers) {
        key += sep;
        key += "tr=" + percentEncode(tr);
        sep = '&';
    }
    return key;
}

/// The stream inside a part key; false for any other key.
inline bool parseStreamKey(const std::string& key, TorrentStream& out) {
    const std::string scheme = keyScheme;
    if (key.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = key.substr(scheme.size());
    std::string query;
    const size_t q = rest.find('?');
    if (q != std::string::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }
    TorrentStream t;
    const size_t a = rest.find('/');
    if (a == std::string::npos) return false;
    t.infoHash = rest.substr(0, a);
    if (!isInfoHash(t.infoHash)) return false;
    const size_t b = rest.find('/', a + 1);
    const std::string index = rest.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (index.empty() || index.size() > 7) return false;
    if (index == "-1") {
        t.fileIdx = -1;
    } else {
        for (char c : index)
            if (c < '0' || c > '9') return false;
        t.fileIdx = std::stoi(index);
    }
    if (b != std::string::npos) t.filename = percentDecode(rest.substr(b + 1));
    size_t i = 0;
    while (i < query.size()) {
        size_t e = query.find('&', i);
        if (e == std::string::npos) e = query.size();
        const std::string part = query.substr(i, e - i);
        if (part.compare(0, 3, "tr=") == 0) t.trackers.push_back(percentDecode(part.substr(3)));
        i = e + 1;
    }
    out = t;
    return true;
}

/// The address as typed, made usable: trimmed, "http://" in front when it has no scheme, no
/// trailing slash. "" for an empty one, and for one that isn't http(s) or has no host.
inline std::string normalizeServerUrl(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return "";
    const size_t scheme = s.find("://");
    if (scheme == std::string::npos) {
        s = "http://" + s;
    } else {
        std::string name = s.substr(0, scheme);
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != "http" && name != "https") return "";
        s = name + s.substr(scheme);
    }
    const size_t host = s.find("://") + 3;
    while (s.size() > host && s.back() == '/') s.pop_back();
    if (host >= s.size() || s[host] == ':' || s[host] == '/') return "";
    for (size_t i = host; i < s.size(); ++i)
        if (std::isspace(static_cast<unsigned char>(s[i]))) return "";
    return s;
}

/// The body of POST /<hash>/create: the trackers as "tracker:<url>" sources and DHT, as Stremio's
/// apps send them, and a file guess when the stream names no file.
inline nlohmann::json createBody(const TorrentStream& t) {
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& tr : t.trackers) sources.push_back("tracker:" + tr);
    sources.push_back("dht:" + t.infoHash);
    nlohmann::json body = {
        {"torrent", {{"infoHash", t.infoHash}}},
        {"peerSearch", {{"sources", sources}, {"min", 40}, {"max", 150}}},
    };
    if (t.fileIdx < 0) body["guessFileIdx"] = nlohmann::json::object();
    return body;
}

/// The file to stream, from what the server says of the torrent (`stats`, the create call's
/// answer or /<hash>/stats.json): the stream's own file, else the server's guess, else the file
/// the stream names, else the largest. -1 while the server has no file list yet (the torrent's
/// metadata hasn't come).
inline int chooseFile(const TorrentStream& t, const nlohmann::json& stats) {
    if (t.fileIdx >= 0) return t.fileIdx;
    auto guessed = stats.find("guessedFileIdx");
    if (guessed != stats.end() && guessed->is_number_integer() && guessed->get<int>() >= 0) return guessed->get<int>();
    auto files = stats.find("files");
    if (files == stats.end() || !files->is_array() || files->empty()) return -1;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto baseName = [](const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    if (!t.filename.empty()) {
        const std::string want = lower(baseName(t.filename));
        for (size_t i = 0; i < files->size(); ++i) {
            const auto& f = (*files)[i];
            if (f.is_object() && f.contains("name") && f["name"].is_string() &&
                lower(baseName(f["name"].get<std::string>())) == want)
                return int(i);
        }
    }
    int best = -1;
    double bestLength = -1;
    for (size_t i = 0; i < files->size(); ++i) {
        const auto& f = (*files)[i];
        if (!f.is_object() || !f.contains("length") || !f["length"].is_number()) continue;
        const double length = f["length"].get<double>();
        if (length > bestLength) {
            best = int(i);
            bestLength = length;
        }
    }
    return best;
}

/// Where the server streams one file of a torrent.
inline std::string streamUrl(const std::string& server, const std::string& infoHash, int fileIdx) {
    return server + "/" + infoHash + "/" + std::to_string(fileIdx);
}

// ---- streaming_server.cpp ------------------------------------------------------------------

/// The server set in Settings, normalized; "" when there's none. configure() takes the setting
/// as typed (at startup and when Settings changes it). Any thread.
std::string configured();
void configure(const std::string& typed);

/// The server answers GET /settings within 2 s. Blocks: a pool or async thread.
bool reachable(const std::string& server);

/// Where to fetch a part key from: the key itself when it isn't a torrent's; for a torrent, the
/// configured server's URL for it, once open there (the file list awaited up to 30 s when the
/// stream names no file). Throws, with a message for the user, when there is no server, it
/// doesn't answer, or it can't open the torrent. Blocks: a pool or async thread.
std::string open(const std::string& key);

}  // namespace stremio::streaming
