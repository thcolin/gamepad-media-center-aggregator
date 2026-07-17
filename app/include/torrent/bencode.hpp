/*
    GMCA — Bencode (BEP-3) decoder/encoder, dependency-free.

    Bencode is BitTorrent's serialization format. We need it to parse tracker
    responses (BEP-3/BEP-23), the torrent `info` dictionary (BEP-9 ut_metadata /
    a .torrent file), and the extension-protocol handshake (BEP-10).

    The engine is intentionally standalone (no borealis, no nlohmann) so it can be
    lifted onto the console toolchains behind the socket shim — hence a small
    hand-rolled bencode rather than pulling a dependency.

    A `Value` records the byte span [begin, end) it was decoded from. That is how
    we recover the RAW bytes of the `info` dictionary from a .torrent file to hash
    its SHA-1 (the infohash) without a canonical re-encode. (Metadata fetched over
    BEP-9 already arrives as the raw info dict, so no extraction is needed there.)
*/

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace torrent {
namespace bencode {

/// A decoded bencode value. Strings are byte strings (may contain NUL / binary).
/// Dicts use std::map so keys stay sorted — which is also the canonical encoding
/// order required by BEP-3, so `encode()` round-trips to canonical form.
struct Value {
    enum class Type { Int, Str, List, Dict };
    Type type = Type::Int;
    int64_t i = 0;
    std::string s;
    std::vector<Value> list;
    std::map<std::string, Value> dict;

    // Byte span this value occupied in the source buffer (for raw info-dict slice).
    size_t begin = 0;
    size_t end = 0;

    bool isInt() const { return type == Type::Int; }
    bool isStr() const { return type == Type::Str; }
    bool isList() const { return type == Type::List; }
    bool isDict() const { return type == Type::Dict; }

    // Lenient accessors: return a default when the key/type is absent. Bencode
    // from the wild is inconsistent, mirroring the repo's jstr/jint helpers.
    const Value* find(const std::string& key) const;
    int64_t asInt(int64_t def = 0) const { return type == Type::Int ? i : def; }
    const std::string& asStr() const { return s; }
    int64_t intAt(const std::string& key, int64_t def = 0) const;
    std::string strAt(const std::string& key, const std::string& def = "") const;
};

/// Decode one value at `pos` (advanced past it). Returns false on malformed input.
bool decode(const uint8_t* data, size_t len, size_t& pos, Value& out);

/// Decode a whole buffer as a single top-level value.
bool decode(const std::string& buf, Value& out);

/// Canonical bencode encoding of a value (keys sorted via std::map).
std::string encode(const Value& v);

}  // namespace bencode
}  // namespace torrent
