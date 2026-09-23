// Standalone logic test — crash-safe config.json replacement.
//
//   c++ -std=gnu++17 -arch x86_64 \
//       -Iapp/include -Ilibrary/borealis/library/include/borealis/extern \
//       tests/test_atomic_file.cpp -o /tmp/t && /tmp/t
//
// Runs on a real temp dir. The Switch fallback (rename refusing to replace an
// existing file) cannot be reproduced here: desktop rename replaces.

#include <sys/stat.h>
#include <cstdio>
#include <utils/atomic_file.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void put(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

int main() {
    const fs::path dir = fs::temp_directory_path() / "gmca_test_atomic_file";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "config.json").string();
    const std::string tmp = path + ".tmp", bak = path + ".bak";
    std::string out;

    CHECK(!AtomicFile::read(path, out));

    AtomicFile::write(path, "{\"a\":1}");
    CHECK(slurp(path) == "{\"a\":1}");
    AtomicFile::write(path, "{\"a\":2}");
    CHECK(slurp(path) == "{\"a\":2}");
    CHECK(!fs::exists(tmp));

    // a stale .tmp from a write cut short is ignored while config.json exists
    put(tmp, "{\"a\":");
    CHECK(AtomicFile::read(path, out) && out == "{\"a\":2}");
    AtomicFile::write(path, "{\"a\":3}");
    CHECK(slurp(path) == "{\"a\":3}" && !fs::exists(tmp));

    // cut between remove and rename (Switch fallback): the .tmp is picked up
    fs::rename(path, tmp);
    CHECK(AtomicFile::read(path, out) && out == "{\"a\":3}");
    AtomicFile::write(path, out);
    CHECK(slurp(path) == "{\"a\":3}" && !fs::exists(tmp));

    // an existing but unopenable file must not read as absent
    chmod(path.c_str(), 0);
    bool threw = false;
    try {
        AtomicFile::read(path, out);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    chmod(path.c_str(), 0644);

    put(bak, "old");
    put(path, "{\"a\":");
    CHECK(AtomicFile::quarantine(path) == bak);
    CHECK(!fs::exists(path) && slurp(bak) == "{\"a\":");

    const std::string missing = (dir / "nope" / "config.json").string();
    threw = false;
    try {
        AtomicFile::write(missing, "{}");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw && !fs::exists(missing));

    fs::remove_all(dir);
    if (failures == 0) {
        printf("test_atomic_file: OK\n");
        return 0;
    }
    printf("test_atomic_file: %d FAILURE(S)\n", failures);
    return 1;
}
