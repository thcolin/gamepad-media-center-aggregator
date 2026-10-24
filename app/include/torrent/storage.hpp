#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "torrent/metadata.hpp"

namespace torrent {

/// Thread-safe, RAM-only, SHA-1 checked. Over budget, pieces behind the playhead are evicted.
class PieceStore {
public:
    static constexpr int kBlockSize = 16384;  // the block size peers serve

    explicit PieceStore(const TorrentMetadata& meta, int64_t ramBudgetBytes);

    int numPieces() const { return meta_.numPieces(); }
    int64_t pieceLength(int idx) const;  // last piece is shorter
    int blocksInPiece(int idx) const;

    enum class AddResult { Accepted, Duplicate, Complete, BadHash, Invalid };

    /// `begin` is the offset within the piece; the last block verifies the piece.
    AddResult addBlock(int idx, int64_t begin, const uint8_t* data, int len);

    AddResult commitPiece(int idx, const uint8_t* data, int64_t len);

    bool have(int idx) const;
    bool blockReceived(int idx, int blockIndex) const;
    int64_t bytesHave() const;
    int piecesHave() const;

    /// Blocks until the covering pieces are in; returns bytes copied, or -1 after stop().
    int64_t readFile(int fileIdx, int64_t fileOffset, uint8_t* out, int64_t len);

    int64_t contiguousBytesFromHead(int fileIdx) const;

    void setPlayhead(int fileIdx, int64_t fileOffset);
    int playheadPiece() const;

    /// Unblocks every waiting readFile().
    void stop();

    const TorrentMetadata& meta() const { return meta_; }

private:
    struct Slot {
        std::vector<uint8_t> data;
        std::vector<uint8_t> received;  // 1 byte per block
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
    std::vector<uint8_t> haveMap_;
    int piecesHave_ = 0;
    int64_t bytesHave_ = 0;
    int64_t bufferedBytes_ = 0;  // resident, unlike bytesHave_
    int64_t playheadGlobal_ = 0;
    bool stopped_ = false;
};

}  // namespace torrent
