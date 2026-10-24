#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/types.hpp"

namespace torrent {

struct FileEntry {
    std::string path;    // joined with '/'
    int64_t length = 0;
    int64_t offset = 0;  // within the concatenation of all files
};

struct TorrentMetadata {
    InfoHash infoHash{};
    std::string name;
    int64_t pieceLength = 0;
    int64_t totalLength = 0;
    std::vector<Sha1Digest> pieceHashes;
    std::vector<FileEntry> files;
    std::vector<std::string> webSeeds;                   // BEP-19 url-list entries
    std::vector<std::vector<std::string>> trackerTiers;  // BEP-12 announce-list

    bool valid() const { return pieceLength > 0 && !pieceHashes.empty() && totalLength > 0; }
    int numPieces() const { return (int)pieceHashes.size(); }

    const FileEntry* file(int idx) const {
        if (idx < 0 || idx >= (int)files.size()) return nullptr;
        return &files[idx];
    }

    int largestFileIndex() const;
};

struct MagnetInfo {
    InfoHash infoHash{};
    bool hasInfoHash = false;
    std::string displayName;
    std::vector<std::string> trackers;
    std::vector<std::string> webSeeds;  // `ws=`
};

/// Also accepts a bare infohash, 40 hex or 32 base32 chars.
bool parseMagnet(const std::string& input, MagnetInfo& out);

bool parseTorrentFile(const std::string& raw, TorrentMetadata& out);

/// For the BEP-9 bytes; checks sha1(raw) against `expected` unless it is zero.
bool parseInfoDict(const std::string& rawInfoDict, const InfoHash& expected, TorrentMetadata& out);

std::string infoHashHex(const InfoHash& h);

}  // namespace torrent
