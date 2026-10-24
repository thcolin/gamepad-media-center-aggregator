// The engine builds without borealis, so it logs through a settable sink.

#pragma once

#include <functional>
#include <string>

namespace torrent {

enum class LogLevel { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

/// {} restores the default stderr sink.
void setLogSink(LogSink sink);

void logMsg(LogLevel level, const std::string& msg);

void logDebug(const char* fmt, ...);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

}  // namespace torrent
