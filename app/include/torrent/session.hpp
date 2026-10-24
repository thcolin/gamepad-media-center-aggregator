#pragma once

#include <memory>
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

    /// Returns the local URL for mpv, or "". Blocks on the metadata: call from a worker thread.
    std::string open(const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources);

    /// Does not wait for the engine's threads, except a previous teardown still running.
    void close();

    Stats stats() const;
    bool active() const;

private:
    EngineSession();
    void joinPending();

    mutable std::mutex mutex_;
    std::shared_ptr<TorrentEngine> engine_;
    std::thread pending_;  // teardown of the previous engine
};

}  // namespace torrent
