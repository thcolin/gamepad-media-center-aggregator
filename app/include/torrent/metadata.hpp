/*
    GMCA — Torrent metadata (the info dictionary) and magnet parsing.

    TorrentMetadata is the parsed `info` dict (BEP-3): piece length, per-piece
    SHA-1 hashes, and the file list. For streaming we pin ONE file (the video,
    chosen by StreamOption.fileIdx) and expose its byte offset within the global
    piece space so the picker/HTTP-server can map file offsets <-> piece indices.

    Two entry points feed this:
      - a magnet / bare infohash (BEP-9 flow): we know only the infohash up front
        and must fetch the info dict from peers via ut_metadata, then parse it;
      - a .torrent file (or the fetched info dict bytes): parse directly.

    We also read `url-list` (BEP-19 web seeds) when present — the CDN HTTP source
    that makes Sintel/BBB reliable to test and is our cold-swarm fallback.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/types.hpp"

namespace torrent {

struct FileEntry {
    std::string path;    // joined with '/'
    int64_t length = 0;  // bytes
    int64_t offset = 0;  // byte offset within the global (concatenated) piece space
};

struct TorrentMetadata {
    InfoHash infoHash{};
    std::string name;
    int64_t pieceLength = 0;
    int64_t totalLength = 0;
    std::vector<Sha1Digest> pieceHashes;                 // one 20-byte SHA-1 per piece
    std::vector<FileEntry> files;                        // single-file torrents => one entry
    std::vector<std::string> webSeeds;                   // BEP-19 url-list entries
    std::vector<std::vector<std::string>> trackerTiers;  // BEP-12 announce-list

    bool valid() const { return pieceLength > 0 && !pieceHashes.empty() && totalLength > 0; }
    int numPieces() const { return (int)pieceHashes.size(); }

    /// Byte range [offset, offset+length) of a file, or {-1,0} if idx invalid.
    const FileEntry* file(int idx) const {
        if (idx < 0 || idx >= (int)files.size()) return nullptr;
        return &files[idx];
    }

    /// Pick the largest file (default video file when fileIdx is unspecified).
    int largestFileIndex() const;
};

/// A magnet's parsed contents. Only `infoHash` is required for the BEP-9 flow;
/// trackers (`tr`) and display name (`dn`) are used when present.
struct MagnetInfo {
    InfoHash infoHash{};
    bool hasInfoHash = false;
    std::string displayName;
    std::vector<std::string> trackers;
    std::vector<std::string> webSeeds;  // magnet `ws=` web-seed hints
};

/// Parse a magnet URI, a bare 40-char hex infohash, or a 32-char base32 infohash.
bool parseMagnet(const std::string& input, MagnetInfo& out);

/// Parse a full .torrent file (bencoded). Computes the infohash from the raw
/// info-dict bytes. Returns false on malformed input.
bool parseTorrentFile(const std::string& raw, TorrentMetadata& out);

/// Parse a bare info dictionary (the bytes delivered by BEP-9 ut_metadata). The
/// infohash is sha1(raw). Verifies against `expected` when non-zero.
bool parseInfoDict(const std::string& rawInfoDict, const InfoHash& expected, TorrentMetadata& out);

/// Hex string (40 chars, lowercase) of an infohash.
std::string infoHashHex(const InfoHash& h);

}  // namespace torrent
