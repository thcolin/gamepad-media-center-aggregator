#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "torrent/dht.hpp"
#include "torrent/http_server.hpp"
#include "torrent/metadata.hpp"
#include "torrent/peer.hpp"
#include "torrent/piece_picker.hpp"
#include "torrent/storage.hpp"
#include "torrent/tracker.hpp"
#include "torrent/transport.hpp"
#include "torrent/types.hpp"
#include "torrent/utp.hpp"

namespace torrent {

enum class TransportKind { Tcp, Utp };

struct PeerAttempt {
    PeerAddr addr;
    TransportKind transport = TransportKind::Tcp;
    Encryption enc = Encryption::Prefer;
};

/// One instance per playback: threads for the peer loop, the announcer, the web seed and the HTTP server.
class TorrentEngine : public PeerHost {
public:
    explicit TorrentEngine(EngineConfig cfg = {});
    ~TorrentEngine() override;

    /// Blocks up to 60 s on the metadata (BEP-9); returns the local URL or "". -1 picks the largest file.
    std::string open(const std::string& magnetOrInfoHash, int fileIdx = -1);

    std::string openTorrentFile(const std::string& rawTorrent, int fileIdx = -1);

    void addPeer(const std::string& host, uint16_t port);
    void addTracker(const std::string& url);
    void addWebSeed(const std::string& url);

    /// Byte offset within the selected file.
    void setPlayhead(int64_t fileByteOffset);

    Stats stats() const;
    std::string localUrl() const;
    bool metadataReady() const override { return metadataReady_.load(); }
    void close();

    bool waitForMetadata(int timeoutMs);
    bool waitForContiguous(int64_t bytes, int timeoutMs);

    const InfoHash& hostInfoHash() const override { return infoHash_; }
    const std::string& hostPeerId() const override { return peerId_; }
    int metadataNumPieces() const override { return metaNumPieces_; }
    int64_t metadataTotalSize() const override { return metadataSize_; }
    void onPeerHandshake(PeerConnection*) override;
    void onPeerBlock(PeerConnection*, int piece, int32_t begin, const uint8_t* data, int len) override;
    void onMetadataPiece(PeerConnection* from, int piece, int64_t totalSize, const uint8_t* data, int len) override;
    void onPexPeers(const std::vector<PeerAddr>&) override;

private:
    void engineLoop();
    void announcerLoop();
    void webSeedLoop();

    void initUtp();
    void initDht();
    std::unique_ptr<PeerTransport> makeTransport(const PeerAddr&, TransportKind);
    std::string attemptKey(const PeerAddr&, TransportKind, Encryption) const;
    void queueFallback(const PeerAddr&, TransportKind wasKind, Encryption wasEnc);

    void enqueuePeers(const std::vector<PeerAddr>&);
    void connectPending();
    void scheduleMetadataRequests();
    void scheduleBlockRequests();
    void schedulePex();
    void onMetadataComplete();
    void resetMetadata();
    void startDataStage(int fileIdx);
    void updateStats();

    EngineConfig cfg_;
    InfoHash infoHash_{};
    std::string peerId_;
    int wantFileIdx_ = -1;

    std::atomic<bool> running_{false};
    std::atomic<bool> metadataReady_{false};
    std::mutex lifecycleMutex_;  // open() thread start and close()
    bool closed_ = false;

    TorrentMetadata meta_;
    int metaNumPieces_ = 0;
    int64_t metadataSize_ = 0;
    std::vector<uint8_t> metaBuf_;
    std::vector<uint8_t> metaReceived_;
    std::vector<int64_t> metaRequestedMs_;
    int metaPiecesGot_ = 0;
    int64_t meta0ReqMs_ = 0;
    PeerConnection* metaPeer_ = nullptr;  // the one peer the info dict is fetched from
    int64_t metaProgressMs_ = 0;
    std::unordered_set<PeerConnection*> metaTried_;

    std::unique_ptr<PieceStore> store_;
    std::unique_ptr<PiecePicker> picker_;
    std::unique_ptr<HttpServer> http_;
    int fileIdx_ = -1;
    std::string fileName_;
    mutable std::mutex pickerMutex_;

    // declared before peers_ so it outlives their µTP transports; null when µTP is off
    std::unique_ptr<UtpManager> utpMgr_;

    std::unique_ptr<DhtManager> dhtMgr_;

    std::vector<std::unique_ptr<PeerConnection>> peers_;
    std::mutex peersMutex_;
    std::vector<PeerAddr> pendingPeers_;
    std::unordered_set<std::string> knownPeers_;
    // engine thread only
    std::unordered_map<int, std::unordered_set<PeerConnection*>> contributors_;
    // engine thread only; attemptTried_ keeps each carrier/encryption rung to one dial per endpoint
    std::vector<PeerAttempt> retryQueue_;
    std::unordered_set<std::string> attemptTried_;
    int64_t lastPexMs_ = 0;
    int pexRounds_ = 0;

    // guards trackers_ and webSeeds_
    std::mutex trackerMutex_;
    std::vector<std::string> trackers_;
    std::vector<std::string> webSeeds_;

    std::thread engineThread_;
    std::thread announcerThread_;
    std::thread webSeedThread_;

    mutable std::mutex statsMutex_;
    Stats stats_;
    int64_t lastRateSampleMs_ = 0;
    int64_t lastRateBytes_ = 0;

    // the web seed's in-flight tag in the picker
    const void* webSeedTag_ = reinterpret_cast<const void*>(0x1);
};

}  // namespace torrent
