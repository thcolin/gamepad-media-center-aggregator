// Per-file options of mpv's loadfile command. Header-only, so tests/test_mpv_options.cpp builds it alone.
#pragma once

#include <string>

namespace mpvopt {

/// `extra` with `key=value` appended, comma-separated. The value is written in mpv's
/// `%byte length%` form, so commas, quotes and `=` in it are not read as separators.
inline std::string add(const std::string& extra, const std::string& key, const std::string& value) {
    std::string opt = key + "=%" + std::to_string(value.size()) + "%" + value;
    return extra.empty() ? opt : extra + "," + opt;
}

}  // namespace mpvopt
