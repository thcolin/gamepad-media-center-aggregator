/*
    GMCA — engine utilities (see torrent/util.hpp).
*/

#include "torrent/util.hpp"

#include <chrono>
#include <cstdint>

// OS entropy source, picked at compile time. std::random_device is deliberately
// NOT used on the consoles: devkitA64 / vitasdk libstdc++ back it with
// /dev/urandom, which does not exist there — the constructor gets a NULL FILE*
// and the first draw faults (Data Abort at 0x0), crashing the app on the very
// first peer-id generation. Each console has a real CSPRNG with no fd:
#if defined(__SWITCH__)
#include <switch.h>  // randomGet (libnx userspace CSPRNG, always initialised)
#elif defined(__vita__)
#include <psp2/kernel/rng.h>  // sceKernelGetRandomNumber
#elif defined(__APPLE__)
#include <cstdlib>  // arc4random_buf (macOS — no fd, always available)
#else
// Desktop Linux/Windows + PS4 (openorbis is FreeBSD-based and has /dev/urandom
// but NOT arc4random_buf). /dev/urandom first, std::random_device last resort.
#include <fcntl.h>   // open  (/dev/urandom)
#include <random>    // last-resort std::random_device
#include <unistd.h>  // read/close
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
    // Last resort (Linux/Windows desktop where random_device is backed properly).
    static thread_local std::mt19937 rng((unsigned)std::random_device{}());
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
