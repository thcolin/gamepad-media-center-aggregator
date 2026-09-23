/*
    GMCA — Sequential + deadline piece picker (the heart of streaming).

    Unlike a download client (rarest-first, maximise completion), a STREAMING
    picker must feed the decoder from the playhead outward. This picker:

      1. Requests strictly SEQUENTIALLY from the current playhead piece — the
         lowest-index missing piece a peer has is always chosen first, so the
         bytes mpv needs next arrive first.
      2. Confines aggressive prefetch to a READ-AHEAD WINDOW [playhead,
         playhead + readAheadPieces]. Beyond the window we stop requesting, which
         bounds RAM (the sliding buffer) and keeps bandwidth on the head.
      3. Enforces a per-block DEADLINE: a block that has been in-flight too long
         is re-requested (possibly from another peer). Near the window head the
         timeout is short (the decoder is waiting); deeper it is lax.
      4. ENDGAME: when only a few blocks of the head remain outstanding, the same
         block may be requested from several peers to beat a slow one.

    The picker is driven by the single-threaded engine loop, so it needs no lock;
    it reads have/received state from the (thread-safe) PieceStore.
*/

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "torrent/storage.hpp"
#include "torrent/types.hpp"

namespace torrent {

class PiecePicker {
public:
    struct BlockRequest {
        int piece = 0;
        int32_t begin = 0;
        int32_t length = 0;
    };

    PiecePicker(PieceStore& store, const EngineConfig& cfg);

    /// Choose up to `count` next blocks for a peer whose bitfield is `peerHas`
    /// (1 byte per piece). Chosen blocks are marked in-flight against `peerTag`.
    std::vector<BlockRequest> pickForPeer(const std::vector<uint8_t>& peerHas, int count, const void* peerTag);

    /// A block arrived — clear its in-flight record.
    void gotBlock(int piece, int32_t begin);

    /// A block request was rejected/cancelled — clear it so it can be re-picked.
    void abandonBlock(int piece, int32_t begin, const void* peerTag);

    /// Peer disconnected — release all its in-flight requests.
    void releasePeer(const void* peerTag);

    /// Periodic maintenance: expire timed-out in-flight requests (deadline).
    void tick();

    /// Whole next missing piece near the playhead, for the web-seed worker
    /// (-1 if none needed right now). Marks the piece's blocks in-flight under
    /// `webSeedTag` so peers don't double-fetch it.
    int pickPieceForWebSeed(const void* webSeedTag);

    bool complete() const;

private:
    struct InFlight {
        const void* peer = nullptr;
        int64_t sentMs = 0;
    };

    static int64_t key(int piece, int32_t begin) { return ((int64_t)piece << 24) | (begin / PieceStore::kBlockSize); }
    bool blockNeeded(int piece, int blockIdx) const;
    int deadlineMsForPiece(int piece, int playhead) const;

    PieceStore& store_;
    EngineConfig cfg_;
    std::unordered_map<int64_t, InFlight> inflight_;
};

}  // namespace torrent
