/*
    GMCA — plex.tv authentication and server discovery.
    Specification: PLEX_MIGRATION.md §2.2 (PIN flow) and §2.3 (resources + connections).

    All functions are SYNCHRONOUS (they chain HTTP requests): call them from
    brls::async, like the Quick Connect flow used to.
    They throw std::runtime_error on network/HTTP failure.
*/

#pragma once

#include "api/plex/types.hpp"
#include <functional>

namespace plex {

/// Creates a 4-character link PIN that the user enters on
/// https://plex.tv/link.
PinResult requestPin();

/// Polls the PIN: returns the account token once the user has validated,
/// empty string until then. Throws if the PIN has expired (404/410).
/// Recommended cadence: 1 s -> 2 s -> 4 s, capped at 5 s, give up at 2 min (§2.2).
std::string pollPin(int64_t pinId);

/// Validates a token (GET /api/v2/user). Throws if invalid/revoked (401/403).
AccountUser getUser(const std::string& accountToken);

/// Servers of the account with their own access tokens (§2.3).
std::vector<ServerResource> getResources(const std::string& accountToken);

/// Plex Home profiles of the account.
std::vector<HomeUser> getHomeUsers(const std::string& accountToken);

/// Switches to a Home profile: returns the token SPECIFIC to that profile.
/// `pin` required when HomeUser::isProtected (error 1041 = wrong PIN).
std::string switchHomeUser(const std::string& accountToken, const std::string& userUuid, const std::string& pin = "");

/// Probes a base URL (GET {base}/ with token); returns true on 200.
/// `connectMs` bounds the connect+DNS phase so an unreachable host fails fast,
/// while the larger `timeoutMs` lets a reachable but high-latency endpoint
/// finish its TLS handshake + response (GH #36).
bool probeConnection(const std::string& baseUrl, const std::string& accessToken, long timeoutMs = 5000,
    long connectMs = 2000);

/// Candidate base URLs of a server, ordered by priority (https+local ->
/// https+remote -> https+relay -> http...) WITHOUT probing any of them.
/// Used to persist a server's connection list ahead of a lazy probe at switch
/// time; raceConnections probes this list.
std::vector<std::string> rankConnections(const ServerResource& server);

/// Probes `urls` (a priority-ordered candidate list) CONCURRENTLY and returns
/// the highest-priority one that answers, or "". Racing matters off-network:
/// probed serially, every unreachable LAN address (ranked first) had to hit its
/// connect timeout before a reachable remote/relay endpoint was even tried, so a
/// roaming connect stalled for many seconds or gave up (GH #36). Priority is
/// still honoured — a lower-ranked candidate wins only once every better one has
/// failed. Probes each candidate with probeConnection.
std::string raceConnections(const std::vector<std::string>& urls, const std::string& accessToken);

/// Same race with the backend's own `probe` (Jellyfin/Emby answer on their API,
/// not on the Plex root).
std::string raceConnections(const std::vector<std::string>& urls, const std::function<bool(const std::string&)>& probe);

/// Picks the best connection for a server: races `preferredUri` (if any) ahead
/// of the ranked candidates and returns the first reachable base URL, or "".
std::string findBestConnection(const ServerResource& server, const std::string& preferredUri = "");

}  // namespace plex
