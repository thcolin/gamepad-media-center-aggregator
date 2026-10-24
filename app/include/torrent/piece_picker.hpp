#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "torrent/storage.hpp"
#include "torrent/types.hpp"

namespace torrent {

/// Lowest missing piece from the playhead first, read-ahead window first. Not thread-safe.
class PiecePicker {
public:
    struct BlockRequest {
        int piece = 0;
        int32_t begin = 0;
        int32_t length = 0;
    };

    PiecePicker(PieceStore& store, const EngineConfig& cfg);

    /// `peerHas` holds 1 byte per piece; chosen blocks are marked in flight for `peerTag`.
    std::vector<BlockRequest> pickForPeer(const std::vector<uint8_t>& peerHas, int count, const void* peerTag);

    void gotBlock(int piece, int32_t begin);

    void abandonBlock(int piece, int32_t begin, const void* peerTag);

    void releasePeer(const void* peerTag);

    /// Expires in-flight blocks past their deadline.
    void tick();

    /// Claims a whole piece for the web seed, or returns -1.
    int pickPieceForWebSeed(const void* webSeedTag);

    bool complete() const;

private:
    struct InFlight {
        const void* peer = nullptr;
        int64_t sentMs = 0;
    };

    static int64_t key(int piece, int32_t begin) { return ((int64_t)piece << 24) | (begin / PieceStore::kBlockSize); }
    bool blockNeeded(int piece, int blockIdx) const;
    int windowEndFor(int playhead) const;
    int deadlineMsForPiece(int piece, int playhead) const;

    PieceStore& store_;
    EngineConfig cfg_;
    std::unordered_map<int64_t, InFlight> inflight_;
};

}  // namespace torrent
