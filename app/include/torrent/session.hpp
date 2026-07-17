/*
    GMCA — TorrentEngine session holder (app integration glue).

    The engine is EPHEMERAL: it lives only for the duration of one playback and is
    torn down on stop/close (TORRENT_STREAMING.md §1). This singleton owns that
    single engine so the two call sites — StremioBackend::resolvePlayback (opens
    it, on a worker thread) and PlayerView teardown (closes it, on the UI thread) —
    stay decoupled from each other and from the concrete engine type.

    Only one torrent playback at a time: open() first tears down any previous
    engine. The (potentially slow — tracker round-trips) teardown runs on a
    detached thread so the UI-thread destructor never blocks; the next open()
    joins it before starting, preserving the one-at-a-time invariant.

    Compiled into the app only when ENABLE_TORRENT (desktop/switch) — see the root
    CMakeLists integration block. Not referenced anywhere when the option is OFF.
*/

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "torrent/types.hpp"  // Stats

namespace torrent {

class TorrentEngine;  // fwd — implementation pulls in the full engine

class EngineSession {
public:
    static EngineSession& instance();
    ~EngineSession();

    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    /// Stand up the engine for `infoHash` (+ selected file index and the addon's
    /// tracker/DHT hints) and return the local HTTP URL mpv should open, or "" on
    /// failure. Blocks on metadata acquisition (cold magnet) — call from a worker
    /// thread. Tears down any previous engine first (one playback at a time).
    std::string open(const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources);

    /// Ephemeral teardown. Safe to call when nothing is open. Does not block the
    /// caller on the engine's thread joins (they run on a detached closer).
    void close();

    /// Snapshot for the buffering UI (default-constructed Stats when idle).
    Stats stats() const;
    bool active() const;

private:
    EngineSession() = default;
    void joinPending();  // finish any in-flight detached teardown

    mutable std::mutex mutex_;
    std::shared_ptr<TorrentEngine> engine_;
    std::thread pending_;  // detached-teardown thread of the previous engine
};

}  // namespace torrent
