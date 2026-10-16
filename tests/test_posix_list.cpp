// client/posix_list.hpp: the PS4 Files listing's logic, on Linux's POSIX calls.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include "client/posix_list.hpp"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    char tmpl[] = "/tmp/posixlistXXXXXX";
    std::string root = mkdtemp(tmpl);
    std::string cmd = "mkdir " + root + "/sub " + root + "/empty && printf 12345 > " + root + "/a.bin";
    CHECK(system(cmd.c_str()) == 0);
    std::string err;
    auto v = remote::posix_list::list(root, err);
    CHECK(err.empty());
    CHECK(v.size() == 3);
    for (auto& e : v) {
        if (e.name == "a.bin") CHECK(!e.dir && e.size == 5);
        else CHECK(e.dir && e.size == 0);
    }
    CHECK(remote::posix_list::isFolder(root + "/empty"));
    CHECK(!remote::posix_list::isFolder(root + "/a.bin"));
    auto none = remote::posix_list::list(root + "/missing", err);
    CHECK(none.empty() && !err.empty());  // never throws
    system(("rm -r " + root).c_str());
    std::printf(fails ? "test_posix_list: %d FAILED\n" : "test_posix_list: all passed\n", fails);
    return fails ? 1 : 0;
}
