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
#include <cstdio>
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

/// RFC1918, CGNAT (Tailscale), loopback, link-local IPv4 and ULA IPv6 addresses.
inline bool isPrivateAddress(const std::string& address) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a <= 255 && b <= 255 &&
        c <= 255 && d <= 255) {
        if (a == 0 || a == 10 || a == 127) return true;     // this-host, RFC1918 /8, loopback
        if (a == 100 && b >= 64 && b <= 127) return true;   // CGNAT 100.64/10 (Tailscale)
        if (a == 169 && b == 254) return true;              // link-local 169.254/16
        if (a == 172 && b >= 16 && b <= 31) return true;    // RFC1918 172.16/12
        if (a == 192 && b == 168) return true;              // RFC1918 192.168/16
        return false;
    }
    if (address.find(':') != std::string::npos) {  // IPv6
        if (address == "::1") return true;          // loopback
        char p0 = (char)std::tolower((unsigned char)address[0]);
        char p1 = address.size() > 1 ? (char)std::tolower((unsigned char)address[1]) : '\0';
        if (p0 == 'f' && (p1 == 'c' || p1 == 'd')) return true;  // ULA fc00::/7 (incl. Tailscale fd7a::)
    }
    return false;
}

/// The IPv4 address a Plex "a-b-c-d.<hash>.plex.direct" URL names, "" for any other URL.
/// Plex's DNS answers that name with that address, so it can be reached without DNS.
inline std::string plexDirectIp(const std::string& url) {
    std::string origin = urlOrigin(url);
    size_t start = origin.find("://");
    if (start == std::string::npos) return "";
    start += 3;
    size_t colon = origin.rfind(':');
    std::string host = origin.substr(start, colon > start ? colon - start : std::string::npos);
    const std::string suffix = ".plex.direct";
    if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0)
        return "";
    std::string label = host.substr(0, host.find('.'));
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(label.c_str(), "%u-%u-%u-%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
        d > 255)
        return "";
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}

}  // namespace misc
