#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace torrent {
namespace bencode {

/// BEP-3 bencode. Strings are binary; std::map keeps dict keys in canonical order.
struct Value {
    enum class Type { Int, Str, List, Dict };
    Type type = Type::Int;
    int64_t i = 0;
    std::string s;
    std::vector<Value> list;
    std::map<std::string, Value> dict;

    // source span, so the infohash can hash the raw info dict of a .torrent
    size_t begin = 0;
    size_t end = 0;

    bool isInt() const { return type == Type::Int; }
    bool isStr() const { return type == Type::Str; }
    bool isList() const { return type == Type::List; }
    bool isDict() const { return type == Type::Dict; }

    // return the default when the key is missing or of another type
    const Value* find(const std::string& key) const;
    int64_t asInt(int64_t def = 0) const { return type == Type::Int ? i : def; }
    const std::string& asStr() const { return s; }
    int64_t intAt(const std::string& key, int64_t def = 0) const;
    std::string strAt(const std::string& key, const std::string& def = "") const;
};

/// Advances `pos` past the value.
bool decode(const uint8_t* data, size_t len, size_t& pos, Value& out);

bool decode(const std::string& buf, Value& out);

std::string encode(const Value& v);

}  // namespace bencode
}  // namespace torrent
