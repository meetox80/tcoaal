#pragma once
#include <cstdint>
#include <cstring>

namespace fixed {

using q16 = int32_t;

constexpr int FRAC = 16;
constexpr q16 ONE = 1 << FRAC;

constexpr q16 from_const(float f) { return static_cast<q16>(f * ONE); }

inline q16 from_float(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    int shift = static_cast<int>(b >> 23 & 255) - 150 + FRAC;
    uint32_t m = (b & 0x7FFFFF) | 0x800000;
    if (!(b & 0x7FFFFFFF) || shift < -24) return 0;
    q16 v = shift >= 0 ? static_cast<q16>(shift > 7 ? 0x7FFFFFFF : m << shift) : static_cast<q16>(m >> -shift);
    return b >> 31 ? -v : v;
}

inline q16 mul(q16 a, q16 b) { return static_cast<q16>(static_cast<int64_t>(a) * b >> FRAC); }

inline q16 quotient(q16 a, q16 b) { return static_cast<q16>((static_cast<int64_t>(a) << FRAC) / b); }

inline int pixel(q16 v) { return (v + ONE / 2) >> FRAC; }

inline q16 snap(q16 v) { return (v + ONE / 2) & ~(ONE - 1); }

}
