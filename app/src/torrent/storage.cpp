/*
    GMCA — In-RAM piece store (see torrent/storage.hpp).
*/

#include "torrent/storage.hpp"

#include <algorithm>
#include <cstring>

#include "torrent/log.hpp"
#include "torrent/sha1.hpp"

namespace torrent {

PieceStore::PieceStore(const TorrentMetadata& meta, int64_t ramBudgetBytes) : meta_(meta), ramBudget_(ramBudgetBytes) {
    haveMap_.assign(meta_.numPieces(), 0);
}

int64_t PieceStore::pieceLength(int idx) const {
    if (idx < 0 || idx >= meta_.numPieces()) return 0;
    if (idx == meta_.numPieces() - 1) {
        int64_t rem = meta_.totalLength - (int64_t)(meta_.numPieces() - 1) * meta_.pieceLength;
        return rem > 0 ? rem : meta_.pieceLength;
    }
    return meta_.pieceLength;
}

int PieceStore::blocksInPiece(int idx) const {
    int64_t plen = pieceLength(idx);
    return (int)((plen + kBlockSize - 1) / kBlockSize);
}

bool PieceStore::have(int idx) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return idx >= 0 && idx < (int)haveMap_.size() && haveMap_[idx];
}

bool PieceStore::blockReceived(int idx, int blockIndex) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (idx >= 0 && idx < (int)haveMap_.size() && haveMap_[idx]) return true;
    auto it = slots_.find(idx);
    if (it == slots_.end()) return false;
    return blockIndex >= 0 && blockIndex < (int)it->second.received.size() && it->second.received[blockIndex];
}

int64_t PieceStore::bytesHave() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return bytesHave_;
}

int PieceStore::piecesHave() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return piecesHave_;
}

PieceStore::AddResult PieceStore::addBlock(int idx, int64_t begin, const uint8_t* data, int len) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (idx < 0 || idx >= meta_.numPieces()) return AddResult::Invalid;
    if (haveMap_[idx]) return AddResult::Duplicate;
    int64_t plen = pieceLength(idx);
    if (begin < 0 || begin + len > plen || (begin % kBlockSize) != 0) return AddResult::Invalid;
    int blockIdx = (int)(begin / kBlockSize);

    Slot& slot = slots_[idx];
    if (slot.data.empty()) {
        slot.data.assign((size_t)plen, 0);
        slot.received.assign(blocksInPiece(idx), 0);
    }
    if (slot.received[blockIdx]) return AddResult::Duplicate;
    std::memcpy(slot.data.data() + begin, data, (size_t)len);
    slot.received[blockIdx] = 1;
    slot.receivedCount++;

    if (slot.receivedCount == (int)slot.received.size()) return verifyAndCommitLocked(idx);
    return AddResult::Accepted;
}

PieceStore::AddResult PieceStore::commitPiece(int idx, const uint8_t* data, int64_t len) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (idx < 0 || idx >= meta_.numPieces()) return AddResult::Invalid;
    if (haveMap_[idx]) return AddResult::Duplicate;
    int64_t plen = pieceLength(idx);
    if (len != plen) return AddResult::Invalid;
    Slot& slot = slots_[idx];
    slot.data.assign(data, data + len);
    slot.received.assign(blocksInPiece(idx), 1);
    slot.receivedCount = (int)slot.received.size();
    return verifyAndCommitLocked(idx);
}

PieceStore::AddResult PieceStore::verifyAndCommitLocked(int idx) {
    Slot& slot = slots_[idx];
    Sha1Digest got = sha1(slot.data.data(), slot.data.size());
    if (got != meta_.pieceHashes[idx]) {
        // Reset the piece so it can be re-fetched (bad peer / corrupt web seed).
        logWarn("storage: piece %d SHA-1 mismatch (got %s), discarding", idx, toHex(got).c_str());
        int64_t was = (int64_t)slot.data.size();
        (void)was;
        slots_.erase(idx);
        return AddResult::BadHash;
    }
    haveMap_[idx] = 1;
    slot.complete = true;
    piecesHave_++;
    int64_t plen = (int64_t)slot.data.size();
    bytesHave_ += plen;
    bufferedBytes_ += plen;
    cv_.notify_all();
    maybeEvictLocked();
    return AddResult::Complete;
}

void PieceStore::maybeEvictLocked() {
    if (bufferedBytes_ <= ramBudget_) return;
    // Evict verified pieces well behind the playhead, lowest index first.
    int playhead = (int)(playheadGlobal_ / meta_.pieceLength);
    const int keepBehind = 16;  // keep a small rewind window
    int guard = playhead - keepBehind;
    for (auto it = slots_.begin(); it != slots_.end() && bufferedBytes_ > ramBudget_;) {
        if (it->second.complete && it->first < guard) {
            bufferedBytes_ -= (int64_t)it->second.data.size();
            // Keep the piece marked Have (we verified it) but drop the bytes.
            // A backward seek past the window would need a re-fetch (rare in
            // streaming); the picker treats evicted pieces as re-requestable via
            // the store's block map being cleared.
            haveMap_[it->first] = 0;  // must re-download if needed again
            piecesHave_--;
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void PieceStore::setPlayhead(int fileIdx, int64_t fileOffset) {
    std::lock_guard<std::mutex> lk(mutex_);
    const FileEntry* fe = meta_.file(fileIdx);
    if (!fe) return;
    playheadGlobal_ = fe->offset + fileOffset;
}

int PieceStore::playheadPiece() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return (int)(playheadGlobal_ / meta_.pieceLength);
}

int64_t PieceStore::contiguousBytesFromHead(int fileIdx) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const FileEntry* fe = meta_.file(fileIdx);
    if (!fe) return 0;
    int64_t start = fe->offset;
    int64_t end = fe->offset + fe->length;
    int firstPiece = (int)(start / meta_.pieceLength);
    int64_t ready = 0;
    for (int p = firstPiece; p < meta_.numPieces(); p++) {
        int64_t ps = (int64_t)p * meta_.pieceLength;
        int64_t pe = ps + pieceLength(p);
        if (ps >= end) break;
        if (!haveMap_[p]) break;
        int64_t from = std::max(ps, start);
        int64_t to = std::min(pe, end);
        ready += (to - from);
    }
    return ready;
}

int64_t PieceStore::readFile(int fileIdx, int64_t fileOffset, uint8_t* out, int64_t len) {
    const FileEntry* fe = meta_.file(fileIdx);
    if (!fe) return -1;
    if (fileOffset >= fe->length) return 0;
    len = std::min(len, fe->length - fileOffset);
    int64_t globalStart = fe->offset + fileOffset;
    int64_t copied = 0;

    std::unique_lock<std::mutex> lk(mutex_);
    while (copied < len && !stopped_) {
        int64_t g = globalStart + copied;
        int piece = (int)(g / meta_.pieceLength);
        int64_t intra = g % meta_.pieceLength;
        if (!haveMap_[piece]) {
            // Block until the covering piece lands. The picker is biased toward
            // the playhead, so this is the streaming stall point.
            cv_.wait(lk);
            continue;
        }
        auto it = slots_.find(piece);
        if (it == slots_.end()) {
            // Evicted after a backward seek: mark Missing and wait for re-fetch.
            haveMap_[piece] = 0;
            cv_.wait(lk);
            continue;
        }
        int64_t avail = (int64_t)it->second.data.size() - intra;
        int64_t chunk = std::min(avail, len - copied);
        std::memcpy(out + copied, it->second.data.data() + intra, (size_t)chunk);
        copied += chunk;
    }
    return stopped_ ? -1 : copied;
}

void PieceStore::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopped_ = true;
    cv_.notify_all();
}

}  // namespace torrent
