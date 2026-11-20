// Standalone logic test — the PS4 update helper's status line (utils/ps4_update_status.hpp).
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_ps4_update_status.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <utils/ps4_update_status.hpp>

using namespace ps4::update;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static Outcome outcome(const std::string& line, int64_t now, const std::string& version, std::string& why) {
    why.clear();
    return outcomeOf(parseStatus(line), now, version, why);
}

int main() {
    // what the helper writes, read back
    CHECK(formatStatus("waiting", 1000, "v1.4.1") == "waiting 1000 v1.4.1");
    CHECK(formatStatus("done", 1000, "") == "done 1000");
    Status s = parseStatus("failed 1700000000 GMCA did not close\n");
    CHECK(s.state == "failed" && s.time == 1700000000 && s.detail == "GMCA did not close");
    s = parseStatus("installing 1700000000 v1.4.1\r\n");
    CHECK(s.state == "installing" && s.time == 1700000000 && s.detail == "v1.4.1");
    s = parseStatus("done 1700000000");
    CHECK(s.state == "done" && s.time == 1700000000 && s.detail.empty());

    // lines from a helper without the time
    s = parseStatus("waiting");
    CHECK(s.state == "waiting" && s.time == 0 && s.detail.empty());
    s = parseStatus("done 1.4.5");
    CHECK(s.state == "done" && s.time == 0 && s.detail == "1.4.5");
    s = parseStatus("failed 404 not found");  // a reason that starts with a number reads as a time
    CHECK(s.state == "failed" && s.time == 404 && s.detail == "not found");
    s = parseStatus("");
    CHECK(s.state.empty());

    std::string why;
    const int64_t t = 1700000000;
    CHECK(outcome("", t, "1.4.1", why) == Outcome::None);
    CHECK(outcome("done 1700000000 v1.4.1", t, "1.4.1", why) == Outcome::Done);
    CHECK(outcome("failed 1700000000 the package is not for this app", t, "1.4.0", why) == Outcome::Failed);
    CHECK(why == "the package is not for this app");

    // a helper still at work
    CHECK(outcome("waiting 1700000000 v1.4.1", t + waitingLimit, "1.4.0", why) == Outcome::Running);
    CHECK(outcome("installing 1700000000 v1.4.1", t + installingLimit, "1.4.0", why) == Outcome::Running);

    // a helper that died: too old, or no time at all
    CHECK(outcome("waiting 1700000000 v1.4.1", t + waitingLimit + 1, "1.4.0", why) == Outcome::Failed);
    CHECK(why == "the update helper stopped");
    CHECK(outcome("installing 1700000000 v1.4.1", t + installingLimit + 1, "1.4.0", why) == Outcome::Failed);
    CHECK(outcome("installing", t, "1.4.0", why) == Outcome::Failed);
    CHECK(outcome("waiting", t, "1.4.0", why) == Outcome::Failed);

    // ... after the install went through: the app runs the new version
    CHECK(outcome("installing 1700000000 v1.4.1", t + installingLimit + 1, "1.4.1", why) == Outcome::Done);
    CHECK(outcome("installing 1700000000 1.4.1", t + installingLimit + 1, "v1.4.1", why) == Outcome::Done);
    // a waiting helper never started the install, whatever runs now
    CHECK(outcome("waiting 1700000000 v1.4.1", t + waitingLimit + 1, "1.4.1", why) == Outcome::Failed);

    if (failures) return 1;
    printf("ps4_update_status: all checks passed\n");
    return 0;
}
