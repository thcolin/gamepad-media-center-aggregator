/*
    GMCA — MSE/PE outgoing handshake (Message Stream Encryption / Protocol
    Encryption, Vuze spec).

    We are ALWAYS the connecting side (leecher), so only the initiator half is
    implemented. The five-step exchange:

        1 A->B: Ya, PadA                          (our DH public key + pad)
        2 B->A: Yb, PadB                          (peer DH public key + pad)
        3 A->B: HASH('req1', S),                  (skip-pad marker)
                HASH('req2', SKEY) xor HASH('req3', S),
                ENCRYPT(VC, crypto_provide, len(PadC), PadC, len(IA)),
                ENCRYPT(IA)                       (IA = our BitTorrent handshake)
        4 B->A: ENCRYPT(VC, crypto_select, len(PadD), PadD),
                ENCRYPT2(payload)                 (peer's BitTorrent handshake…)
        5 A->B: ENCRYPT2(payload)                 (continues over the peer loop)

    S is the DH shared secret; SKEY is the infohash. VC = 8 zero bytes; the
    initiator locates B's encrypted VC in the byte stream to skip the unknown-length
    PadB (resynchronisation). If crypto_select == RC4 the wire stays RC4-encrypted
    (KeyA for A->B, KeyB for B->A); if plaintext, only step 3/4 headers were
    encrypted and the payload flows in clear.

    This drives the crypto; the peer connection owns the socket buffering and feeds
    bytes in via feed(). Not thread-safe; lives on the engine loop thread only.
*/

#pragma once

#include <cstdint>
#include <string>

#include "torrent/crypto.hpp"
#include "torrent/types.hpp"

namespace torrent {

class MseHandshake {
public:
    enum class Status { NeedMore, Done, Failed };

    /// `infoHash` is SKEY. `ia` is the plaintext initial payload (our 68-byte
    /// BitTorrent handshake) sent encrypted in step 3. `requireRc4` advertises
    /// RC4 only (Forced policy); otherwise plaintext+RC4 (Prefer policy).
    MseHandshake(const InfoHash& infoHash, std::string ia, bool requireRc4);

    /// Bytes to transmit right after TCP connect (step 1). Call once.
    std::string firstFlight();

    /// Feed freshly-received raw bytes. `toSend` collects any bytes we must now
    /// transmit (step 3, emitted once). On Done, `appData` holds the decrypted (or
    /// plaintext) application bytes that followed the handshake — the beginning of
    /// the peer's BitTorrent stream.
    Status feed(const uint8_t* data, size_t len, std::string& toSend, std::string& appData);

    /// Valid after Done. If rc4() the caller keeps ciphering the wire with the
    /// lifted cipher states; otherwise the wire is plaintext from here on.
    bool rc4() const { return rc4_; }
    Rc4& sendCipher() { return encA_; }  // A->B stream, already advanced past IA
    Rc4& recvCipher() { return decB_; }  // B->A stream, already advanced past PadD

private:
    enum class Phase { ExpectYb, SyncVc, Header, Body, Failed, Done };

    void deriveKeys(const uint8_t secret[mse::kKeyLen]);
    std::string buildStep3(const uint8_t secret[mse::kKeyLen]);

    InfoHash infoHash_;
    std::string ia_;
    bool requireRc4_;

    Phase phase_ = Phase::ExpectYb;
    std::string priv_;    // our DH private exponent
    std::string rx_;      // undecrypted receive accumulator
    std::string vcPattern_;  // expected encrypted VC (8 bytes) for the PadB skip

    Rc4 encA_;  // A->B RC4 (KeyA)
    Rc4 decB_;  // B->A RC4 (KeyB)
    bool step3Sent_ = false;
    bool hdrDecoded_ = false;
    int padDRemaining_ = 0;
    bool rc4_ = false;
};

}  // namespace torrent
