#include "snd0.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

#define RATE 48000
#define LOOP_START 768000
#define LOOP_END 2304000
#define BLEND 12000
#define HOLD 8192
#define TAIL 8192
#define FADE_IN 2400
#define CUTOFF 0.485
#define ZEROS 32
#define KAISER_BETA 9.0

static const double PI = 3.14159265358979323846;

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64 && term > sum * 1e-15; k++) {
        double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
    }
    return sum;
}

static uint32_t gcd(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static float* decode(const uint8_t* ogg, size_t len, uint32_t* rate, size_t* frames) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg, (int)len, &err, NULL);
    if (!v) return NULL;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = info.channels;
    if (ch < 1 || !info.sample_rate) {
        stb_vorbis_close(v);
        return NULL;
    }
    size_t want = (size_t)((uint64_t)LOOP_END * info.sample_rate / RATE) + 1024;
    float* pcm = xmalloc(want * 2 * sizeof(float));
    float* chunk = xmalloc(4096 * (size_t)ch * sizeof(float));
    size_t have = 0;
    while (have < want) {
        int got = stb_vorbis_get_samples_float_interleaved(v, ch, chunk, 4096 * ch);
        if (got <= 0) break;
        for (int i = 0; i < got && have < want; i++, have++) {
            pcm[have * 2] = chunk[i * ch];
            pcm[have * 2 + 1] = chunk[i * ch + (ch > 1)];
        }
    }
    free(chunk);
    stb_vorbis_close(v);
    *rate = info.sample_rate;
    *frames = have;
    return pcm;
}

static float* resample(const float* in, size_t frames, uint32_t from, size_t count) {
    uint32_t g = gcd(RATE, from);
    uint32_t up = RATE / g, down = from / g;
    double fc = CUTOFF * (from > RATE ? (double)RATE / from : 1.0);
    int half = (int)ceil(ZEROS / (2.0 * fc));
    int taps = half * 2;
    float* kernel = xmalloc((size_t)up * (size_t)taps * sizeof(float));
    double norm = bessel_i0(KAISER_BETA);
    for (uint32_t p = 0; p < up; p++) {
        float* k = kernel + (size_t)p * (size_t)taps;
        double sum = 0;
        for (int j = 0; j < taps; j++) {
            double t = (double)p / up + half - 1 - j;
            double u = t / half;
            double w = fabs(u) < 1 ? bessel_i0(KAISER_BETA * sqrt(1 - u * u)) / norm : 0;
            double a = 2 * fc * t;
            double s = fabs(a) < 1e-12 ? 1 : sin(PI * a) / (PI * a);
            k[j] = (float)(2 * fc * s * w);
            sum += k[j];
        }
        for (int j = 0; j < taps; j++) k[j] = (float)(k[j] / sum);
    }
    float* out = xmalloc(count * 2 * sizeof(float));
    for (size_t m = 0; m < count; m++) {
        uint64_t pos = (uint64_t)m * down;
        int64_t base = (int64_t)(pos / up) - half + 1;
        const float* k = kernel + (size_t)(pos % up) * (size_t)taps;
        float l = 0, r = 0;
        for (int j = 0; j < taps; j++) {
            int64_t at = base + j;
            if (at < 0 || at >= (int64_t)frames) continue;
            l += in[at * 2] * k[j];
            r += in[at * 2 + 1] * k[j];
        }
        out[m * 2] = l;
        out[m * 2 + 1] = r;
    }
    free(kernel);
    return out;
}

static float* loop_wave(const float* song, size_t* count, int32_t* start, int32_t* end) {
    size_t trim = (LOOP_START + ATRAC3_SKIP) % ATRAC3_FRAME;
    size_t span = LOOP_END - LOOP_START;
    size_t n = LOOP_END - trim + TAIL;
    float* wave = xmalloc(n * 2 * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        size_t at = i + trim;
        double w = 0;
        if (at + HOLD >= LOOP_END) w = 1;
        else if (at + HOLD + BLEND > LOOP_END) w = (double)(at + HOLD + BLEND - LOOP_END) / BLEND;
        double level = i < FADE_IN ? (double)i / FADE_IN : 1.0;
        for (int c = 0; c < 2; c++) {
            double now = at < LOOP_END ? song[at * 2 + c] : 0;
            double again = w > 0 ? song[(at - span) * 2 + c] : 0;
            wave[i * 2 + c] = (float)((now * (1 - w) + again * w) * level);
        }
    }
    *count = n;
    *start = (int32_t)(LOOP_START - trim);
    *end = (int32_t)(LOOP_END - trim);
    return wave;
}

int snd0_from_ogg(const uint8_t* ogg, size_t len, Buf* out) {
    uint32_t from = 0;
    size_t frames = 0;
    float* pcm = decode(ogg, len, &from, &frames);
    if (!pcm || (uint64_t)frames * RATE / from < LOOP_END) {
        free(pcm);
        return 0;
    }
    float* song = resample(pcm, frames, from, LOOP_END);
    free(pcm);
    size_t count = 0;
    int32_t start = 0, end = 0;
    float* wave = loop_wave(song, &count, &start, &end);
    free(song);
    int ok = atrac3_encode(wave, count, RATE, start, end, out);
    free(wave);
    return ok;
}
