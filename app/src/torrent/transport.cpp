/*
    GMCA — TCP transport (see torrent/transport.hpp).

    The µTP transport lives in utp.cpp (it needs the libutp context owned by the
    UtpManager); this file is only the TCP carrier, kept identical to the original
    per-peer socket path.
*/

#include "torrent/transport.hpp"

namespace torrent {

bool TcpTransport::startConnect(const PeerAddr& addr) {
    if (!sock_.open()) return false;
    if (!sock_.startConnect(addr.host, addr.port)) {
        sock_.close();
        return false;
    }
    return true;
}

}  // namespace torrent
