/*
    GMCA — Torrent metadata & magnet parsing (see torrent/metadata.hpp).
*/

#include "torrent/metadata.hpp"

#include <algorithm>
#include <cctype>

#include "torrent/bencode.hpp"
#include "torrent/log.hpp"

namespace torrent {

std::string infoHashHex(const InfoHash& h) { return toHex(h); }

// ---- hex / base32 helpers ---------------------------------------------------

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decodeHex40(const std::string& s, InfoHash& out) {
    if (s.size() != 40) return false;
    for (int i = 0; i < 20; i++) {
        int hi = hexNibble(s[i * 2]);
        int lo = hexNibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool decodeBase32_32(const std::string& in, InfoHash& out) {
    // RFC 4648 base32, 32 chars -> 20 bytes (160 bits). Used by older magnets.
    if (in.size() != 32) return false;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    };
    uint64_t buffer = 0;
    int bits = 0;
    int oi = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) return false;
        buffer = (buffer << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out[oi++] = (uint8_t)((buffer >> bits) & 0xFF);
            if (oi == 20) break;
        }
    }
    return oi == 20;
}

// ---- percent-decode (magnet tr/ws values) -----------------------------------

static std::string urlDecode(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexNibble(s[i + 1]), lo = hexNibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                o += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+')
            o += ' ';
        else
            o += s[i];
    }
    return o;
}

// ---- magnet -----------------------------------------------------------------

bool parseMagnet(const std::string& input, MagnetInfo& out) {
    std::string s = input;
    // A bare infohash (hex-40 or base32-32) is also accepted.
    if (s.rfind("magnet:", 0) != 0) {
        if (decodeHex40(s, out.infoHash) || decodeBase32_32(s, out.infoHash)) {
            out.hasInfoHash = true;
            return true;
        }
        return false;
    }
    size_t q = s.find('?');
    if (q == std::string::npos) return false;
    std::string query = s.substr(q + 1);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        pos = (amp == std::string::npos) ? query.size() : amp + 1;
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = pair.substr(0, eq);
        std::string val = urlDecode(pair.substr(eq + 1));
        if (key == "xt") {
            // urn:btih:<hex40|base32-32>
            const std::string prefix = "urn:btih:";
            if (val.rfind(prefix, 0) == 0) {
                std::string ih = val.substr(prefix.size());
                if (decodeHex40(ih, out.infoHash) || decodeBase32_32(ih, out.infoHash)) out.hasInfoHash = true;
            }
        } else if (key == "dn") {
            out.displayName = val;
        } else if (key == "tr") {
            out.trackers.push_back(val);
        } else if (key == "ws") {
            out.webSeeds.push_back(val);
        }
    }
    return out.hasInfoHash;
}

// ---- info dict --------------------------------------------------------------

int TorrentMetadata::largestFileIndex() const {
    int best = -1;
    int64_t bestLen = -1;
    for (int i = 0; i < (int)files.size(); i++)
        if (files[i].length > bestLen) {
            bestLen = files[i].length;
            best = i;
        }
    return best;
}

bool parseInfoDict(const std::string& rawInfoDict, const InfoHash& expected, TorrentMetadata& out) {
    InfoHash computed = sha1(rawInfoDict);
    static const InfoHash zero{};
    if (expected != zero && expected != computed) {
        logWarn("metadata: infohash mismatch (got %s)", toHex(computed).c_str());
        return false;
    }
    out.infoHash = computed;

    bencode::Value info;
    if (!bencode::decode(rawInfoDict, info) || !info.isDict()) return false;

    out.name = info.strAt("name");
    out.pieceLength = info.intAt("piece length");

    const bencode::Value* pieces = info.find("pieces");
    if (!pieces || !pieces->isStr() || pieces->s.size() % 20 != 0) return false;
    size_t n = pieces->s.size() / 20;
    out.pieceHashes.resize(n);
    for (size_t i = 0; i < n; i++)
        std::copy_n(reinterpret_cast<const uint8_t*>(pieces->s.data() + i * 20), 20, out.pieceHashes[i].begin());

    out.files.clear();
    const bencode::Value* filesv = info.find("files");
    if (filesv && filesv->isList()) {
        int64_t offset = 0;
        for (const auto& f : filesv->list) {
            FileEntry fe;
            fe.length = f.intAt("length");
            fe.offset = offset;
            const bencode::Value* pathv = f.find("path");
            std::string joined = out.name;  // multi-file: name is the directory
            if (pathv && pathv->isList())
                for (const auto& seg : pathv->list)
                    if (seg.isStr()) joined += "/" + seg.s;
            fe.path = joined;
            out.files.push_back(std::move(fe));
            offset += fe.length;
        }
        out.totalLength = offset;
    } else {
        FileEntry fe;
        fe.length = info.intAt("length");
        fe.offset = 0;
        fe.path = out.name;
        out.totalLength = fe.length;
        out.files.push_back(std::move(fe));
    }

    if (!out.valid()) {
        logWarn("metadata: parsed info dict is invalid (pieceLen=%lld total=%lld pieces=%zu)",
            (long long)out.pieceLength, (long long)out.totalLength, out.pieceHashes.size());
        return false;
    }
    return true;
}

// ---- .torrent file ----------------------------------------------------------

static void collectWebSeeds(const bencode::Value& top, TorrentMetadata& out) {
    const bencode::Value* ul = top.find("url-list");  // BEP-19
    if (!ul) return;
    if (ul->isStr() && !ul->s.empty())
        out.webSeeds.push_back(ul->s);
    else if (ul->isList())
        for (const auto& e : ul->list)
            if (e.isStr() && !e.s.empty()) out.webSeeds.push_back(e.s);
}

static void collectTrackers(const bencode::Value& top, TorrentMetadata& out) {
    const bencode::Value* al = top.find("announce-list");  // BEP-12
    if (al && al->isList()) {
        for (const auto& tier : al->list) {
            if (!tier.isList()) continue;
            std::vector<std::string> t;
            for (const auto& u : tier.list)
                if (u.isStr()) t.push_back(u.s);
            if (!t.empty()) out.trackerTiers.push_back(std::move(t));
        }
    }
    if (out.trackerTiers.empty()) {
        std::string a = top.strAt("announce");
        if (!a.empty()) out.trackerTiers.push_back({a});
    }
}

bool parseTorrentFile(const std::string& raw, TorrentMetadata& out) {
    bencode::Value top;
    if (!bencode::decode(raw, top) || !top.isDict()) return false;
    const bencode::Value* info = top.find("info");
    if (!info || !info->isDict()) return false;

    // Raw bytes of the info value -> infohash (no canonical re-encode needed).
    std::string rawInfo = raw.substr(info->begin, info->end - info->begin);
    static const InfoHash zero{};
    if (!parseInfoDict(rawInfo, zero, out)) return false;

    collectTrackers(top, out);
    collectWebSeeds(top, out);
    return true;
}

}  // namespace torrent
