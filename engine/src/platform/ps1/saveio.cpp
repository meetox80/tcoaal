#include "../../game/saveio.h"

#include <cstring>
#include <unordered_map>

#include "../../core/fs.h"
#include "../platform.h"
#include "lz.h"

namespace saveio {

namespace {

enum : uint32_t { STR = 1, ARR = 2, OBJ = 3, NUM = 4, BIG = 5, BYTES = 6 };
enum : uint8_t { T_NULL, T_FALSE, T_TRUE, T_INT, T_NUM, T_STR, T_ARR, T_OBJ, T_BYTES };

void varint(std::vector<uint8_t>& out, uint32_t v) {
    for (; v >= 0x80; v >>= 7) out.push_back((uint8_t)(v | 0x80));
    out.push_back((uint8_t)v);
}

struct Compactor {
    std::vector<uint8_t> body;
    std::vector<const std::string*> strings;
    std::unordered_map<std::string, uint32_t> index;

    uint32_t intern(const std::string& s) {
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        uint32_t id = (uint32_t)strings.size();
        it = index.emplace(s, id).first;
        strings.push_back(&it->first);
        return id;
    }

    void value(const Node& n) {
        switch (n.t) {
            case Node::Null: body.push_back(T_NULL); break;
            case Node::Bool: body.push_back(n.b ? T_TRUE : T_FALSE); break;
            case Node::Int:
                body.push_back(T_INT);
                varint(body, ((uint32_t)n.i << 1) ^ (uint32_t)(n.i >> 31));
                break;
            case Node::Num: {
                body.push_back(T_NUM);
                uint8_t b[8];
                std::memcpy(b, &n.d, 8);
                body.insert(body.end(), b, b + 8);
                break;
            }
            case Node::Str:
                body.push_back(T_STR);
                varint(body, intern(n.s));
                break;
            case Node::Bytes:
                body.push_back(T_BYTES);
                varint(body, (uint32_t)n.s.size());
                body.insert(body.end(), n.s.begin(), n.s.end());
                break;
            case Node::Arr:
                body.push_back(T_ARR);
                varint(body, (uint32_t)n.arr.size());
                for (const Node& c : n.arr) value(c);
                break;
            case Node::Obj:
                body.push_back(T_OBJ);
                varint(body, (uint32_t)n.obj.size());
                for (const auto& kv : n.obj) {
                    varint(body, intern(kv.first));
                    value(kv.second);
                }
                break;
        }
    }
};

struct Expander {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    std::vector<const uint8_t*> text;
    std::vector<uint32_t> length;
    std::vector<int32_t> placed;
    std::vector<uint8_t> out;

    uint32_t varint() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            if (p >= end) break;
            uint8_t b = *p++;
            v |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }

    void put32(size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); }

    size_t reserve(size_t bytes) {
        size_t at = out.size();
        out.resize(at + ((bytes + 3) & ~(size_t)3), 0);
        return at;
    }

    size_t blob(uint32_t type, const uint8_t* data, size_t n, bool terminate) {
        size_t at = reserve(4 + n + terminate);
        put32(at, (uint32_t)(n << 3 | type));
        if (n) std::memcpy(&out[at + 4], data, n);
        return at;
    }

    static uint32_t rel(size_t node, size_t slot) { return (uint32_t)((int32_t)node - (int32_t)slot); }

    size_t string(uint32_t id) {
        if (id >= text.size()) {
            ok = false;
            id = 0;
        }
        if (placed[id] < 0) placed[id] = (int32_t)blob(STR, text[id], length[id], true);
        return (size_t)placed[id];
    }

    uint32_t value(size_t slot) {
        if (!ok || p >= end) {
            ok = false;
            return 0;
        }
        switch (*p++) {
            case T_NULL: return 0;
            case T_FALSE: return 2;
            case T_TRUE: return 6;
            case T_INT: {
                uint32_t z = varint();
                int32_t i = (int32_t)((z >> 1) ^ (~(z & 1) + 1));
                if (i >= -(1 << 30) && i < (1 << 30)) return (uint32_t)i << 1 | 1;
                size_t at = reserve(8);
                put32(at, BIG);
                put32(at + 4, (uint32_t)i);
                return rel(at, slot);
            }
            case T_NUM: {
                if (end - p < 8) {
                    ok = false;
                    return 0;
                }
                size_t at = reserve(12);
                put32(at, NUM);
                std::memcpy(&out[at + 4], p, 8);
                p += 8;
                return rel(at, slot);
            }
            case T_STR: return rel(string(varint()), slot);
            case T_BYTES: {
                uint32_t n = varint();
                if ((size_t)(end - p) < n) {
                    ok = false;
                    return 0;
                }
                size_t at = blob(BYTES, p, n, false);
                p += n;
                return rel(at, slot);
            }
            case T_ARR: {
                uint32_t n = varint();
                size_t at = reserve(4 + (size_t)n * 4);
                put32(at, n << 3 | ARR);
                for (uint32_t k = 0; k < n && ok; k++) {
                    size_t s = at + 4 + (size_t)k * 4;
                    uint32_t v = value(s);
                    put32(s, v);
                }
                return rel(at, slot);
            }
            case T_OBJ: {
                uint32_t n = varint();
                size_t at = reserve(4 + (size_t)n * 8);
                put32(at, n << 3 | OBJ);
                for (uint32_t k = 0; k < n && ok; k++) {
                    size_t s = at + 4 + (size_t)k * 8;
                    uint32_t key = rel(string(varint()), s);
                    put32(s, key);
                    uint32_t v = value(s + 4);
                    put32(s + 4, v);
                }
                return rel(at, slot);
            }
            default: ok = false; return 0;
        }
    }
};

bool expand(const uint8_t* data, size_t size, std::vector<uint8_t>& vbn) {
    if (size < 4 || std::memcmp(data, "VBC1", 4) != 0) return false;
    Expander e;
    e.p = data + 4;
    e.end = data + size;
    uint32_t count = e.varint();
    if (count > size) return false;
    e.text.reserve(count);
    e.length.reserve(count);
    for (uint32_t i = 0; i < count && e.ok; i++) {
        uint32_t n = e.varint();
        if ((size_t)(e.end - e.p) < n) return false;
        e.text.push_back(e.p);
        e.length.push_back(n);
        e.p += n;
    }
    e.placed.assign(count, -1);
    e.out.reserve(size * 3);
    e.reserve(12);
    std::memcpy(e.out.data(), "VBN2", 4);
    uint32_t root = e.value(8);
    if (!e.ok) return false;
    e.put32(8, root);
    e.put32(4, (uint32_t)e.out.size());
    vbn = std::move(e.out);
    return true;
}

}

std::vector<uint8_t> encode(const Node& root) {
    Compactor c;
    c.body.reserve(8192);
    c.value(root);
    std::vector<uint8_t> out = {'V', 'B', 'C', '1'};
    varint(out, (uint32_t)c.strings.size());
    for (const std::string* s : c.strings) {
        varint(out, (uint32_t)s->size());
        out.insert(out.end(), s->begin(), s->end());
    }
    out.insert(out.end(), c.body.begin(), c.body.end());
    return out;
}

static void put32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (k * 8));
}

static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

std::vector<uint8_t> pack(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> body = lz::compress(raw.data(), raw.size());
    std::vector<uint8_t> out(12 + body.size());
    std::memcpy(out.data(), "TSV2", 4);
    put32(&out[4], (uint32_t)raw.size());
    put32(&out[8], lz::checksum(raw.data(), raw.size()));
    std::memcpy(out.data() + 12, body.data(), body.size());
    return out;
}

bool unpack(const std::vector<uint8_t>& data, std::vector<uint8_t>& out) {
    if (data.size() >= 4 && std::memcmp(data.data(), "VBC1", 4) == 0) {
        out = data;
        return true;
    }
    if (data.size() < 12 || std::memcmp(data.data(), "TSV2", 4) != 0) return false;
    uint32_t size = get32(&data[4]);
    if (size > (1u << 20)) return false;
    out.resize(size);
    return lz::decompress(data.data() + 12, data.size() - 12, out.data(), size) && lz::checksum(out.data(), size) == get32(&data[8]);
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> packed = pack(raw);
    if (packed.empty()) return false;
    return fs::write_file(path, packed.data(), packed.size());
}

bool write_file(const std::string& path, const Node& root) { return write_bytes(path, encode(root)); }

bool parse_bytes(const std::vector<uint8_t>& raw, Document& doc) {
    std::vector<uint8_t> vbn;
    return expand(raw.data(), raw.size(), vbn) && doc.load_memory(std::move(vbn));
}

int read_file(const std::string& path, Document& doc) {
    std::vector<uint8_t> data;
    if (!fs::read_file(path, data)) return fs::exists(path) ? 2 : 1;
    if (data.empty()) return 2;
    std::vector<uint8_t> raw;
    if (!unpack(data, raw)) return 3;
    return parse_bytes(raw, doc) ? 0 : 4;
}

std::vector<std::string> list_dir(const std::string& dir) { return fs::list_dir(dir); }

double now_ms() { return plat::time_seconds() * 1000.0; }

}
