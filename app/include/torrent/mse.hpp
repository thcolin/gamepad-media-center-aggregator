// MSE/PE (Vuze spec), initiator side only: the engine always connects out.
// If crypto_select is RC4 the wire stays encrypted (KeyA out, KeyB in);
// otherwise only the handshake headers are. Engine loop thread only.

#pragma once

#include <cstdint>
#include <string>

#include "torrent/crypto.hpp"
#include "torrent/types.hpp"

namespace torrent {

class MseHandshake {
public:
    enum class Status { NeedMore, Done, Failed };

    /// `ia` is our BitTorrent handshake, sent in step 3. `requireRc4` offers RC4
    /// only; otherwise plaintext and RC4.
    MseHandshake(const InfoHash& infoHash, std::string ia, bool requireRc4);

    /// Step 1, sent once right after connect.
    std::string firstFlight();

    /// `toSend` receives step 3. On Done, `appData` holds the start of the peer's
    /// BitTorrent stream, already decrypted.
    Status feed(const uint8_t* data, size_t len, std::string& toSend, std::string& appData);

    /// Valid after Done. If true, the caller keeps using the two ciphers below.
    bool rc4() const { return rc4_; }
    Rc4& sendCipher() { return encA_; }
    Rc4& recvCipher() { return decB_; }

private:
    enum class Phase { ExpectYb, SyncVc, Header, Body, Failed, Done };

    void deriveKeys(const uint8_t secret[mse::kKeyLen]);
    std::string buildStep3(const uint8_t secret[mse::kKeyLen]);

    InfoHash infoHash_;
    std::string ia_;
    bool requireRc4_;

    Phase phase_ = Phase::ExpectYb;
    std::string priv_;
    std::string rx_;
    std::string vcPattern_;

    Rc4 encA_;
    Rc4 decB_;
    bool step3Sent_ = false;
    bool hdrDecoded_ = false;
    int padDRemaining_ = 0;
    bool rc4_ = false;
};

}  // namespace torrent
