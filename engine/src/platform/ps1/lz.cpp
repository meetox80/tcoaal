#include "lz.h"

#include <cstring>

namespace lz {

namespace {

constexpr size_t MIN_MATCH = 4;
constexpr size_t MAX_OFFSET = 65535;
constexpr int HASH_BITS = 12;

uint32_t read32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t hash(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

void put_length(std::vector<uint8_t>& out, size_t n) {
    for (; n >= 255; n -= 255) out.push_back(255);
    out.push_back((uint8_t)n);
}

void emit(std::vector<uint8_t>& out, const uint8_t* lit, size_t lits, size_t offset, size_t match) {
    size_t ml = match ? match - MIN_MATCH : 0;
    out.push_back((uint8_t)((lits < 15 ? lits : 15) << 4 | (ml < 15 ? ml : 15)));
    if (lits >= 15) put_length(out, lits - 15);
    out.insert(out.end(), lit, lit + lits);
    if (!match) return;
    out.push_back((uint8_t)offset);
    out.push_back((uint8_t)(offset >> 8));
    if (ml >= 15) put_length(out, ml - 15);
}

bool get_length(const uint8_t*& p, const uint8_t* end, size_t& n) {
    for (;;) {
        if (p >= end) return false;
        uint8_t b = *p++;
        n += b;
        if (b != 255) return true;
    }
}

}

std::vector<uint8_t> compress(const uint8_t* src, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n / 2 + 16);
    uint32_t table[1 << HASH_BITS];
    std::memset(table, 0xFF, sizeof(table));
    size_t anchor = 0, i = 0;
    while (i + MIN_MATCH <= n) {
        uint32_t h = hash(read32(src + i));
        size_t cand = table[h];
        table[h] = (uint32_t)i;
        if (cand == 0xFFFFFFFFu || i - cand > MAX_OFFSET || read32(src + cand) != read32(src + i)) {
            i++;
            continue;
        }
        size_t len = MIN_MATCH;
        while (i + len < n && src[cand + len] == src[i + len]) len++;
        emit(out, src + anchor, i - anchor, i - cand, len);
        i += len;
        anchor = i;
    }
    emit(out, src + anchor, n - anchor, 0, 0);
    return out;
}

bool decompress(const uint8_t* p, size_t n, uint8_t* dst, size_t out_n) {
    const uint8_t* end = p + n;
    size_t o = 0;
    while (p < end) {
        uint8_t token = *p++;
        size_t lits = token >> 4;
        if (lits == 15 && !get_length(p, end, lits)) return false;
        if ((size_t)(end - p) < lits || out_n - o < lits) return false;
        std::memcpy(dst + o, p, lits);
        p += lits;
        o += lits;
        if (p == end) break;
        if (end - p < 2) return false;
        size_t offset = (size_t)p[0] | (size_t)p[1] << 8;
        p += 2;
        size_t len = token & 15;
        if (len == 15 && !get_length(p, end, len)) return false;
        len += MIN_MATCH;
        if (!offset || offset > o || out_n - o < len) return false;
        for (size_t k = 0; k < len; k++, o++) dst[o] = dst[o - offset];
    }
    return o == out_n;
}

uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

}
