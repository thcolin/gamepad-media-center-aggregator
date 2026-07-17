/*
    GMCA — torrent engine logging shim (see torrent/log.hpp).
*/

#include "torrent/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace torrent {

namespace {
std::mutex g_mutex;
LogSink g_sink;

const char* levelTag(LogLevel l) {
    switch (l) {
    case LogLevel::Debug:
        return "DBG";
    case LogLevel::Info:
        return "INF";
    case LogLevel::Warning:
        return "WRN";
    case LogLevel::Error:
        return "ERR";
    }
    return "?";
}

std::string vformat(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return {};
    std::vector<char> buf((size_t)n + 1);
    vsnprintf(buf.data(), buf.size(), fmt, ap);
    return std::string(buf.data(), (size_t)n);
}
}  // namespace

void setLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_sink = std::move(sink);
}

void logMsg(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_sink) {
        g_sink(level, msg);
    } else {
        fprintf(stderr, "[torrent %s] %s\n", levelTag(level), msg.c_str());
        fflush(stderr);
    }
}

#define TORRENT_LOG_IMPL(FN, LEVEL)       \
    void FN(const char* fmt, ...) {       \
        va_list ap;                       \
        va_start(ap, fmt);                \
        std::string s = vformat(fmt, ap); \
        va_end(ap);                       \
        logMsg(LEVEL, s);                 \
    }

TORRENT_LOG_IMPL(logDebug, LogLevel::Debug)
TORRENT_LOG_IMPL(logInfo, LogLevel::Info)
TORRENT_LOG_IMPL(logWarn, LogLevel::Warning)
TORRENT_LOG_IMPL(logError, LogLevel::Error)

#undef TORRENT_LOG_IMPL

}  // namespace torrent
