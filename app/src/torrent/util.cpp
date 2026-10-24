#include "torrent/util.hpp"

#include <chrono>
#include <cstdint>

// No std::random_device on Switch and Vita: their libstdc++ reads /dev/urandom,
// which does not exist there, and the first draw crashes on a NULL FILE*.
#if defined(__SWITCH__)
#include <switch.h>
#elif defined(__vita__)
#include <psp2/kernel/rng.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
// PS4 (OpenOrbis) has /dev/urandom but no arc4random_buf.
#include <fcntl.h>
#include <random>
#include <unistd.h>
#endif

namespace torrent {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void osRandom(void* buf, size_t n) {
    if (n == 0) return;
#if defined(__SWITCH__)
    randomGet(buf, n);
#elif defined(__vita__)
    sceKernelGetRandomNumber(buf, n);
#elif defined(__APPLE__)
    arc4random_buf(buf, n);
#else
    auto* p = static_cast<unsigned char*>(buf);
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = ::read(fd, p + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        ::close(fd);
        if (got == n) return;
    }
    // No /dev/urandom in the sandbox: std::random_device would read it too, so
    // seed from the clock. These bytes feed peer ids, DHT ids and MSE, none secret.
    static thread_local std::mt19937_64 rng((uint64_t)nowMs() ^ (uint64_t)(uintptr_t)&n);
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(rng() & 0xFF);
#endif
}

std::string randomBytes(size_t n) {
    std::string s;
    s.resize(n);
    if (n) osRandom(&s[0], n);
    return s;
}

std::string makePeerId(const std::string& prefix) {
    std::string id = prefix;
    if (id.size() > 20) id.resize(20);
    id += randomBytes(20 - id.size());
    return id;
}

}  // namespace torrent
