// The PS4 update helper's status line (utils/ps4_self_update.hpp), shared by the helper, which
// writes it, and the app, which reads it. Plain C++ with no PS4 dependency, so it is tested on the
// desktop (tests/test_ps4_update_status.cpp).
//
// The line is "<state> <time> <detail>": the state is waiting, installing, done or failed, the
// time is when the helper wrote it (seconds since 1970), and the detail is the version being
// installed, or the reason for a failure. Helpers built before the time was added wrote
// "<state> <detail>".
#pragma once

#include <cstdint>
#include <string>

namespace ps4::update {

struct Status {
    std::string state;  // "" when there is no status
    int64_t time = 0;   // 0 when the line has none
    std::string detail;
};

inline std::string formatStatus(const std::string& state, int64_t time, const std::string& detail) {
    return state + " " + std::to_string(time) + (detail.empty() ? "" : " " + detail);
}

inline Status parseStatus(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    Status s;
    const size_t a = line.find(' ');
    s.state = line.substr(0, a);
    if (a == std::string::npos) return s;
    std::string rest = line.substr(a + 1);
    const size_t b = rest.find(' ');
    const std::string word = rest.substr(0, b);
    bool digits = !word.empty();
    for (char c : word) digits = digits && c >= '0' && c <= '9';
    if (digits) {
        s.time = std::stoll(word);
        rest = b == std::string::npos ? "" : rest.substr(b + 1);
    }
    s.detail = rest;
    return s;
}

/// How long the helper can stay in a state: it waits at most a minute for the app to close, and
/// gives up on an install after 30 minutes. A status older than that is from a helper that died
/// (a crash, the console turned off).
constexpr int64_t waitingLimit = 5 * 60;
constexpr int64_t installingLimit = 40 * 60;

enum class Outcome { None, Running, Done, Failed };

/// What a status means at `now`. A waiting or installing status past its limit, or without a
/// time, is a helper that died: Failed with `why` set, unless the app already runs the version it
/// was installing (`appVersion`; a "v" in front of either is ignored), in which case the install
/// got through and only the helper's last word is missing.
inline Outcome outcomeOf(const Status& s, int64_t now, const std::string& appVersion, std::string& why) {
    if (s.state.empty()) return Outcome::None;
    if (s.state == "done") return Outcome::Done;
    if (s.state == "failed") {
        why = s.detail;
        return Outcome::Failed;
    }
    const int64_t limit = s.state == "waiting" ? waitingLimit : installingLimit;
    if (s.time > 0 && now - s.time <= limit) return Outcome::Running;
    auto bare = [](const std::string& v) { return v.rfind('v', 0) == 0 ? v.substr(1) : v; };
    if (s.state == "installing" && !s.detail.empty() && bare(s.detail) == bare(appVersion)) return Outcome::Done;
    why = "the update helper stopped";
    return Outcome::Failed;
}

}  // namespace ps4::update
