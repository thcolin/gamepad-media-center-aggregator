#pragma once

/*
    DefaultFont: copies a bundled TTF to {config}/subfont.ttf when none is
    there. The PS4 libass has no font provider, so that file is its only
    fallback font and without it text subtitles draw nothing.

    Plain POSIX calls only (std::filesystem fails on the PS4). The copy goes
    to "{dst}.tmp" and is renamed into place, so a cut mid-copy never leaves a
    half-written font. A font that is already at dst (the user's own) is left
    alone.
*/

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>
#include <string>

namespace DefaultFont {

enum class Result { Installed, AlreadyThere, Failed };

inline bool exists(const std::string& path) {
    // No stat(): on the PS4 musl's stat family goes through fstatat, which is
    // an ENOSYS stub, so it would always report "missing".
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    off_t size = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    return size > 0;
}

inline Result install(const std::string& src, const std::string& dst) {
    if (exists(dst)) return Result::AlreadyThere;
    const std::string tmp = dst + ".tmp";
    int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) return Result::Failed;
    int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        ::close(in);
        return Result::Failed;
    }
    char buf[16384];
    bool ok = true;
    long total = 0;
    for (;;) {
        ssize_t n = ::read(in, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = ::write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
        total += n;
    }
    ::close(in);
    if (::close(out) != 0) ok = false;
    if (!ok || total == 0 || ::rename(tmp.c_str(), dst.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return Result::Failed;
    }
    return Result::Installed;
}

}  // namespace DefaultFont
