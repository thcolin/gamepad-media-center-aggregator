#pragma once

/*
    Redaction of mpv/ffmpeg log lines before they reach an on-screen dialog.
    Users post screenshots of it: URLs carry tokens in the query (Plex, Jellyfin),
    API keys in the path (Stremio debrid addons), credentials in the userinfo and
    the server's public IP in the host (*.plex.direct). Only the scheme and the
    port of a URL survive; bare host names and IPv4 addresses are masked too.
    The text comes from remote servers: control characters are dropped and the
    line is capped.

    Depends only on the STL, so tests/test_log_redact.cpp can exercise it.
*/

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace misc {

namespace detail {

inline bool isWordBreak(char c) { return std::isspace(static_cast<unsigned char>(c)) || std::strchr("\"'<>()[],;", c); }

inline bool isIPv4(const std::string& w) {
    int dots = 0, digits = 0;
    for (char c : w) {
        if (c == '.') {
            if (digits == 0) return false;
            ++dots;
            digits = 0;
        } else if (std::isdigit(static_cast<unsigned char>(c)) && ++digits <= 3) {
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0;
}

inline size_t ifind(const std::string& text, const std::string& key, size_t from) {
    auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
    auto it = std::search(text.begin() + from, text.end(), key.begin(), key.end(),
        [&](char a, char b) { return lower(a) == lower(b); });
    return it == text.end() ? std::string::npos : size_t(it - text.begin());
}

}  // namespace detail

inline std::string redactLogLine(std::string text) {
    constexpr size_t maxBytes = 200;

    std::string clean;
    for (char c : text) {
        if (c == '\t') c = ' ';
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) clean += c;
    }
    text = clean;
    while (!text.empty() && text.back() == ' ') text.pop_back();

    for (size_t pos = 0; (pos = text.find("://", pos)) != std::string::npos;) {
        size_t start = pos;
        while (start > 0 && std::isalpha(static_cast<unsigned char>(text[start - 1]))) --start;
        size_t end = text.find_first_of(" \"'<>", pos);
        if (end == std::string::npos) end = text.size();
        size_t hostEnd = text.find_first_of("/?#", pos + 3);
        if (hostEnd == std::string::npos || hostEnd > end) hostEnd = end;
        size_t at = text.rfind('@', hostEnd);
        size_t hostStart = at != std::string::npos && at > pos ? at + 1 : pos + 3;
        size_t colon = text.rfind(':', hostEnd);
        bool hasPort = colon != std::string::npos && colon >= hostStart && colon + 1 < hostEnd &&
                       std::all_of(text.begin() + colon + 1, text.begin() + hostEnd,
                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        std::string redacted = text.substr(start, pos - start) + "://<host>" +
                               (hasPort ? text.substr(colon, hostEnd - colon) : "") + (hostEnd < end ? "/…" : "");
        text.replace(start, end - start, redacted);
        pos = start + redacted.size();
    }

    for (size_t start = 0; start < text.size();) {
        if (detail::isWordBreak(text[start])) {
            ++start;
            continue;
        }
        size_t end = start;
        while (end < text.size() && !detail::isWordBreak(text[end])) ++end;
        std::string word = text.substr(start, end - start);
        std::string bare = word;
        while (!bare.empty() && (bare.back() == ':' || bare.back() == '.')) bare.pop_back();
        bool afterHostname = start >= 9 && text.compare(start - 9, 9, "hostname ") == 0;
        if (afterHostname || detail::isIPv4(bare) || detail::ifind(bare, ".plex.direct", 0) != std::string::npos) {
            std::string redacted = "<host>" + word.substr(bare.size());
            text.replace(start, end - start, redacted);
            end = start + redacted.size();
        }
        start = end;
    }

    for (const char* key : {"token=", "api_key=", "apikey=", "token%3D", "api_key%3D", "apikey%3D"}) {
        for (size_t pos = 0; (pos = detail::ifind(text, key, pos)) != std::string::npos;) {
            pos += std::strlen(key);
            size_t end = text.find_first_of("&\"' ;,)", pos);
            size_t encodedAmp = detail::ifind(text, "%26", pos);
            if (encodedAmp != std::string::npos && (end == std::string::npos || encodedAmp < end)) end = encodedAmp;
            text.replace(pos, (end == std::string::npos ? text.size() : end) - pos, "***");
            pos += 3;
        }
    }

    if (text.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text = text.substr(0, cut) + "…";
    }
    return text;
}

}  // namespace misc
