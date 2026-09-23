#pragma once

/*
    Redaction of mpv/ffmpeg log lines before they reach an on-screen dialog.
    Users post screenshots of it: URLs carry tokens in the query (Plex, Jellyfin),
    API keys in the path (Stremio debrid addons) and the server's public IP in
    the host (*.plex.direct). Only the scheme and the port survive.

    Depends only on the STL, so tests/test_log_redact.cpp can exercise it.
*/

#include <cctype>
#include <cstring>
#include <string>

namespace misc {

inline std::string redactLogLine(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

    for (size_t pos = 0; (pos = text.find("://", pos)) != std::string::npos;) {
        size_t start = pos;
        while (start > 0 && std::isalpha(static_cast<unsigned char>(text[start - 1]))) --start;
        size_t end = text.find_first_of(" \t\"'<>", pos);
        if (end == std::string::npos) end = text.size();
        size_t hostEnd = text.find_first_of("/?#", pos + 3);
        if (hostEnd == std::string::npos || hostEnd > end) hostEnd = end;
        size_t colon = text.rfind(':', hostEnd);
        std::string port = colon > pos + 2 && colon < hostEnd ? text.substr(colon, hostEnd - colon) : "";
        std::string redacted = text.substr(start, pos - start) + "://<host>" + port + (hostEnd < end ? "/…" : "");
        text.replace(start, end - start, redacted);
        pos = start + redacted.size();
    }

    for (const char* key : {"X-Plex-Token=", "api_key=", "token="}) {
        for (size_t pos = 0; (pos = text.find(key, pos)) != std::string::npos;) {
            pos += std::strlen(key);
            size_t end = text.find_first_of("&\"' \t", pos);
            text.replace(pos, (end == std::string::npos ? text.size() : end) - pos, "***");
            pos += 3;
        }
    }
    return text;
}

}  // namespace misc
