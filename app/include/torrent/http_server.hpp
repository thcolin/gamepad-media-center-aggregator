/*
    GMCA — Local HTTP server: torrent piece store -> mpv.

    The whole point of the "engine -> local HTTP -> player" model (TORRENT_
    STREAMING.md §2): mpv already speaks HTTP Range/seek, so the player is
    unchanged — it just opens http://127.0.0.1:PORT/… and the server feeds it
    bytes of the pinned file as pieces land.

    Semantics: one file, GET/HEAD, byte-range (206 Partial Content). A read for a
    not-yet-downloaded region BLOCKS on the piece store until the covering pieces
    arrive — and the requested offset is pushed to the store as the playhead, so
    the sequential picker chases the player's read position (seek support).

    Runs on its own thread(s). Uses blocking BSD sockets directly (bind/listen/
    accept) — the one place the engine needs a listening socket. Console seam:
    libnx/openorbis expose bind/listen/accept as-is; Vita uses sceNetListen /
    sceNetAccept (TODO).
*/

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

    /// Bind 127.0.0.1:port (0 = ephemeral) and start accepting. False on failure.
    bool start(uint16_t port);
    void stop();

    uint16_t port() const { return port_; }
    std::string url() const;  // http://127.0.0.1:PORT/<file>

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
