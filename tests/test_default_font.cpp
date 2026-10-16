// Standalone logic test: default subtitle font install.
//
//   c++ -std=gnu++17 -Iapp/include tests/test_default_font.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utils/default_font.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static void put(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    char tpl[] = "/tmp/dfXXXXXX";
    std::string d = mkdtemp(tpl);
    std::string src = d + "/src.ttf", dst = d + "/subfont.ttf";
    std::string big(100000, 'x');
    put(src, big);

    CHECK(DefaultFont::install(src, dst) == DefaultFont::Result::Installed);
    CHECK(slurp(dst) == big);
    CHECK(!DefaultFont::exists(dst + ".tmp"));

    put(src, "changed");
    CHECK(DefaultFont::install(src, dst) == DefaultFont::Result::AlreadyThere);
    CHECK(slurp(dst) == big);

    std::string mine = d + "/mine.ttf";
    put(mine, "user font");
    CHECK(DefaultFont::install(src, mine) == DefaultFont::Result::AlreadyThere);
    CHECK(slurp(mine) == "user font");

    CHECK(DefaultFont::install(d + "/missing.ttf", d + "/other.ttf") == DefaultFont::Result::Failed);
    CHECK(!DefaultFont::exists(d + "/other.ttf.tmp"));
    CHECK(DefaultFont::install(src, d + "/nodir/x.ttf") == DefaultFont::Result::Failed);

    // a zero-byte leftover does not count as a font
    put(d + "/empty.ttf", "");
    CHECK(DefaultFont::install(src, d + "/empty.ttf") == DefaultFont::Result::Installed);

    if (failures == 0) printf("PASS\n");
    return failures ? 1 : 0;
}
