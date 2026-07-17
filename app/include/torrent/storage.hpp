/*
    GMCA — In-RAM, ephemeral piece store (no disk, no persistence).

    The engine holds one instance per playback. Pieces are assembled from 16 KiB
    blocks in RAM, verified against their SHA-1 (BEP-3), and kept in a map keyed
    by piece index. When the RAM budget is exceeded, verified pieces far BEHIND
    the playhead are evicted — the "sliding window" of TORRENT_STREAMING.md §7
    that dodges FAT32/exFAT and the sleep/crash fragilities of on-disk clients.

    Thread model: the engine loop and the web-seed worker WRITE pieces; the local
    HTTP server thread READS, blocking on a condition variable until the covering
    pieces are present. All state is behind one mutex.
*/

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "torrent/metadata.hpp"

namespace torrent {

class PieceStore {
public:
    static constexpr int kBlockSize = 16384;  // 16 KiB — the near-universal request unit

    explicit PieceStore(const TorrentMetadata& meta, int64_t ramBudgetBytes);

    int numPieces() const { return meta_.numPieces(); }
    int64_t pieceLength(int idx) const;  // last piece is shorter
    int blocksInPiece(int idx) const;

    enum class AddResult { Accepted, Duplicate, Complete, BadHash, Invalid };

    /// Add a 16 KiB-ish block into piece `idx` at intra-piece byte `begin`.
    /// Verifies + commits the piece when the final block arrives.
    AddResult addBlock(int idx, int64_t begin, const uint8_t* data, int len);

    /// Commit a whole piece at once (web-seed path). Verifies SHA-1.
    AddResult commitPiece(int idx, const uint8_t* data, int64_t len);

    bool have(int idx) const;
    bool blockReceived(int idx, int blockIndex) const;
    int64_t bytesHave() const;
    int piecesHave() const;

    /// HTTP-server read of the pinned file: copies [fileOffset, +len) into `out`,
    /// blocking until each covering piece is present (or stop()). Returns bytes
    /// copied, or -1 if stopped while waiting.
    int64_t readFile(int fileIdx, int64_t fileOffset, uint8_t* out, int64_t len);

    /// Contiguous verified bytes from the start of the pinned file (buffered %).
    int64_t contiguousBytesFromHead(int fileIdx) const;

    /// Report the playhead (file byte offset) — drives eviction and the picker.
    void setPlayhead(int fileIdx, int64_t fileOffset);
    int playheadPiece() const;

    /// Unblock any waiting reader (teardown).
    void stop();

    const TorrentMetadata& meta() const { return meta_; }

private:
    struct Slot {
        std::vector<uint8_t> data;      // full piece bytes (allocated lazily)
        std::vector<uint8_t> received;  // 1 byte per block: 0/1 (avoids vector<bool> quirks)
        int receivedCount = 0;
        bool complete = false;
    };

    AddResult verifyAndCommitLocked(int idx);
    void maybeEvictLocked();
    int64_t globalToPiece(int64_t globalOffset) const { return globalOffset / meta_.pieceLength; }

    TorrentMetadata meta_;
    int64_t ramBudget_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<int, Slot> slots_;
    std::vector<uint8_t> haveMap_;  // 0/1 per piece
    int piecesHave_ = 0;
    int64_t bytesHave_ = 0;
    int64_t bufferedBytes_ = 0;  // bytes currently resident (for eviction)
    int64_t playheadGlobal_ = 0;
    bool stopped_ = false;
};

}  // namespace torrent
