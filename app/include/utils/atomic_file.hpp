#pragma once

/*
    AtomicFile — write() goes through "{path}.tmp" and a rename, so a cut
    mid-write never leaves a truncated {path}. Horizon (Switch) refuses to
    rename onto an existing file: the old one is removed first, and read()
    picks up the complete .tmp if the cut lands between remove and rename.
*/

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "utils/misc.hpp"

namespace AtomicFile {

#if !defined(USE_BOOST_FILESYSTEM) || defined(_WIN32)
inline fs::path toPath(const std::string& p) { return fs::u8path(p); }
inline const fs::path& streamPath(const fs::path& p) { return p; }
#else
inline fs::path toPath(const std::string& p) { return p; }
inline std::string streamPath(const fs::path& p) { return p.string(); }
#endif

inline void write(const std::string& path, const std::string& content) {
    const fs::path dst = toPath(path), tmp = toPath(path + ".tmp");
    std::ofstream f(streamPath(tmp), std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    if (f.fail()) throw std::runtime_error("cannot write " + path + ".tmp");
    try {
        fs::rename(tmp, dst);
    } catch (const std::exception&) {
        fs::remove(dst);
        fs::rename(tmp, dst);
    }
}

inline bool read(const std::string& path, std::string& out) {
    fs::path src = toPath(path);
    if (!fs::exists(src)) {
        src = toPath(path + ".tmp");
        if (!fs::exists(src)) return false;
    }
    std::ifstream f(streamPath(src), std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("cannot open " + src.string());
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline std::string quarantine(const std::string& path) {
    const std::string bak = path + ".bak";
    fs::remove(toPath(bak));
    fs::rename(toPath(path), toPath(bak));
    return bak;
}

}  // namespace AtomicFile
