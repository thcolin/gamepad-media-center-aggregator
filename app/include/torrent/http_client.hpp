/*
    GMCA — Minimal blocking HTTP client (libcurl) for trackers & web seeds.

    HTTP trackers (BEP-3/23) and web seeds (BEP-19) are plain HTTP GETs — better
    served by libcurl (TLS, redirects, DNS, chunked) than by hand over the socket
    shim. libcurl is linked on every target the app ships to (switch-curl,
    vita curl, openorbis, desktop), so this stays portable.

    Calls block, so the engine issues them from worker threads (the announcer and
    the web-seed worker), never from the peer event loop.
*/

#pragma once

#include <cstdint>
#include <string>

namespace torrent {
namespace http {

struct Response {
    long status = 0;    // HTTP status code, 0 on transport failure
    std::string body;   // raw bytes (bencode for trackers, file bytes for web seeds)
    std::string error;  // curl error string when status == 0
    bool ok() const { return status >= 200 && status < 300; }
};

/// GET a URL. `binary` is informational only (body is always raw bytes).
Response get(const std::string& url, int timeoutMs = 15000);

/// GET a byte range [first, last] inclusive (BEP-19 web seed). Servers may honor
/// it with 206 Partial Content or ignore it (200 = whole file) — the caller must
/// check the returned length.
Response getRange(const std::string& url, int64_t first, int64_t last, int timeoutMs = 30000);

/// URL-encode a raw byte string for tracker query params (info_hash/peer_id).
std::string urlEncode(const std::string& raw);

}  // namespace http
}  // namespace torrent
