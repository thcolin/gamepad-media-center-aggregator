/*
    GMCA — BitTorrent peer connection (TCP wire protocol).

    Implements, over the non-blocking TCP shim:
      - the BEP-3 handshake and message stream (choke/unchoke/interested,
        bitfield/have, request/piece/cancel);
      - the BEP-10 extension protocol handshake (advertises ut_metadata/ut_pex);
      - BEP-9 ut_metadata (fetch the info dict from an infohash-only magnet);
      - BEP-11 ut_pex (learn more peers from connected ones).

    µTP (BEP-29) and outgoing DHT (BEP-5) are intentionally OUT of PoC scope. We
    are leech-only: incoming piece requests from peers are ignored (we never
    upload data), which is the ephemeral, low-footprint posture the project wants.

    All I/O is non-blocking and driven by the single-threaded engine loop via
    onReadable()/onWritable(); the peer never blocks and holds no lock.
*/

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

/// Callbacks the engine implements. Invoked from the engine loop thread only.
struct PeerHost {
    virtual ~PeerHost() = default;
    virtual const InfoHash& hostInfoHash() const = 0;
    virtual const std::string& hostPeerId() const = 0;
    virtual bool metadataReady() const = 0;
    virtual int metadataNumPieces() const = 0;      // valid only when metadataReady()
    virtual int64_t metadataTotalSize() const = 0;  // 0 until learned from a peer

    virtual void onPeerHandshake(PeerConnection*) = 0;
    virtual void onPeerBlock(PeerConnection*, int piece, int32_t begin, const uint8_t* data, int len) = 0;
    virtual void onMetadataPiece(int piece, int64_t totalSize, const uint8_t* data, int len) = 0;
    virtual void onPexPeers(const std::vector<PeerAddr>&) = 0;
};

class PeerConnection {
public:
    enum class State { Idle, Connecting, MseHandshake, HandshakeWait, Ready, Closed };

    /// `transport` is the carrier (TcpTransport or a µTP transport from the
    /// UtpManager) — the engine picks it per its connection policy. The peer state
    /// machine (handshake / MSE / wire messages) is identical on either carrier.
    PeerConnection(const PeerAddr& addr, PeerHost* host, std::unique_ptr<PeerTransport> transport,
        Encryption enc = Encryption::Plaintext);

    bool startConnect();
    void close();

    // ---- event-loop hooks ----
    net::Handle handle() const { return transport_ ? transport_->handle() : net::invalidHandle(); }
    /// True for a TCP peer (its fd goes in the engine's select() set); false for a
    /// µTP peer (serviced through the shared UDP socket instead).
    bool pollable() const { return transport_ && transport_->pollable(); }
    bool wantWrite() const;  // connecting, or bytes queued
    bool alive() const { return state_ != State::Closed; }
    void onReadable();
    void onWritable();
    void onTick(int64_t nowMs);  // keep-alive + connect timeout

    // ---- state for the scheduler ----
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
    /// True if this connection attempted MSE (used by the engine's plaintext
    /// fallback: an MSE peer that dies before its BitTorrent handshake gets one
    /// clear-text reconnect).
    bool usedMse() const { return enc_ != Encryption::Plaintext; }
    bool handshaked() const { return handshakeParsed_; }
    /// The encryption policy this connection was started with — the engine's
    /// TCP/µTP fallback ladder reads it to decide the next attempt.
    Encryption encryptionMode() const { return enc_; }

    // ---- actions the scheduler issues ----
    void sendInterested();
    void sendRequest(int piece, int32_t begin, int32_t length);
    void sendMetadataRequest(int piece);
    /// BEP-11: advertise the delta of currently-known peers to this peer (added /
    /// dropped since the last call). No-op until the peer advertised ut_pex.
    void sendPex(const std::vector<PeerAddr>& current);
    /// Called by the engine once metadata is known: size the have-bitmap and
    /// apply any bitfield/have received during the metadata phase.
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
    void onMseComplete(const std::string& appData);  // install ciphers, resume BT

    PeerAddr addr_;
    PeerHost* host_;
    std::unique_ptr<PeerTransport> transport_;  // TCP or µTP carrier (see transport.hpp)
    State state_ = State::Idle;
    Encryption enc_ = Encryption::Plaintext;

    std::string inbuf_;
    std::string outbuf_;
    size_t outSent_ = 0;

    // MSE/PE: the crypto handshake runs before the BitTorrent handshake; once it
    // resolves, the wire is either RC4 (ciphers active) or plaintext.
    std::unique_ptr<MseHandshake> mse_;
    std::string btHandshake_;  // our BEP-3 handshake bytes (the MSE "IA")
    Rc4 sendCipher_;
    Rc4 recvCipher_;
    bool sendEncrypted_ = false;
    bool recvEncrypted_ = false;

    bool handshakeParsed_ = false;
    bool extSupported_ = false;
    bool peerChoking_ = true;
    bool amChoking_ = true;
    bool amInterested_ = false;
    bool peerInterested_ = false;
    int outstanding_ = 0;

    std::vector<uint8_t> peerHas_;  // 1 byte/piece; sized once metadata known
    std::string pendingBitfield_;   // bitfield seen before metadata was ready
    std::vector<int> pendingHaves_;

    int peerUtMetadataId_ = 0;  // id to use when SENDING ut_metadata to this peer
    int peerUtPexId_ = 0;
    int64_t peerMetadataSize_ = 0;
    std::vector<std::string> pexAdvertised_;  // endpoints already sent to this peer (BEP-11 delta)

    int64_t connectStartMs_ = 0;
    int64_t lastSendMs_ = 0;

    static constexpr int kUtMetadataLocalId = 1;  // id peers use to send us ut_metadata
    static constexpr int kUtPexLocalId = 2;
};

}  // namespace torrent
