/*
    GMCA — pure URL host helpers (no HTTP / Borealis dependency, so the logic is
    unit-testable standalone; see tests/test_hostname.cpp). Used by
    utils/net_error.hpp to name and classify the host a server URL points at.
*/

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace net {

/// Host portion of a URL: text between the scheme and the first '/', '?' or
/// ':', minus any userinfo, with IPv6 brackets stripped. Best-effort — only
/// used to name the offending host back to the user.
inline std::string hostFromUrl(const std::string& url) {
    std::string s = url;
    auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    s = s.substr(0, s.find_first_of("/?"));  // drop path/query
    auto at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);  // drop userinfo
    if (!s.empty() && s.front() == '[') {               // literal IPv6 [::1]:port
        auto close = s.find(']');
        return close == std::string::npos ? s.substr(1) : s.substr(1, close - 1);
    }
    auto colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(0, colon);  // drop :port
    return s;
}

/// True when the host is an mDNS `.local` name — unresolvable on console by
/// construction, so the add form can warn before even attempting a request.
/// LAN-only unicast names and IPv6-only hosts also fail, but only at resolve
/// time; those are caught reactively via HTTP::ErrorKind::ResolveFailed.
inline bool isMdnsHost(const std::string& url) {
    std::string host = hostFromUrl(url);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!host.empty() && host.back() == '.') host.pop_back();  // tolerate a trailing dot
    const std::string suffix = ".local";
    return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace net
