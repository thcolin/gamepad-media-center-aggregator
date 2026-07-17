/*
    GMCA — Sequential + deadline piece picker (see torrent/piece_picker.hpp).
*/

#include "torrent/piece_picker.hpp"

#include <algorithm>

#include "torrent/util.hpp"

namespace torrent {

PiecePicker::PiecePicker(PieceStore& store, const EngineConfig& cfg) : store_(store), cfg_(cfg) {}

bool PiecePicker::blockNeeded(int piece, int blockIdx) const {
    if (store_.have(piece)) return false;
    if (store_.blockReceived(piece, blockIdx)) return false;
    auto it = inflight_.find(key(piece, blockIdx * PieceStore::kBlockSize));
    return it == inflight_.end();
}

int PiecePicker::deadlineMsForPiece(int piece, int playhead) const {
    // Tighter deadline the closer a piece is to the playhead (the decoder waits).
    int dist = piece - playhead;
    if (dist <= 2) return 3000;
    if (dist <= 8) return 6000;
    return 12000;
}

std::vector<PiecePicker::BlockRequest> PiecePicker::pickForPeer(
    const std::vector<uint8_t>& peerHas, int count, const void* peerTag) {
    std::vector<BlockRequest> out;
    if (count <= 0) return out;
    int total = store_.numPieces();
    int playhead = store_.playheadPiece();
    int windowEnd = std::min(total - 1, playhead + cfg_.readAheadPieces);

    // How many missing head-window blocks are still outstanding: drives endgame.
    // (Cheap heuristic: endgame once we are near the tail of the window.)
    auto tryPiece = [&](int p, bool endgame) {
        if ((int)out.size() >= count) return;
        if (p < 0 || p >= total) return;
        if (p < (int)peerHas.size() && !peerHas[p]) return;  // peer lacks the piece
        if (store_.have(p)) return;
        int blocks = store_.blocksInPiece(p);
        for (int b = 0; b < blocks && (int)out.size() < count; b++) {
            if (store_.blockReceived(p, b)) continue;
            int64_t k = key(p, b * PieceStore::kBlockSize);
            bool busy = inflight_.count(k) != 0;
            if (busy && !endgame) continue;  // someone already fetching it
            if (busy && endgame) {
                // don't double-assign to the SAME peer
                auto& f = inflight_[k];
                if (f.peer == peerTag) continue;
            }
            int32_t begin = b * PieceStore::kBlockSize;
            int32_t plen = (int32_t)store_.pieceLength(p);
            int32_t length = std::min<int32_t>(PieceStore::kBlockSize, plen - begin);
            out.push_back({p, begin, length});
            inflight_[k] = InFlight{peerTag, nowMs()};
        }
    };

    // Phase 1: strictly sequential inside the read-ahead window.
    for (int p = playhead; p <= windowEnd && (int)out.size() < count; p++) tryPiece(p, false);

    // Phase 2 (endgame): still short and window not satisfiable without dupes —
    // allow duplicate requests inside the window to beat a slow peer.
    if ((int)out.size() < count)
        for (int p = playhead; p <= windowEnd && (int)out.size() < count; p++) tryPiece(p, true);

    // Phase 3: keep a lone peer busy on the rest of the file (below-window catch-up
    // after seeks, and tail pieces) — still lowest-index first for sequentiality.
    if (out.empty()) {
        for (int p = windowEnd + 1; p < total && (int)out.size() < count; p++) tryPiece(p, false);
        for (int p = 0; p < playhead && (int)out.size() < count; p++) tryPiece(p, false);
    }
    return out;
}

void PiecePicker::gotBlock(int piece, int32_t begin) { inflight_.erase(key(piece, begin)); }

void PiecePicker::abandonBlock(int piece, int32_t begin, const void* peerTag) {
    auto it = inflight_.find(key(piece, begin));
    if (it != inflight_.end() && it->second.peer == peerTag) inflight_.erase(it);
}

void PiecePicker::releasePeer(const void* peerTag) {
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        if (it->second.peer == peerTag)
            it = inflight_.erase(it);
        else
            ++it;
    }
}

void PiecePicker::tick() {
    int playhead = store_.playheadPiece();
    int64_t now = nowMs();
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        int piece = (int)(it->first >> 24);
        if (now - it->second.sentMs > deadlineMsForPiece(piece, playhead))
            it = inflight_.erase(it);  // expired -> re-pickable next round
        else
            ++it;
    }
}

int PiecePicker::pickPieceForWebSeed(const void* webSeedTag) {
    int total = store_.numPieces();
    int playhead = store_.playheadPiece();
    int windowEnd = std::min(total - 1, playhead + cfg_.readAheadPieces);
    auto claim = [&](int p) -> bool {
        if (store_.have(p)) return false;
        int blocks = store_.blocksInPiece(p);
        // Web seed fetches the whole piece; only take it if NO block is in-flight
        // (avoid racing a peer that is already delivering it).
        for (int b = 0; b < blocks; b++)
            if (inflight_.count(key(p, b * PieceStore::kBlockSize))) return false;
        for (int b = 0; b < blocks; b++) inflight_[key(p, b * PieceStore::kBlockSize)] = InFlight{webSeedTag, nowMs()};
        return true;
    };
    for (int p = playhead; p <= windowEnd; p++)
        if (claim(p)) return p;
    for (int p = windowEnd + 1; p < total; p++)
        if (claim(p)) return p;
    for (int p = 0; p < playhead; p++)
        if (claim(p)) return p;
    return -1;
}

bool PiecePicker::complete() const { return store_.piecesHave() == store_.numPieces(); }

}  // namespace torrent
