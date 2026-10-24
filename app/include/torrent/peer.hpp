#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "torrent/crypto.hpp"
#include "torrent/mse.hpp"
#include "torrent/socket.hpp"
#include "torrent/transport.hpp"
#include "torrent/types.hpp"

namespace torrent {

class PeerConnection;

/// Called on the engine loop thread only.
struct PeerHost {
    virtual ~PeerHost() = default;
    virtual const InfoHash& hostInfoHash() const = 0;
    virtual const std::string& hostPeerId() const = 0;
    virtual bool metadataReady() const = 0;
    virtual int metadataNumPieces() const = 0;      // valid once metadataReady()
    virtual int64_t metadataTotalSize() const = 0;  // 0 until learned from a peer

    virtual void onPeerHandshake(PeerConnection*) = 0;
    virtual void onPeerBlock(PeerConnection*, int piece, int32_t begin, const uint8_t* data, int len) = 0;
    virtual void onMetadataPiece(PeerConnection* from, int piece, int64_t totalSize, const uint8_t* data, int len) = 0;
    virtual void onPexPeers(const std::vector<PeerAddr>&) = 0;
};

/// Leech-only BEP-3 peer over TCP or µTP, with BEP-10 extensions, ut_metadata and ut_pex.
class PeerConnection {
public:
    enum class State { Idle, Connecting, MseHandshake, HandshakeWait, Ready, Closed };

    PeerConnection(const PeerAddr& addr, PeerHost* host, std::unique_ptr<PeerTransport> transport,
        Encryption enc = Encryption::Plaintext);

    bool startConnect();
    void close();

    net::Handle handle() const { return transport_ ? transport_->handle() : net::invalidHandle(); }
    /// False for µTP: those peers share the UtpManager socket and have no fd of their own.
    bool pollable() const { return transport_ && transport_->pollable(); }
    bool wantWrite() const;
    bool alive() const { return state_ != State::Closed; }
    void onReadable();
    void onWritable();
    void onTick(int64_t nowMs);  // keep-alive, connect timeout

    State state() const { return state_; }
    bool ready() const { return state_ == State::Ready; }
    bool peerChoking() const { return peerChoking_; }
    bool amInterested() const { return amInterested_; }
    int outstanding() const { return outstanding_; }
    const std::vector<uint8_t>& peerHas() const { return peerHas_; }
    bool supportsExtensions() const { return extSupported_; }
    int utMetadataId() const { return peerUtMetadataId_; }
    int utPexId() const { return peerUtPexId_; }
    const PeerAddr& addr() const { return addr_; }
    bool usedMse() const { return enc_ != Encryption::Plaintext; }
    bool handshaked() const { return handshakeParsed_; }
    Encryption encryptionMode() const { return enc_; }

    void sendInterested();
    void sendRequest(int piece, int32_t begin, int32_t length);
    void sendMetadataRequest(int piece);
    /// BEP-11 delta since the last call; no-op until the peer advertised ut_pex.
    void sendPex(const std::vector<PeerAddr>& current);
    /// Applies the bitfield and haves received before the metadata.
    void onMetadataReady(int numPieces);

private:
    void queue(const std::string& bytes);
    void flush();
    void buildHandshake();
    void sendExtendedHandshake();
    void parseInbound();
    bool parseHandshake();
    void handleMessage(const uint8_t* data, int len);
    void handleExtended(const uint8_t* data, int len);
    void applyHave(int index);
    void onMseComplete(const std::string& appData);

    PeerAddr addr_;
    PeerHost* host_;
    std::unique_ptr<PeerTransport> transport_;
    State state_ = State::Idle;
    Encryption enc_ = Encryption::Plaintext;

    std::string inbuf_;
    std::string outbuf_;
    size_t outSent_ = 0;

    std::unique_ptr<MseHandshake> mse_;
    std::string btHandshake_;  // also the MSE IA
    Rc4 sendCipher_;
    Rc4 recvCipher_;
    bool sendEncrypted_ = false;
    bool recvEncrypted_ = false;

    bool handshakeParsed_ = false;
    bool extSupported_ = false;
    bool peerChoking_ = true;
    bool amInterested_ = false;
    int outstanding_ = 0;

    std::vector<uint8_t> peerHas_;  // 1 byte per piece, empty until the metadata
    std::string pendingBitfield_;   // received before the metadata
    std::vector<int> pendingHaves_;

    int peerUtMetadataId_ = 0;  // ids the peer assigned: used when sending to it
    int peerUtPexId_ = 0;
    int64_t peerMetadataSize_ = 0;
    std::vector<std::string> pexAdvertised_;

    int64_t connectStartMs_ = 0;
    int64_t lastSendMs_ = 0;

    static constexpr int kUtMetadataLocalId = 1;  // ids we assign: peers use them to send to us
    static constexpr int kUtPexLocalId = 2;
};

}  // namespace torrent
