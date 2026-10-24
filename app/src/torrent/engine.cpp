#include "torrent/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "torrent/http_client.hpp"
#include "torrent/log.hpp"
#include "torrent/util.hpp"

namespace torrent {

namespace {
constexpr int kMetadataPieceSize = 16384;  // BEP-9 metadata pieces are 16 KiB
// total_size comes from a peer; real info dicts stay well under this.
constexpr int64_t kMaxMetadataSize = 4 * 1024 * 1024;
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

TorrentEngine::TorrentEngine(EngineConfig cfg) : cfg_(std::move(cfg)) {
    net::globalInit();
    peerId_ = makePeerId(cfg_.peerIdPrefix);
    lastRateSampleMs_ = nowMs();
}

TorrentEngine::~TorrentEngine() { close(); }

// ---- entry points -----------------------------------------------------------

std::string TorrentEngine::open(const std::string& magnetOrInfoHash, int fileIdx) {
    MagnetInfo mi;
    if (!parseMagnet(magnetOrInfoHash, mi) || !mi.hasInfoHash) {
        logError("engine: could not parse magnet/infohash: %s", magnetOrInfoHash.c_str());
        return "";
    }
    infoHash_ = mi.infoHash;
    wantFileIdx_ = fileIdx;
    logInfo("engine: opening infohash %s (%s)", infoHashHex(infoHash_).c_str(),
        mi.displayName.empty() ? "no name" : mi.displayName.c_str());
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        for (auto& t : mi.trackers) trackers_.push_back(t);
        for (auto& w : mi.webSeeds) webSeeds_.push_back(w);
    }
    meta0ReqMs_ = 0;
    running_ = true;
    initUtp();
    initDht();
    engineThread_ = std::thread([this] { engineLoop(); });
    announcerThread_ = std::thread([this] { announcerLoop(); });

    // the local URL needs the file length, which only the metadata gives
    if (!waitForMetadata(60000)) {
        logWarn("engine: metadata not acquired within timeout");
        return "";
    }
    return localUrl();
}

std::string TorrentEngine::openTorrentFile(const std::string& rawTorrent, int fileIdx) {
    if (!parseTorrentFile(rawTorrent, meta_)) {
        logError("engine: could not parse .torrent file");
        return "";
    }
    infoHash_ = meta_.infoHash;
    wantFileIdx_ = fileIdx;
    logInfo("engine: opened .torrent %s (%s, %lld bytes, %d pieces)", infoHashHex(infoHash_).c_str(),
        meta_.name.c_str(), (long long)meta_.totalLength, meta_.numPieces());
    running_ = true;
    initUtp();
    initDht();
    startDataStage(wantFileIdx_);
    engineThread_ = std::thread([this] { engineLoop(); });
    announcerThread_ = std::thread([this] { announcerLoop(); });
    return localUrl();
}

void TorrentEngine::addPeer(const std::string& host, uint16_t port) { enqueuePeers({{host, port}}); }

void TorrentEngine::addTracker(const std::string& url) {
    std::lock_guard<std::mutex> lk(trackerMutex_);
    trackers_.push_back(url);
}

void TorrentEngine::addWebSeed(const std::string& url) {
    std::lock_guard<std::mutex> lk(trackerMutex_);
    webSeeds_.push_back(url);
}

void TorrentEngine::setPlayhead(int64_t fileByteOffset) {
    if (store_) store_->setPlayhead(fileIdx_, fileByteOffset);
}

std::string TorrentEngine::localUrl() const { return http_ ? http_->url() : std::string(); }

// ---- data stage bring-up ----------------------------------------------------

void TorrentEngine::startDataStage(int fileIdx) {
    if (!meta_.valid()) {
        logError("engine: startDataStage with invalid metadata");
        return;
    }
    fileIdx_ = (fileIdx >= 0 && fileIdx < (int)meta_.files.size()) ? fileIdx : meta_.largestFileIndex();
    metaNumPieces_ = meta_.numPieces();
    const FileEntry* fe = meta_.file(fileIdx_);
    fileName_ = fe ? fe->path : meta_.name;
    size_t slash = fileName_.find_last_of('/');
    if (slash != std::string::npos) fileName_ = fileName_.substr(slash + 1);

    store_ = std::make_unique<PieceStore>(meta_, cfg_.ramBudgetBytes);
    {
        std::lock_guard<std::mutex> lk(pickerMutex_);
        picker_ = std::make_unique<PiecePicker>(*store_, cfg_);
    }
    store_->setPlayhead(fileIdx_, 0);

    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        for (auto& tier : meta_.trackerTiers)
            for (auto& t : tier) trackers_.push_back(t);
        for (auto& w : meta_.webSeeds) webSeeds_.push_back(w);
    }

    http_ = std::make_unique<HttpServer>(*store_, fileIdx_, fileName_);
    if (!http_->start(cfg_.httpPort)) {
        logError("engine: local HTTP server failed to bind");
    } else {
        logInfo("engine: local HTTP server at %s", http_->url().c_str());
    }

    // peers connected during the metadata phase get their have-bitmap sized now
    for (auto& p : peers_)
        if (p && p->alive()) p->onMetadataReady(metaNumPieces_);

    bool hasWebSeed;
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        hasWebSeed = !webSeeds_.empty();
    }
    if (cfg_.enableWebSeed && hasWebSeed && meta_.files.size() == 1)
        webSeedThread_ = std::thread([this] { webSeedLoop(); });

    metadataReady_ = true;
    logInfo("engine: data stage ready — file '%s' (%lld bytes, %d pieces, pieceLen %lld)", fileName_.c_str(),
        (long long)(fe ? fe->length : 0), meta_.numPieces(), (long long)meta_.pieceLength);
}

// ---- peer discovery ---------------------------------------------------------

void TorrentEngine::enqueuePeers(const std::vector<PeerAddr>& peers) {
    std::lock_guard<std::mutex> lk(peersMutex_);
    for (const auto& a : peers) {
        std::string key = a.str();
        if (knownPeers_.count(key)) continue;
        knownPeers_.insert(key);
        pendingPeers_.push_back(a);
    }
}

void TorrentEngine::initUtp() {
    if (!cfg_.enableUtp) return;
    utpMgr_ = std::make_unique<UtpManager>();
    if (!utpMgr_->start()) {
        utpMgr_.reset();  // stays TCP-only
    }
}

void TorrentEngine::initDht() {
    if (!cfg_.enableDht) return;
    dhtMgr_ = std::make_unique<DhtManager>();
    bool ok = dhtMgr_->start([this](const std::vector<PeerAddr>& peers) { enqueuePeers(peers); });
    if (!ok) {
        dhtMgr_.reset();
        return;
    }
    // runs before the engine thread starts; every later dht_* call is on that thread
    dhtMgr_->search(infoHash_);
}

std::unique_ptr<PeerTransport> TorrentEngine::makeTransport(const PeerAddr& addr, TransportKind kind) {
    if (kind == TransportKind::Utp) {
        if (!utpMgr_ || !utpMgr_->active()) return nullptr;
        return utpMgr_->createTransport(addr);
    }
    return std::make_unique<TcpTransport>();
}

std::string TorrentEngine::attemptKey(const PeerAddr& addr, TransportKind kind, Encryption enc) const {
    char t = kind == TransportKind::Utp ? 'u' : 't';
    char e = enc == Encryption::Forced ? 'f' : enc == Encryption::Prefer ? 'r' : 'p';
    return addr.str() + "|" + t + "|" + e;
}

void TorrentEngine::queueFallback(const PeerAddr& addr, TransportKind wasKind, Encryption wasEnc) {
    auto utpUsable = [&] { return cfg_.enableUtp && utpMgr_ && utpMgr_->active(); };
    auto tryEnqueue = [&](TransportKind kind, Encryption enc) {
        if (kind == TransportKind::Tcp && !cfg_.enableTcp) return;
        if (kind == TransportKind::Utp && !utpUsable()) return;
        std::string key = attemptKey(addr, kind, enc);
        if (attemptTried_.count(key)) return;
        attemptTried_.insert(key);
        retryQueue_.push_back({addr, kind, enc});
        logDebug("engine: fallback %s -> %s/%s", addr.str().c_str(), kind == TransportKind::Utp ? "uTP" : "TCP",
            enc == Encryption::Forced   ? "force"
            : enc == Encryption::Prefer ? "prefer"
                                        : "plain");
    };

    // same carrier, MSE -> plaintext: a peer that never handshaked may not speak MSE
    if (wasEnc == Encryption::Prefer) {
        tryEnqueue(wasKind, Encryption::Plaintext);
        return;
    }
    // then TCP -> µTP, back at the configured encryption policy
    if (wasKind == TransportKind::Tcp && utpUsable()) {
        tryEnqueue(TransportKind::Utp, cfg_.encryption);
        return;
    }
}

void TorrentEngine::connectPending() {
    // a peer that died before its BitTorrent handshake is retried on the next rung
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!(*it)->alive()) {
            if (!(*it)->handshaked()) {
                TransportKind wasKind = (*it)->pollable() ? TransportKind::Tcp : TransportKind::Utp;
                queueFallback((*it)->addr(), wasKind, (*it)->encryptionMode());
            }
            {
                std::lock_guard<std::mutex> lk(pickerMutex_);
                if (picker_) picker_->releasePeer(it->get());
            }
            for (auto& c : contributors_) c.second.erase(it->get());
            hashStrikes_.erase(it->get());
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    while ((int)peers_.size() < cfg_.maxPeers) {
        PeerAttempt at;
        if (!retryQueue_.empty()) {
            at = retryQueue_.back();
            retryQueue_.pop_back();
        } else {
            std::lock_guard<std::mutex> lk(peersMutex_);
            if (pendingPeers_.empty()) break;
            at.addr = pendingPeers_.back();
            pendingPeers_.pop_back();
            at.transport = cfg_.enableTcp ? TransportKind::Tcp : TransportKind::Utp;
            at.enc = cfg_.encryption;
            attemptTried_.insert(attemptKey(at.addr, at.transport, at.enc));
        }
        auto transport = makeTransport(at.addr, at.transport);
        if (!transport) continue;
        auto peer = std::make_unique<PeerConnection>(at.addr, this, std::move(transport), at.enc);
        if (peer->startConnect()) {
            peers_.push_back(std::move(peer));
        }
    }
}

// ---- metadata (BEP-9) -------------------------------------------------------

void TorrentEngine::scheduleMetadataRequests() {
    std::vector<PeerConnection*> capable;
    for (auto& p : peers_)
        if (p->ready() && p->utMetadataId() > 0) capable.push_back(p.get());
    if (capable.empty()) return;
    int64_t now = nowMs();

    if (metaNumPieces_ == 0 && metadataSize_ == 0) {
        // the reply to piece 0 carries total_size
        if (now - meta0ReqMs_ > 3000) {
            capable[0]->sendMetadataRequest(0);
            meta0ReqMs_ = now;
        }
        return;
    }
    int rr = 0;
    for (int i = 0; i < (int)metaReceived_.size(); i++) {
        if (metaReceived_[i]) continue;
        if (now - metaRequestedMs_[i] < 3000) continue;
        capable[rr % capable.size()]->sendMetadataRequest(i);
        metaRequestedMs_[i] = now;
        rr++;
    }
}

void TorrentEngine::onMetadataPiece(int piece, int64_t totalSize, const uint8_t* data, int len) {
    if (metadataReady_) return;
    if (metadataSize_ == 0 && totalSize > 0 && totalSize <= kMaxMetadataSize) {
        metadataSize_ = totalSize;
        int count = (int)((totalSize + kMetadataPieceSize - 1) / kMetadataPieceSize);
        metaBuf_.assign((size_t)totalSize, 0);
        metaReceived_.assign(count, 0);
        metaRequestedMs_.assign(count, 0);
        logInfo("engine: metadata size %lld bytes (%d pieces)", (long long)totalSize, count);
    }
    if (metadataSize_ == 0) return;
    int count = (int)metaReceived_.size();
    if (piece < 0 || piece >= count || metaReceived_[piece]) return;
    int64_t off = (int64_t)piece * kMetadataPieceSize;
    int64_t space = (int64_t)metaBuf_.size() - off;
    int copy = (int)std::min<int64_t>(std::min(len, kMetadataPieceSize), space);
    if (copy <= 0) return;
    std::memcpy(metaBuf_.data() + off, data, (size_t)copy);
    metaReceived_[piece] = 1;
    metaPiecesGot_++;
    if (metaPiecesGot_ == count) onMetadataComplete();
}

void TorrentEngine::onMetadataComplete() {
    std::string raw(reinterpret_cast<char*>(metaBuf_.data()), metaBuf_.size());
    if (!parseInfoDict(raw, infoHash_, meta_)) {
        logError("engine: metadata failed verification/parse — resetting to re-fetch");
        // The size came from one peer too: forget it, or a lying peer stalls the torrent for good.
        metadataSize_ = 0;
        metaPiecesGot_ = 0;
        metaBuf_.clear();
        metaReceived_.clear();
        metaRequestedMs_.clear();
        return;
    }
    logInfo("engine: metadata verified — '%s' %lld bytes, %d files", meta_.name.c_str(), (long long)meta_.totalLength,
        (int)meta_.files.size());
    startDataStage(wantFileIdx_);
}

// ---- data requests ----------------------------------------------------------

void TorrentEngine::scheduleBlockRequests() {
    std::lock_guard<std::mutex> lk(pickerMutex_);
    if (!picker_) return;
    for (auto& p : peers_) {
        if (!p->ready() || p->peerChoking() || p->peerHas().empty()) continue;
        int need = cfg_.blockPipelineDepth - p->outstanding();
        if (need <= 0) continue;
        auto reqs = picker_->pickForPeer(p->peerHas(), need, p.get());
        for (const auto& r : reqs) p->sendRequest(r.piece, r.begin, r.length);
    }
}

void TorrentEngine::onPeerHandshake(PeerConnection* peer) {
    logInfo("engine: peer %s handshaked over %s", peer->addr().str().c_str(), peer->pollable() ? "TCP" : "uTP");
    if (metadataReady_) peer->onMetadataReady(metaNumPieces_);
}

void TorrentEngine::onPeerBlock(PeerConnection* peer, int piece, int32_t begin, const uint8_t* data, int len) {
    if (!store_) return;
    auto r = store_->addBlock(piece, begin, data, len);
    if (r == PieceStore::AddResult::Accepted || r == PieceStore::AddResult::Complete ||
        r == PieceStore::AddResult::BadHash)
        contributors_[piece].insert(peer);
    if (r == PieceStore::AddResult::BadHash) {
        // A peer alone behind a bad piece is lying; with several, the second strike decides.
        // A closed peer is never dialed again (knownPeers_).
        auto& who = contributors_[piece];
        for (PeerConnection* p : who)
            if (who.size() == 1 || ++hashStrikes_[p] >= 2) {
                logWarn("engine: peer %s sent a bad piece %d, dropped", p->addr().str().c_str(), piece);
                p->close();
            }
        contributors_.erase(piece);
    } else if (r == PieceStore::AddResult::Complete) {
        contributors_.erase(piece);
    }
    std::lock_guard<std::mutex> lk(pickerMutex_);
    if (picker_) picker_->gotBlock(piece, begin);
}

void TorrentEngine::onPexPeers(const std::vector<PeerAddr>& peers) {
    if (cfg_.enablePex) enqueuePeers(peers);
}

// ---- PEX (BEP-11) -----------------------------------------------------------

void TorrentEngine::schedulePex() {
    if (!cfg_.enablePex) return;
    int64_t now = nowMs();
    if (lastPexMs_ == 0) lastPexMs_ = now;
    // a first round early, for peers that only send PEX once they get some, then
    // the BEP-11 minimum of 60 s
    int64_t interval = pexRounds_ == 0 ? 5000 : 60000;
    if (now - lastPexMs_ < interval) return;

    std::vector<PeerAddr> connected;
    for (auto& p : peers_)
        if (p->ready()) connected.push_back(p->addr());
    if (connected.empty()) return;

    for (auto& p : peers_) {
        if (!p->ready() || p->utPexId() == 0) continue;
        std::vector<PeerAddr> forPeer;
        forPeer.reserve(connected.size());
        for (auto& e : connected)
            if (!(e == p->addr())) forPeer.push_back(e);
        p->sendPex(forPeer);
    }
    lastPexMs_ = now;
    pexRounds_++;
}

// ---- engine loop ------------------------------------------------------------

void TorrentEngine::engineLoop() {
    int64_t lastTick = nowMs();
    while (running_) {
        connectPending();

        // µTP peers have no fd of their own: they share the µTP UDP socket
        std::vector<net::PollItem> items;
        std::vector<PeerConnection*> ptrs;
        items.reserve(peers_.size() + 1);
        ptrs.reserve(peers_.size());
        for (auto& p : peers_) {
            if (!p->alive() || !p->pollable()) continue;
            net::PollItem it;
            it.fd = p->handle();
            it.wantRead = true;
            it.wantWrite = p->wantWrite();
            items.push_back(it);
            ptrs.push_back(p.get());
        }
        bool utpActive = utpMgr_ && utpMgr_->active();
        int udpIdx = -1;
        if (utpActive) {
            udpIdx = (int)items.size();
            net::PollItem it;
            it.fd = utpMgr_->udpHandle();
            it.wantRead = true;
            items.push_back(it);
        }
        // the DHT has its own UDP socket, never multiplexed with µTP
        bool dhtActive = dhtMgr_ && dhtMgr_->active();
        int dhtIdx = -1;
        if (dhtActive) {
            dhtIdx = (int)items.size();
            net::PollItem it;
            it.fd = dhtMgr_->udpHandle();
            it.wantRead = true;
            items.push_back(it);
        }

        net::poll(items, 200);
        for (size_t i = 0; i < ptrs.size(); i++) {
            PeerConnection* p = ptrs[i];
            if (!p->alive()) continue;
            if (items[i].error) {
                p->close();
                continue;
            }
            if (items[i].writable) p->onWritable();
            if (p->alive() && items[i].readable) p->onReadable();
        }

        // libutp reports state changes through callbacks, not poll readiness, so
        // every µTP peer is pumped on each iteration
        if (utpActive) {
            if (udpIdx >= 0 && items[udpIdx].readable) utpMgr_->serviceReadable();
            for (auto& p : peers_) {
                if (!p->alive() || p->pollable()) continue;
                p->onWritable();
                if (p->alive()) p->onReadable();
            }
            utpMgr_->checkTimeouts(nowMs());
        }

        // jech/dht is not thread-safe: every dht_* call stays on this thread
        if (dhtActive) {
            if (dhtIdx >= 0 && items[dhtIdx].readable) dhtMgr_->serviceReadable();
            dhtMgr_->periodic(nowMs());
        }

        int64_t now = nowMs();
        for (auto& p : peers_) p->onTick(now);

        if (!metadataReady_) {
            scheduleMetadataRequests();
        } else {
            scheduleBlockRequests();
            if (now - lastTick > 1000) {
                std::lock_guard<std::mutex> lk(pickerMutex_);
                if (picker_) picker_->tick();
                lastTick = now;
            }
        }
        schedulePex();
        updateStats();
    }
}

// ---- announcer --------------------------------------------------------------

void TorrentEngine::announcerLoop() {
    bool firstRound = true;
    while (running_) {
        // bootstrap DNS resolves here so a slow lookup never stalls the peer loop;
        // the engine thread pings the results in DhtManager::periodic()
        if (dhtMgr_ && dhtMgr_->active()) dhtMgr_->resolveBootstrap();

        std::vector<std::string> trackers;
        {
            std::lock_guard<std::mutex> lk(trackerMutex_);
            trackers = trackers_;
        }
        AnnounceContext ctx;
        ctx.infoHash = infoHash_;
        ctx.peerId = peerId_;
        // Nothing listens: the engine only dials out. Trackers still want a port.
        ctx.port = 6881;
        // store_ and meta_ are written by the engine thread until metadataReady_ is set.
        bool ready = metadataReady_.load();
        ctx.downloaded = ready ? store_->bytesHave() : 0;
        ctx.left = ready ? std::max<int64_t>(0, meta_.totalLength - ctx.downloaded) : 16384;
        ctx.event = firstRound ? 2 : 0;
        ctx.numWant = cfg_.maxPeers * 2;

        int minInterval = 1800;
        for (const auto& t : trackers) {
            if (!running_) break;
            AnnounceResult r = announce(t, ctx, 12000);
            if (r.ok) {
                logInfo("announce %s -> %d peers (interval %ds)", t.c_str(), (int)r.peers.size(), r.intervalSec);
                enqueuePeers(r.peers);
                minInterval = std::min(minInterval, std::max(60, r.intervalSec));
            } else {
                logWarn("announce %s failed: %s", t.c_str(), r.error.c_str());
            }
        }
        firstRound = false;

        // re-announce at most every 2 min, wake early on close
        int waitS = std::min(minInterval, 120);
        for (int i = 0; i < waitS * 5 && running_; i++) sleepMs(200);
    }
}

// ---- web seed (BEP-19) ------------------------------------------------------

void TorrentEngine::webSeedLoop() {
    std::vector<std::string> seeds;
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        seeds = webSeeds_;
    }
    // single-file only: a multi-file web seed needs a URL per file
    while (running_ && store_ && !seeds.empty()) {
        int piece = -1;
        {
            std::lock_guard<std::mutex> lk(pickerMutex_);
            if (picker_) piece = picker_->pickPieceForWebSeed(webSeedTag_);
        }
        if (piece < 0) {
            sleepMs(200);
            continue;
        }
        int64_t begin = (int64_t)piece * meta_.pieceLength;
        int64_t plen = store_->pieceLength(piece);
        int64_t last = begin + plen - 1;

        bool got = false;
        for (auto it = seeds.begin(); it != seeds.end() && running_;) {
            std::string url = *it;
            if (!url.empty() && url.back() == '/') url += fileName_;  // BEP-19 dir form
            http::Response resp = http::getRange(url, begin, last, 30000);
            if (resp.tooLarge) {
                logWarn("webseed %s ignores Range, dropped", url.c_str());
                it = seeds.erase(it);
                continue;
            }
            ++it;
            if (!resp.ok()) {
                if (resp.status != 0)
                    logWarn("webseed %s piece %d: HTTP %ld", url.c_str(), piece, resp.status);
                else
                    logWarn("webseed %s piece %d: %s", url.c_str(), piece, resp.error.c_str());
                continue;
            }
            const std::string& body = resp.body;
            if ((int64_t)body.size() == plen) {
                if (store_->commitPiece(piece, reinterpret_cast<const uint8_t*>(body.data()), plen) ==
                    PieceStore::AddResult::Complete)
                    got = true;
                if (got) break;
            } else {
                logWarn("webseed %s piece %d: unexpected body %zu bytes (want %lld)", url.c_str(), piece, body.size(),
                    (long long)plen);
            }
        }
        // release the claim either way, on success the store has the piece
        std::lock_guard<std::mutex> lk(pickerMutex_);
        if (picker_) picker_->releasePeer(webSeedTag_);
        if (!got) sleepMs(300);
    }
}

// ---- stats / waits ----------------------------------------------------------

void TorrentEngine::updateStats() {
    int64_t now = nowMs();
    Stats s;
    for (auto& p : peers_)
        if (p->ready()) s.peersConnected++;
    {
        std::lock_guard<std::mutex> lk(peersMutex_);
        s.peersKnown = (int)knownPeers_.size();
    }
    s.metadataReady = metadataReady_;
    if (store_) {
        s.downloadedBytes = store_->bytesHave();
        s.piecesTotal = store_->numPieces();
        s.piecesHave = store_->piecesHave();
        s.contiguousReadyBytes = store_->contiguousBytesFromHead(fileIdx_);
    }
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        s.webSeeds = (int)webSeeds_.size();
    }
    // a dht_* call, safe because updateStats runs on the engine thread
    if (dhtMgr_ && dhtMgr_->active()) s.dhtNodes = dhtMgr_->nodeCount();
    int64_t dt = now - lastRateSampleMs_;
    if (dt >= 1000) {
        s.downloadRateBps = (double)(s.downloadedBytes - lastRateBytes_) * 1000.0 / (double)dt;
        lastRateSampleMs_ = now;
        lastRateBytes_ = s.downloadedBytes;
    } else {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s.downloadRateBps = stats_.downloadRateBps;
    }
    std::lock_guard<std::mutex> lk(statsMutex_);
    stats_ = s;
}

Stats TorrentEngine::stats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

bool TorrentEngine::waitForMetadata(int timeoutMs) {
    int64_t deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (metadataReady_) return true;
        if (!running_) return false;
        sleepMs(50);
    }
    return metadataReady_;
}

bool TorrentEngine::waitForContiguous(int64_t bytes, int timeoutMs) {
    int64_t deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (store_ && store_->contiguousBytesFromHead(fileIdx_) >= bytes) return true;
        if (!running_) return false;
        sleepMs(100);
    }
    return store_ && store_->contiguousBytesFromHead(fileIdx_) >= bytes;
}

// ---- teardown ---------------------------------------------------------------

void TorrentEngine::close() {
    if (!running_.exchange(false)) {
        // not started or already closed: threads may still need joining
    }
    if (store_) store_->stop();  // unblocks the HTTP server's readFile
    if (engineThread_.joinable()) engineThread_.join();
    if (announcerThread_.joinable()) announcerThread_.join();
    if (webSeedThread_.joinable()) webSeedThread_.join();
    if (http_) {
        http_->stop();
        http_.reset();
    }
    // peers close their µTP transports into utpMgr_'s context, so they go first;
    // the engine thread has joined, so libutp is single-threaded here
    peers_.clear();
    if (utpMgr_) {
        utpMgr_->stop();
        utpMgr_.reset();
    }
    // the engine thread has joined, so dht_uninit runs with no query in flight
    if (dhtMgr_) {
        dhtMgr_->stop();
        dhtMgr_.reset();
    }
    {
        std::lock_guard<std::mutex> lk(pickerMutex_);
        picker_.reset();
    }
    store_.reset();
}

}  // namespace torrent
