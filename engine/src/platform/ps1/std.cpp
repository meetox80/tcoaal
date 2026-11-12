#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace std {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI_HI = 1.57079632673412561417;
constexpr double HALF_PI_LO = 6.07710050650619224932e-11;
constexpr double LN2 = 0.69314718055994530942;

uint64_t bits_of(double x) {
    uint64_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

double from_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, sizeof(x));
    return x;
}

int exponent_of(double x) { return static_cast<int>(bits_of(x) >> 52 & 0x7FF) - 1023; }

double scale2(double x, int e) {
    if (e > 1023) e = 1023;
    if (e < -1022) return 0;
    return x * from_bits(static_cast<uint64_t>(e + 1023) << 52);
}

double nan_value() { return from_bits(0x7FF8000000000000ULL); }

double sin_poly(double r) {
    double r2 = r * r;
    return r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800 + r2 / 6227020800.0)))));
}

double cos_poly(double r) {
    double r2 = r * r;
    return 1 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800 + r2 / 479001600.0)))));
}

double sincos(double x, int shift) {
    if (!isfinite(x)) return nan_value();
    double k = round(x / (PI / 2));
    double r = (x - k * HALF_PI_HI) - k * HALF_PI_LO;
    int q = (static_cast<int>(fmod(k, 4.0)) + 4 + shift) & 3;
    switch (q) {
        case 0: return sin_poly(r);
        case 1: return cos_poly(r);
        case 2: return -sin_poly(r);
        default: return -cos_poly(r);
    }
}

string unsigned_text(unsigned long long v, bool negative) {
    char buf[24];
    char* p = buf + sizeof(buf);
    do {
        *--p = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    if (negative) *--p = '-';
    return string(p, static_cast<size_t>(buf + sizeof(buf) - p));
}

int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'z') return (c | 32) - 'a' + 10;
    return 99;
}

}

double trunc(double x) {
    uint64_t u = bits_of(x);
    int e = exponent_of(x);
    if (e >= 52) return x;
    if (e < 0) return from_bits(u & 0x8000000000000000ULL);
    return from_bits(u & ~(0x000FFFFFFFFFFFFFULL >> e));
}

double floor(double x) {
    double t = trunc(x);
    return x < 0 && t != x ? t - 1 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return x > 0 && t != x ? t + 1 : t;
}

double round(double x) { return x < 0 ? -floor(0.5 - x) : floor(x + 0.5); }

double fmod(double x, double y) {
    if (y == 0 || !isfinite(x)) return nan_value();
    return x - trunc(x / y) * y;
}

double sqrt(double x) {
    if (x <= 0 || !isfinite(x)) return x == 0 || x > 0 ? x : nan_value();
    double g = from_bits((bits_of(x) >> 1) + 0x1FF8000000000000ULL);
    for (int i = 0; i < 4; i++) g = 0.5 * (g + x / g);
    return g;
}

double exp(double x) {
    if (x != x) return x;
    if (x > 709) return from_bits(0x7FF0000000000000ULL);
    if (x < -745) return 0;
    double k = round(x / LN2);
    double r = x - k * LN2;
    double term = 1, sum = 1;
    for (int i = 1; i < 14; i++) {
        term *= r / i;
        sum += term;
    }
    return scale2(sum, static_cast<int>(k));
}

double log(double x) {
    if (x != x || x < 0) return nan_value();
    if (x == 0) return -from_bits(0x7FF0000000000000ULL);
    int e = exponent_of(x);
    double m = from_bits((bits_of(x) & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL);
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 30; i += 2) {
        sum += term / i;
        term *= s2;
    }
    return 2 * sum + e * LN2;
}

double pow(double x, double y) {
    if (y == 0) return 1;
    if (y == trunc(y) && fabs(y) <= 64) {
        unsigned n = static_cast<unsigned>(fabs(y));
        double r = 1, b = x;
        for (; n; n >>= 1) {
            if (n & 1) r *= b;
            b *= b;
        }
        return y < 0 ? 1 / r : r;
    }
    if (x == 0) return y > 0 ? 0 : from_bits(0x7FF0000000000000ULL);
    if (x < 0) return nan_value();
    return exp(y * log(x));
}

double sin(double x) { return sincos(x, 0); }

double cos(double x) { return sincos(x, 1); }

double atan(double x) {
    if (x != x) return x;
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv = x > 1;
    if (inv) x = 1 / x;
    bool shift = x > 0.41421356237309504880;
    if (shift) x = (x - 1) / (x + 1);
    double x2 = x * x, term = x, sum = 0;
    for (int i = 1; i < 40; i += 2) {
        sum += (i & 2 ? -term : term) / i;
        term *= x2;
    }
    if (shift) sum += PI / 4;
    if (inv) sum = PI / 2 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + PI : atan(y / x) - PI;
    if (y > 0) return PI / 2;
    if (y < 0) return -PI / 2;
    return 0;
}

double strtod(const char* s, char** end) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    bool neg = *p == '-';
    if (*p == '-' || *p == '+') p++;
    double v = 0;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; p++, any = true) v = v * 10 + (*p - '0');
    if (*p == '.') {
        double f = 0.1;
        for (p++; *p >= '0' && *p <= '9'; p++, any = true, f *= 0.1) v += (*p - '0') * f;
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool eneg = *q == '-';
        if (*q == '-' || *q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            int e = 0;
            for (; *q >= '0' && *q <= '9'; q++) e = e * 10 + (*q - '0');
            v *= pow(10.0, eneg ? -e : e);
            p = q;
        }
    }
    if (end) *end = const_cast<char*>(any ? p : s);
    return neg ? -v : v;
}

unsigned long strtoul(const char* s, char** end, int base) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    const char* start = p;
    for (int d; (d = digit_value(*p)) < base; p++) v = v * static_cast<unsigned long>(base) + static_cast<unsigned long>(d);
    if (end) *end = const_cast<char*>(p == start ? s : p);
    return v;
}

string to_string(long long v) { return unsigned_text(v < 0 ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v), v < 0); }

string to_string(unsigned long long v) { return unsigned_text(v, false); }

string to_string(double v) {
    if (v != v) return "nan";
    bool neg = v < 0;
    if (neg) v = -v;
    unsigned long long scaled = static_cast<unsigned long long>(v * 1000000.0 + 0.5);
    string out = unsigned_text(scaled / 1000000, neg);
    string frac = unsigned_text(scaled % 1000000 + 1000000, false);
    frac[0] = '.';
    return out + frac;
}

}
