/*
    GMCA — Bencode decoder/encoder implementation (see torrent/bencode.hpp).
*/

#include "torrent/bencode.hpp"

namespace torrent {
namespace bencode {

const Value* Value::find(const std::string& key) const {
    if (type != Type::Dict) return nullptr;
    auto it = dict.find(key);
    return it == dict.end() ? nullptr : &it->second;
}

int64_t Value::intAt(const std::string& key, int64_t def) const {
    const Value* v = find(key);
    return (v && v->type == Type::Int) ? v->i : def;
}

std::string Value::strAt(const std::string& key, const std::string& def) const {
    const Value* v = find(key);
    return (v && v->type == Type::Str) ? v->s : def;
}

// ---- decode -----------------------------------------------------------------

static bool decodeInt(const uint8_t* data, size_t len, size_t& pos, int64_t& out) {
    // format: 'i' <digits> 'e'  (already positioned on the digit run)
    bool neg = false;
    if (pos < len && data[pos] == '-') {
        neg = true;
        ++pos;
    }
    if (pos >= len || data[pos] < '0' || data[pos] > '9') return false;
    int64_t v = 0;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
        v = v * 10 + (data[pos] - '0');
        ++pos;
    }
    out = neg ? -v : v;
    return true;
}

bool decode(const uint8_t* data, size_t len, size_t& pos, Value& out) {
    if (pos >= len) return false;
    size_t start = pos;
    uint8_t c = data[pos];

    if (c == 'i') {
        ++pos;
        out.type = Value::Type::Int;
        if (!decodeInt(data, len, pos, out.i)) return false;
        if (pos >= len || data[pos] != 'e') return false;
        ++pos;
    } else if (c >= '0' && c <= '9') {
        // byte string: <len> ':' <bytes>
        int64_t slen = 0;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            slen = slen * 10 + (data[pos] - '0');
            ++pos;
        }
        if (pos >= len || data[pos] != ':') return false;
        ++pos;
        if (slen < 0 || pos + (size_t)slen > len) return false;
        out.type = Value::Type::Str;
        out.s.assign(reinterpret_cast<const char*>(data + pos), (size_t)slen);
        pos += (size_t)slen;
    } else if (c == 'l') {
        ++pos;
        out.type = Value::Type::List;
        while (pos < len && data[pos] != 'e') {
            Value child;
            if (!decode(data, len, pos, child)) return false;
            out.list.push_back(std::move(child));
        }
        if (pos >= len) return false;  // missing 'e'
        ++pos;
    } else if (c == 'd') {
        ++pos;
        out.type = Value::Type::Dict;
        while (pos < len && data[pos] != 'e') {
            Value key;
            if (!decode(data, len, pos, key) || key.type != Value::Type::Str) return false;
            Value val;
            if (!decode(data, len, pos, val)) return false;
            out.dict.emplace(std::move(key.s), std::move(val));
        }
        if (pos >= len) return false;
        ++pos;
    } else {
        return false;
    }

    out.begin = start;
    out.end = pos;
    return true;
}

bool decode(const std::string& buf, Value& out) {
    size_t pos = 0;
    return decode(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), pos, out);
}

// ---- encode -----------------------------------------------------------------

std::string encode(const Value& v) {
    std::string o;
    switch (v.type) {
    case Value::Type::Int:
        o += 'i';
        o += std::to_string(v.i);
        o += 'e';
        break;
    case Value::Type::Str:
        o += std::to_string(v.s.size());
        o += ':';
        o += v.s;
        break;
    case Value::Type::List:
        o += 'l';
        for (const auto& e : v.list) o += encode(e);
        o += 'e';
        break;
    case Value::Type::Dict:
        o += 'd';
        for (const auto& kv : v.dict) {  // std::map => sorted => canonical
            o += std::to_string(kv.first.size());
            o += ':';
            o += kv.first;
            o += encode(kv.second);
        }
        o += 'e';
        break;
    }
    return o;
}

}  // namespace bencode
}  // namespace torrent
