// Listing a folder with only the calls that work on the PS4. OpenOrbis musl's fstatat is a stub
// that fails with ENOSYS (and its lstat goes through it), so std::filesystem's
// directory_iterator, is_directory, file_size and last_write_time fail there. Names come from
// opendir/readdir; a name is a folder if it opens as one and reads without error, else a file
// whose size is lseek(SEEK_END) on an open fd. No modification time: no stat call that works
// there fills one in a known layout (0 keeps the "Date" sort stable). Builds and behaves the same
// on Linux, which is how it is tested.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace remote {
namespace posix_list {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

/// True for an entry the file system kept after a delete: FreeBSD's UFS hands them out with inode
/// 0, and musl's readdir doesn't skip them like FreeBSD's libc does. (The OpenOrbis header names
/// the field d_fileno, then #defines d_fileno to d_ino, a field it doesn't have.)
inline bool isDeletedEntry(const struct dirent* entry) {
#ifdef __PS4__
#pragma push_macro("d_fileno")
#undef d_fileno
    return entry->d_fileno == 0;
#pragma pop_macro("d_fileno")
#else
    return entry->d_ino == 0;
#endif
}

/// The names in `dir` ("." and ".." left out). False, with `error` set to "<call> <dir>: <reason>",
/// if it can't be opened or read to the end; the names read until then are kept.
inline bool listNames(const std::string& dir, std::vector<std::string>& names, std::string& error) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        const int err = errno;
        error = "opendir " + dir + ": " + std::strerror(err);
        return false;
    }
    bool ok = true;
    for (;;) {
        errno = 0;
        const struct dirent* e = readdir(d);
        if (!e) {
            // NULL with errno unchanged is the end. On the PS4, musl's readdir reports any
            // getdents failure as EPERM.
            const int err = errno;
            if (err != 0) {
                error = "readdir " + dir + ": " + std::strerror(err);
                ok = false;
            }
            break;
        }
        if (isDeletedEntry(e)) continue;
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(d);
    return ok;
}

/// True when `path` opens as a folder and reads to its first entry (or its end) without an error:
/// on the PS4 a file can pass opendir, and then its first readdir fails.
inline bool isFolder(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    errno = 0;
    const struct dirent* e = readdir(d);
    bool ok = e != nullptr || errno == 0;
    closedir(d);
    return ok;
}

/// A file's size in bytes, 0 when it can't be opened.
inline uint64_t fileSize(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    off_t end = lseek(fd, 0, SEEK_END);
    close(fd);
    return end > 0 ? (uint64_t)end : 0;
}

/// The entries of `dir` ("." and ".." left out). Never throws: a folder that can't be read lists
/// what was read before the failure, and `error` says why ("" when it all read).
inline std::vector<Entry> list(const std::string& dir, std::string& error) {
    std::vector<std::string> names;
    listNames(dir, names, error);
    std::vector<Entry> out;
    out.reserve(names.size());
    const std::string base = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    for (auto& n : names) {
        Entry e;
        e.name = n;
        std::string full = base + n;
        e.dir = isFolder(full);
        if (!e.dir) e.size = fileSize(full);
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace posix_list
}  // namespace remote
