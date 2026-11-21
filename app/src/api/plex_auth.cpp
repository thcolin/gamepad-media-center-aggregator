/*
    GMCA — plex.tv authentication implementation.
    Specification: PLEX_MIGRATION.md §2.2-2.3.
*/

#include "api/plex/auth.hpp"
#include "api/plex.hpp"
#include "utils/thread.hpp"
#include "utils/url_origin.hpp"
#include <cctype>
#include <cstdio>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace plex {

/// True when `address` is an RFC1918 / CGNAT / loopback / IPv6-ULA host — i.e.
/// an IP whose plex.direct alias resolves to a non-public address, which routers
/// with DNS-rebinding protection refuse to resolve. We probe such addresses
/// directly (raw IP, bypassing plex.direct) so a LAN/VPN server stays reachable
/// (GH #3). Public addresses gain nothing — plex.direct resolves to the same IP.
/// IPv6 link-local (fe80::) and unspecified (::) are filtered by the caller.
/// Runs `fn` against the primary API (clients.plex.tv) then retries on
/// plex.tv in case of transient failure.
template <typename Fn>
static auto withFallback(Fn&& fn) {
    try {
        return fn(tvApiBase);
    } catch (const std::exception& ex) {
        brls::Logger::warning("plex.tv api primaire en échec ({}), repli sur {}", ex.what(), tvApiFallback);
        return fn(tvApiFallback);
    }
}

PinResult requestPin() {
    return withFallback([](const std::string& base) {
        // POST {base}/pins, empty body — weak 4-character PIN for
        // plex.tv/link (cf. tvApiPins comment)
        std::string url = fmt::format(fmt::runtime(tvApiPins), base);
        return postSync(url, "").get<PinResult>();
    });
}

std::string pollPin(int64_t pinId) {
    // 404/410 (expired PIN) surfaces as an "http status 4xx" exception
    return withFallback([pinId](const std::string& base) {
        std::string url = fmt::format(fmt::runtime(tvApiPinPoll), base, pinId);
        return getSync(url, "").get<PinResult>().authToken;
    });
}

AccountUser getUser(const std::string& accountToken) {
    return withFallback([&accountToken](const std::string& base) {
        std::string url = fmt::format(fmt::runtime(tvApiUser), base);
        return getSync(url, accountToken).get<AccountUser>();
    });
}

std::vector<ServerResource> getResources(const std::string& accountToken) {
    return withFallback([&accountToken](const std::string& base) {
        std::string url = fmt::format(fmt::runtime(tvApiResources), base);
        nlohmann::json j = getSync(url, accountToken, 10000);

        // The response is an array of resources; keep only provides=server
        std::vector<ServerResource> servers;
        if (!j.is_array()) return servers;
        for (auto& e : j) {
            if (jstr(e, "provides").find("server") == std::string::npos) continue;
            ServerResource s = e.get<ServerResource>();
            if (!s.clientIdentifier.empty() && !s.connections.empty()) servers.push_back(std::move(s));
        }
        return servers;
    });
}

std::vector<HomeUser> getHomeUsers(const std::string& accountToken) {
    return withFallback([&accountToken](const std::string& base) {
        std::string url = fmt::format(fmt::runtime(tvApiHomeUsers), base);
        nlohmann::json j = getSync(url, accountToken);
        std::vector<HomeUser> users;
        // v2 returns an object containing `users`, some deployments a bare array
        const nlohmann::json& arr = j.contains("users") ? j.at("users") : j;
        if (arr.is_array()) users = arr.get<std::vector<HomeUser>>();
        return users;
    });
}

std::string switchHomeUser(const std::string& accountToken, const std::string& userUuid, const std::string& pin) {
    // POST {base}/home/users/{uuid}/switch — parameters in query, empty body
    // (wrong PIN = 403 with code 1041)
    HTTP::Form form = {
        {"includeSubscriptions", "1"},
        {"includeProviders", "1"},
        {"includeSettings", "1"},
        {"includeSharedSettings", "1"},
        {"X-Plex-Language", "en"},
    };
    if (!pin.empty()) form["pin"] = pin;
    std::string query = HTTP::encode_form(form);

    return withFallback([&](const std::string& base) {
        std::string url = fmt::format(fmt::runtime(tvApiSwitchUser), base, userUuid, query);
        nlohmann::json j = postSync(url, accountToken);
        std::string token = jstr(j, "authToken");
        if (token.empty()) throw std::runtime_error("switchHomeUser: authToken absent de la réponse");
        return token;
    });
}

bool probeConnection(const std::string& baseUrl, const std::string& accessToken, long timeoutMs, long connectMs) {
    // GET {base}/ WITH token: the root requires authentication, which
    // immediately detects a revoked token. A short connect budget lets an
    // unreachable LAN address fail fast; the larger total budget leaves room for
    // a reachable but high-latency remote/relay handshake (GH #36).
    try {
        HTTP::get(baseUrl + "/", headers(accessToken), HTTP::Timeout{timeoutMs, connectMs});
        return true;
    } catch (const std::exception& ex) {
        brls::Logger::debug("plex probe {} en échec : {}", baseUrl, ex.what());
        return false;
    }
}

std::string raceConnections(const std::vector<std::string>& urls, const std::string& accessToken) {
    return raceConnections(urls, [accessToken](const std::string& url) { return probeConnection(url, accessToken); });
}

std::string raceConnections(const std::vector<std::string>& urls, const std::function<bool(const std::string&)>& probe) {
    if (urls.empty()) return "";
    if (urls.size() == 1) return probe(urls.front()) ? urls.front() : "";

    // Probe every candidate at once on the shared thread pool, then return the
    // best-ranked one that answers. Priority is preserved without waiting on
    // dead candidates: a candidate wins as soon as it succeeds AND every
    // higher-ranked candidate has already failed. The state is heap-owned so the
    // still-running probes stay valid after we return early (they only touch
    // `state`, never this stack frame).
    enum Status { Pending, Reachable, Unreachable };
    struct RaceState {
        std::mutex mutex;
        std::condition_variable cond;
        std::vector<Status> status;
    };
    auto state = std::make_shared<RaceState>();
    state->status.assign(urls.size(), Pending);

    for (size_t i = 0; i < urls.size(); i++) {
        std::string url = urls[i];
        ThreadPool::instance().submit([state, url, probe, i](HTTP&) {
            // Guard against any stray throw so status[i] is always decided and
            // the waiter below never blocks forever (the probes already
            // swallow std::exception; this covers the pathological rest).
            Status result = Unreachable;
            try {
                if (probe(url)) result = Reachable;
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->status[i] = result;
            }
            state->cond.notify_all();
        });
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        size_t i = 0;
        for (; i < urls.size(); i++) {
            if (state->status[i] == Pending) break;         // best candidate not yet decided: keep waiting
            if (state->status[i] == Reachable) return urls[i];  // best decided candidate answers: winner
            // Unreachable: a better candidate is ruled out, look at the next one
        }
        if (i == urls.size()) return "";  // every candidate decided, none reachable
        state->cond.wait(lock);
    }
}

std::vector<std::string> rankConnections(const ServerResource& server) {
    // Priority by protocol + reachability: https+local -> https+remote ->
    // https+relay -> http+local -> ... (§2.3)
    auto rank = [](const std::string& protocol, bool local, bool relay) {
        int r = (protocol == "https") ? 0 : 3;
        if (relay)
            r += 2;
        else if (!local)
            r += 1;
        return r;
    };

    struct Candidate {
        std::string url;
        int rank;
    };
    std::vector<Candidate> candidates;
    auto add = [&](const std::string& url, const std::string& protocol, bool local, bool relay) {
        if (url.empty()) return;
        for (auto& e : candidates)
            if (e.url == url) return;  // dedupe
        candidates.push_back({url, rank(protocol, local, relay)});
    };

    for (auto& c : server.connections) {
        // Unreachable addresses: null or link-local IPv6
        if (c.address == "::" || c.address.rfind("fe80:", 0) == 0) continue;

        // Advertised URI: a plex.direct hostname for local/remote https servers.
        add(c.uri, c.protocol, c.local, c.relay);

        // DNS-rebinding protection refuses the plex.direct alias of a private
        // address (GH #3); HTTP connects to the address the alias names without
        // DNS (misc::plexDirectIp), so the https URI above still works. For
        // private addresses, also try plain HTTP on the raw address:port, unless
        // the owner requires secure connections.
        if (misc::isPrivateAddress(c.address) && !server.httpsRequired) {
            bool ipv6 = c.address.find(':') != std::string::npos;
            std::string host = ipv6 ? fmt::format("[{}]", c.address) : c.address;
            add(fmt::format("http://{}:{}", host, c.port), "http", c.local, c.relay);
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });

    std::vector<std::string> urls;
    urls.reserve(candidates.size());
    for (auto& c : candidates) urls.push_back(c.url);
    return urls;
}

std::string findBestConnection(const ServerResource& server, const std::string& preferredUri) {
    // Race the remembered endpoint (if any) ahead of the ranked candidates.
    std::vector<std::string> urls;
    if (!preferredUri.empty()) urls.push_back(preferredUri);
    for (auto& url : rankConnections(server))
        if (url != preferredUri) urls.push_back(url);
    return raceConnections(urls, server.accessToken);
}

}  // namespace plex
