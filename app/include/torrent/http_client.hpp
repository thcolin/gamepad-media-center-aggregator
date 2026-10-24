// Blocking libcurl GETs for HTTP trackers and web seeds (BEP-19). Never call
// from the peer event loop.

#pragma once

#include <cstdint>
#include <string>

namespace torrent {
namespace http {

struct Response {
    long status = 0;    // 0 on transport failure
    std::string body;
    std::string error;  // set when status == 0
    bool ok() const { return status >= 200 && status < 300; }
};

Response get(const std::string& url, int timeoutMs = 15000);

/// [first, last] inclusive. A server may ignore the range and answer 200 with
/// the whole file: check the length.
Response getRange(const std::string& url, int64_t first, int64_t last, int timeoutMs = 30000);

std::string urlEncode(const std::string& raw);

}  // namespace http
}  // namespace torrent
