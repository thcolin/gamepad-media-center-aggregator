/*
    GMCA — user-facing text for server-connection failures.

    A game console can't resolve every hostname a PC can: libnx (Switch) and
    SceNet (Vita) ship no mDNS responder, so `.local` names never resolve; both
    stacks are IPv4-only (Switch registers only AF_INET/AF_ROUTE, the Vita's
    SceNet has no IPv6 at all), so an IPv6-only host never connects; and a Switch
    pointed at a public DNS (8.8.8.8…) can't see a LAN-only unicast name its
    router would. All three surface as curl's terse "Couldn't resolve host name".

    Rather than dump that raw, these helpers name the host and tell the user the
    one thing that always works on console — the server's raw IPv4 address. The
    pure host parsing lives in utils/hostname.hpp (dependency-free, unit-tested).
*/

#pragma once

#include "api/http.hpp"
#include "utils/hostname.hpp"

#include <borealis/core/i18n.hpp>
#include <fmt/format.h>

#include <string>

namespace net {

/// Actionable guidance for a DNS dead-end: names the host and points the user at
/// the server's raw IPv4 address (proactive `.local` catch and reactive resolve
/// failure share it — the advice is the same either way).
inline std::string dnsHelp(const std::string& url) {
    using namespace brls::literals;
    return fmt::format(fmt::runtime("main/server/dns_unresolved"_i18n), hostFromUrl(url));
}

/// A host DNS resolution failure becomes the IP-address hint; any other failure
/// (HTTP status, TLS, connect, timeout, and a proxy-resolution failure — which
/// is about the proxy, not the server URL) passes through unchanged.
inline std::string errorText(const HTTP::Error& ex, const std::string& url) {
    return ex.kind == HTTP::ErrorKind::ResolveFailed ? dnsHelp(url) : ex.what();
}

}  // namespace net
