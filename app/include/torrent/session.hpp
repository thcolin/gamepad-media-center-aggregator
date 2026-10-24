#pragma once

#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "torrent/types.hpp"

namespace torrent {

class TorrentEngine;

/// Owns the single engine of the current playback: open() tears down the previous one.
class EngineSession {
public:
    static EngineSession& instance();
    ~EngineSession();

    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    /// Called on the UI thread when a playback starts; open() needs its value.
    uint64_t expect();

    /// Returns the local URL for mpv, or "" (also when a close() came after expect()).
    /// Blocks on the metadata: call from a worker thread.
    std::string open(
        uint64_t generation, const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources);

    /// Does not wait for the engine's threads, except a previous teardown still running.
    void close();

    Stats stats() const;
    bool active() const;

private:
    EngineSession();
    void joinPending();
    void teardown();

    mutable std::mutex mutex_;
    std::shared_ptr<TorrentEngine> engine_;
    std::thread pending_;  // teardown of the previous engine
    uint64_t generation_ = 0;
};

}  // namespace torrent
