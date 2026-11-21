// Standalone logic test — SHA-256 of a downloaded update and its check against
// the release asset's `digest`.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_sha256.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include <unistd.h>
#include <utils/sha256.hpp>

static int failures = 0;
#define CHECK_EQ(got, want)                                                              \
    do {                                                                                 \
        std::string g = (got), w = (want);                                               \
        if (g != w) {                                                                    \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str()); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

static std::string hash(const std::string& s) {
    misc::Sha256 sha;
    sha.update(s.data(), s.size());
    return sha.hex();
}

static bool throws(const std::string& path, const std::string& digest) {
    try {
        misc::verifyDigest(path, digest);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

int main() {
    // FIPS 180-2 vectors
    CHECK_EQ(hash(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(hash("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK_EQ(hash(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // a message cut in uneven chunks hashes like the whole message
    std::string msg(200003, 'x');
    for (size_t i = 0; i < msg.size(); i++) msg[i] = char(i * 31);
    misc::Sha256 chunked;
    for (size_t pos = 0, n = 1; pos < msg.size(); pos += n, n = n * 3 % 997 + 1)
        chunked.update(msg.data() + pos, std::min(n, msg.size() - pos));
    CHECK_EQ(chunked.hex(), hash(msg));

    // the file is read past the 64 KB buffer
    char tmpl[] = "/tmp/gmca_sha256_XXXXXX";
    close(mkstemp(tmpl));
    std::string path = tmpl;
    {
        std::ofstream out(path, std::ios::binary);
        out << msg;
    }
    CHECK_EQ(misc::sha256File(path), hash(msg));

    std::string good = "sha256:" + hash(msg);
    std::string upper = good;
    for (size_t i = 7; i < upper.size(); i++) upper[i] = char(std::toupper(static_cast<unsigned char>(upper[i])));
    std::string wrong = good;
    wrong.back() = wrong.back() == '0' ? '1' : '0';

    CHECK_EQ(throws(path, good) ? "throws" : "ok", "ok");
    CHECK_EQ(throws(path, upper) ? "throws" : "ok", "ok");
    CHECK_EQ(throws(path, wrong) ? "throws" : "ok", "throws");
    CHECK_EQ(throws(path, "") ? "throws" : "ok", "throws");
    CHECK_EQ(throws(path, "sha512:" + hash(msg)) ? "throws" : "ok", "throws");
    CHECK_EQ(throws(path, good + "00") ? "throws" : "ok", "throws");
    CHECK_EQ(throws(path + ".missing", good) ? "throws" : "ok", "throws");
    std::remove(path.c_str());

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_sha256: all passed\n");
    return 0;
}
