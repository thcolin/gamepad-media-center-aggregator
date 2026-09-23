/*
    GMCA — Tiny logging shim for the torrent engine.

    The engine is standalone (it must build without borealis for the PoC and the
    console ports), so it cannot use brls::Logger directly. Instead it emits
    through a settable callback: the PoC wires it to stderr; the borealis
    integration will forward it to brls::Logger. Default = stderr.
*/

#pragma once

#include <functional>
#include <string>

namespace torrent {

enum class LogLevel { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

/// Install a log sink (thread-safe to call once at startup). Passing {} restores
/// the default stderr sink.
void setLogSink(LogSink sink);

void logMsg(LogLevel level, const std::string& msg);

// printf-style helpers implemented with a small vsnprintf wrapper.
void logDebug(const char* fmt, ...);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

}  // namespace torrent
