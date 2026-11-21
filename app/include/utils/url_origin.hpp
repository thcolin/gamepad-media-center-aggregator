#pragma once

/*
    Origin of a URL, "scheme://host:port" in lower case with the default port
    written out, so two URLs of the same server compare equal. HTTP follows
    redirects with it and the self-signed servers allowed by the user are
    listed by it. webdav(s):// is the http(s) URL the WebDAV client requests.

    Depends only on the STL, so tests/test_url_origin.cpp can exercise it.
*/

#include <algorithm>
#include <cctype>
#include <string>

namespace misc {

inline std::string urlOrigin(const std::string& url) {
    size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return "";
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string scheme = lower(url.substr(0, sep));
    if (scheme == "webdav") scheme = "http";
    if (scheme == "webdavs") scheme = "https";

    size_t start = sep + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(start, end - start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    std::string host = authority, port;
    size_t colon = authority.rfind(':');
    size_t bracket = authority.rfind(']');
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty()) return "";
    if (port.empty()) port = scheme == "https" ? "443" : scheme == "http" ? "80" : "";
    return scheme + "://" + lower(host) + (port.empty() ? "" : ":" + port);
}

}  // namespace misc
