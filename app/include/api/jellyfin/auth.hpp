/*
    GMCA — Jellyfin/Emby authentication (username/password login).
    Synchronous helpers (call from brls::async); throw std::runtime_error on failure.
    Quick Connect could be added later (GET /QuickConnect/Initiate -> poll).
*/

#pragma once

#include <string>

namespace jellyfin {

struct LoginResult {
    std::string token;       // AccessToken
    std::string userId;      // Jellyfin user Id (for /Users/{u}/...)
    std::string userName;
    std::string serverId;
    std::string serverName;
};

/// GET /System/Info/Public — validates the server, returns its name (throws on failure).
std::string getServerName(const std::string& baseUrl);

/// GET /System/Info/Public — true when `baseUrl` answers as a Jellyfin/Emby
/// server. Same budgets as plex::probeConnection (GH #36); the web client at
/// the root is not probed, a reverse proxy may serve the API alone.
bool probeConnection(const std::string& baseUrl, long timeoutMs = 5000, long connectMs = 2000);

/// POST /Users/AuthenticateByName — username/password login.
LoginResult login(const std::string& baseUrl, const std::string& user, const std::string& pass);

}  // namespace jellyfin
