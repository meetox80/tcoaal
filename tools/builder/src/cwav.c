#include "cwav.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define CWAV_HEADER 0x40
#define TB_PI 3.14159265358979323846
#define SILENCE 300
#define SKIP_SECONDS 8.0
#define FADE_IN 0.01
#define FADE_OUT 0.5
#define PEAK 26000.0
#define MAX_GAIN 4.0

static uint32_t align32(uint32_t v) { return (v + 0x1F) & ~0x1Fu; }

static void ref(Buf* b, uint16_t type, uint32_t offset) {
    buf_u16(b, type);
    buf_u16(b, 0);
    buf_u32(b, offset);
}

void cwav_build(const int16_t* pcm, size_t frames, int channels, int rate, Buf* out) {
    uint32_t chan_bytes = (uint32_t)frames * 2;
    uint32_t info = align32(0x20 + (uint32_t)channels * (8 + 20));
    uint32_t data = 0x20 + (uint32_t)channels * chan_bytes;
    size_t start = out->len;
    buf_put(out, "CWAV", 4);
    buf_u16(out, 0xFEFF);
    buf_u16(out, CWAV_HEADER);
    buf_u32(out, 0x02010000);
    buf_u32(out, CWAV_HEADER + info + data);
    buf_u16(out, 2);
    buf_u16(out, 0);
    ref(out, 0x7000, CWAV_HEADER);
    buf_u32(out, info);
    ref(out, 0x7001, CWAV_HEADER + info);
    buf_u32(out, data);
    buf_zero(out, start + CWAV_HEADER - out->len);
    buf_put(out, "INFO", 4);
    buf_u32(out, info);
    buf_u8(out, 1);
    buf_u8(out, 0);
    buf_u16(out, 0);
    buf_u32(out, (uint32_t)rate);
    buf_u32(out, 0);
    buf_u32(out, (uint32_t)frames);
    buf_u32(out, 0);
    buf_u32(out, (uint32_t)channels);
    for (int c = 0; c < channels; c++) ref(out, 0x7100, 4 + (uint32_t)channels * 8 + (uint32_t)c * 20);
    for (int c = 0; c < channels; c++) {
        ref(out, 0x1F00, 0x18 + (uint32_t)c * chan_bytes);
        ref(out, 0, 0xFFFFFFFFu);
        buf_u32(out, 0);
    }
    buf_zero(out, start + CWAV_HEADER + info - out->len);
    buf_put(out, "DATA", 4);
    buf_u32(out, data);
    buf_zero(out, 0x18);
    for (int c = 0; c < channels; c++) {
        for (size_t i = 0; i < frames; i++) buf_u16(out, (uint16_t)pcm[i * (size_t)channels + (size_t)c]);
    }
}

int cwav_from_ogg(const uint8_t* ogg, size_t len, double seconds, Buf* out) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg, (int)len, &err, NULL);
    if (!v) return 0;
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    int ch = vi.channels < 2 ? 1 : 2;
    int rate = (int)vi.sample_rate;
    size_t want = (size_t)((SKIP_SECONDS + seconds) * rate);
    int16_t* pcm = xmalloc(want * (size_t)ch * sizeof(int16_t));
    size_t got = 0;
    while (got < want) {
        int n = stb_vorbis_get_samples_short_interleaved(v, ch, pcm + got * (size_t)ch, (int)((want - got) * (size_t)ch));
        if (n <= 0) break;
        got += (size_t)n;
    }
    stb_vorbis_close(v);
    size_t first = 0;
    while (first < got * (size_t)ch && abs(pcm[first]) < SILENCE) first++;
    first /= (size_t)ch;
    if (first >= got) {
        free(pcm);
        return 0;
    }
    size_t lead = (size_t)(0.02 * rate);
    first = first > lead ? first - lead : 0;
    size_t frames = (size_t)(seconds * rate);
    if (frames > got - first) frames = got - first;
    int16_t* clip = pcm + first * (size_t)ch;
    int peak = 1;
    for (size_t i = 0; i < frames * (size_t)ch; i++) {
        if (abs(clip[i]) > peak) peak = abs(clip[i]);
    }
    double gain = PEAK / peak < MAX_GAIN ? PEAK / peak : MAX_GAIN;
    size_t fin = (size_t)(FADE_IN * rate), fout = (size_t)(FADE_OUT * rate);
    for (size_t i = 0; i < frames; i++) {
        double g = gain;
        if (i < fin) g *= (double)i / (double)fin;
        if (frames - i < fout) g *= 0.5 - 0.5 * cos(TB_PI * (double)(frames - i) / (double)fout);
        for (int c = 0; c < ch; c++) {
            int16_t* s = &clip[i * (size_t)ch + (size_t)c];
            *s = (int16_t)lrint(*s * g);
        }
    }
    cwav_build(clip, frames, ch, rate, out);
    free(pcm);
    return 1;
}
