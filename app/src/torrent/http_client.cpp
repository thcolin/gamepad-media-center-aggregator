/*
    GMCA — libcurl HTTP client (see torrent/http_client.hpp).
*/

#include "torrent/http_client.hpp"

#include <curl/curl.h>

#include <mutex>

namespace torrent {
namespace http {

namespace {
std::once_flag g_curlOnce;
void ensureCurl() {
    std::call_once(g_curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

Response perform(const std::string& url, const char* range, int timeoutMs) {
    ensureCurl();
    Response resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        return resp;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // no transparent gzip on binary
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GMCA-torrent/0.1");
    if (range) curl_easy_setopt(curl, CURLOPT_RANGE, range);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    }
    curl_easy_cleanup(curl);
    return resp;
}
}  // namespace

Response get(const std::string& url, int timeoutMs) { return perform(url, nullptr, timeoutMs); }

Response getRange(const std::string& url, int64_t first, int64_t last, int timeoutMs) {
    std::string range = std::to_string(first) + "-" + std::to_string(last);
    return perform(url, range.c_str(), timeoutMs);
}

std::string urlEncode(const std::string& raw) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (unsigned char c : raw) {
        // RFC 3986 unreserved set stays literal; everything else is %XX.
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

}  // namespace http
}  // namespace torrent
