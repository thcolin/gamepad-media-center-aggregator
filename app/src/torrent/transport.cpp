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
