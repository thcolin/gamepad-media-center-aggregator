// Serves one file of the piece store to mpv over loopback HTTP (GET/HEAD,
// Range). A read of a missing region blocks until its pieces arrive, and each
// read offset becomes the picker's playhead, which is how seeking works.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "torrent/storage.hpp"

namespace torrent {

class HttpServer {
public:
    HttpServer(PieceStore& store, int fileIdx, const std::string& fileName);
    ~HttpServer();

    /// Binds 127.0.0.1; port 0 picks an ephemeral one.
    bool start(uint16_t port);
    void stop();

    uint16_t port() const { return port_; }
    std::string url() const;

private:
    void acceptLoop();
    void handleClient(int fd);

    PieceStore& store_;
    int fileIdx_;
    std::string fileName_;
    int64_t fileLength_;

    int listenFd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace torrent
