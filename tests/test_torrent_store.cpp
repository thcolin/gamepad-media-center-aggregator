// Standalone logic test — the piece store stays within its RAM budget while the
// picker downloads a whole file around a moving playhead. See tests/run.sh.

#include <cstdio>
#include <string>
#include <vector>

#include "../app/src/torrent/bencode.cpp"
#include "../app/src/torrent/log.cpp"
#include "../app/src/torrent/metadata.cpp"
#include "../app/src/torrent/piece_picker.cpp"
#include "../app/src/torrent/sha1.cpp"
#include "../app/src/torrent/storage.cpp"
#include "../app/src/torrent/util.cpp"

using namespace torrent;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    setLogSink([](LogLevel, const std::string&) {});

    const int kPieces = 64;
    const int64_t kPieceLen = 2 * PieceStore::kBlockSize;
    std::vector<std::string> data(kPieces);
    TorrentMetadata meta;
    meta.name = "a.mkv";
    meta.pieceLength = kPieceLen;
    meta.totalLength = kPieces * kPieceLen;
    meta.files.push_back({"a.mkv", meta.totalLength, 0});
    for (int i = 0; i < kPieces; i++) {
        data[i] = std::string((size_t)kPieceLen, (char)('a' + i % 26));
        meta.pieceHashes.push_back(sha1(data[i]));
    }

    const int64_t kBudget = 8 * kPieceLen;
    EngineConfig cfg;
    cfg.readAheadPieces = 24;
    PieceStore store(meta, kBudget);
    PiecePicker picker(store, cfg);
    std::vector<uint8_t> peerHas(kPieces, 1);
    int tag = 0;

    // stream from the start, the playhead following what the reader consumed
    int64_t maxResident = 0;
    for (int round = 0; round < 2000 && !picker.complete(); round++) {
        auto reqs = picker.pickForPeer(peerHas, 4, &tag);
        if (reqs.empty()) {
            int next = store.playheadPiece() + 1;
            if (next >= kPieces) break;
            store.setPlayhead(0, next * kPieceLen);
            continue;
        }
        for (auto& r : reqs) {
            store.addBlock(r.piece, r.begin, reinterpret_cast<const uint8_t*>(data[r.piece].data() + r.begin),
                r.length);
            picker.gotBlock(r.piece, r.begin);
        }
        maxResident = std::max<int64_t>(maxResident, (int64_t)store.piecesHave() * kPieceLen);
    }
    CHECK(maxResident <= kBudget);
    CHECK(store.have(kPieces - 1));

    // a read of a resident piece returns its bytes
    {
        int p = store.playheadPiece();
        std::vector<uint8_t> out((size_t)kPieceLen);
        if (store.have(p)) CHECK(store.readFile(0, p * kPieceLen, out.data(), kPieceLen) == kPieceLen);
    }

    // seeking back far evicts the pieces ahead first and stays within budget
    store.setPlayhead(0, 0);
    for (int round = 0; round < 200; round++) {
        auto reqs = picker.pickForPeer(peerHas, 4, &tag);
        if (reqs.empty()) break;
        for (auto& r : reqs) {
            store.addBlock(r.piece, r.begin, reinterpret_cast<const uint8_t*>(data[r.piece].data() + r.begin),
                r.length);
            picker.gotBlock(r.piece, r.begin);
        }
        CHECK((int64_t)store.piecesHave() * kPieceLen <= kBudget);
    }
    CHECK(store.have(0));

    if (failures == 0) printf("test_torrent_store: all passed\n");
    return failures ? 1 : 0;
}
