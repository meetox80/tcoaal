#include "adpcm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

#define KERNEL_LOBES 8
#define KERNEL_STEPS 256
#define NO_LOOP 0xFFFFFFFFu
#define PI 3.14159265358979323846

static const int FILTERS[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

typedef struct {
    int16_t* pcm;
    size_t frames;
    int channels;
    int rate;
    long loop_start;
    long loop_length;
} Pcm;

typedef struct {
    int32_t s1, s2;
} History;

static long comment_value(stb_vorbis* v, const char* key) {
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    size_t kl = strlen(key);
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        size_t k = 0;
        while (k < kl && s[k] && (s[k] | 32) == (key[k] | 32)) k++;
        if (k == kl && s[kl] == '=') return atol(s + kl + 1);
    }
    return -1;
}

static int decode(const uint8_t* ogg, size_t len, Pcm* out) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg, (int)len, &err, NULL);
    if (!v) return 0;
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    int ch = vi.channels < 2 ? 1 : 2;
    size_t total = stb_vorbis_stream_length_in_samples(v);
    size_t cap = total ? total : 1;
    int16_t* pcm = xmalloc(cap * (size_t)ch * sizeof(int16_t));
    size_t got = 0;
    for (;;) {
        if (got == cap) {
            cap *= 2;
            pcm = xrealloc(pcm, cap * (size_t)ch * sizeof(int16_t));
        }
        int n = stb_vorbis_get_samples_short_interleaved(v, ch, pcm + got * (size_t)ch, (int)((cap - got) * (size_t)ch));
        if (n <= 0) break;
        got += (size_t)n;
    }
    out->pcm = pcm;
    out->frames = got;
    out->channels = ch;
    out->rate = (int)vi.sample_rate;
    out->loop_start = comment_value(v, "LOOPSTART");
    out->loop_length = comment_value(v, "LOOPLENGTH");
    stb_vorbis_close(v);
    return got > 0;
}

static void mixdown(Pcm* p) {
    if (p->channels == 1) return;
    for (size_t i = 0; i < p->frames; i++) p->pcm[i] = (int16_t)((p->pcm[i * 2] + p->pcm[i * 2 + 1]) / 2);
    p->channels = 1;
}

static double kernel_at(double x, double fc) {
    if (fabs(x) >= KERNEL_LOBES) return 0;
    double s = x == 0 ? 1.0 : sin(PI * x) / (PI * x);
    double w = 0.42 + 0.5 * cos(PI * x / KERNEL_LOBES) + 0.08 * cos(2 * PI * x / KERNEL_LOBES);
    return fc * s * w;
}

static int16_t* resample(const int16_t* in, size_t frames, int ch, int in_rate, int out_rate, size_t* out_frames) {
    size_t n = (size_t)((double)frames * out_rate / in_rate);
    int16_t* out = xmalloc((n ? n : 1) * (size_t)ch * sizeof(int16_t));
    *out_frames = n;
    if (in_rate == out_rate) {
        memcpy(out, in, frames * (size_t)ch * sizeof(int16_t));
        return out;
    }
    double ratio = (double)out_rate / in_rate, fc = (ratio < 1 ? ratio : 1.0) * 0.95;
    int radius = (int)ceil(KERNEL_LOBES / fc);
    int steps = radius * KERNEL_STEPS;
    float* table = xmalloc(sizeof(float) * (size_t)(steps + 2));
    for (int i = 0; i <= steps + 1; i++) table[i] = (float)kernel_at(fc * i / KERNEL_STEPS, fc);
    for (size_t i = 0; i < n; i++) {
        double pos = i / ratio;
        long center = (long)floor(pos);
        double acc[2] = {0, 0}, wsum = 0;
        for (long k = center - radius + 1; k <= center + radius; k++) {
            double d = fabs(pos - (double)k) * KERNEL_STEPS;
            int t = (int)d;
            if (t > steps) continue;
            double f = d - t, w = table[t] + (table[t + 1] - table[t]) * f;
            long s = k < 0 ? 0 : k >= (long)frames ? (long)frames - 1 : k;
            for (int c = 0; c < ch; c++) acc[c] += w * in[(size_t)s * (size_t)ch + (size_t)c];
            wsum += w;
        }
        for (int c = 0; c < ch; c++) {
            double v = wsum > 0 ? acc[c] / wsum : 0;
            out[i * (size_t)ch + (size_t)c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lround(v));
        }
    }
    free(table);
    return out;
}

static int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

static int32_t asr(int32_t v, int s) { return v >= 0 ? v >> s : ~(~v >> s); }

static uint64_t try_block(const int32_t* x, int f, int shift, History h, uint8_t* nib, History* out) {
    uint64_t err = 0;
    int e = 12 - shift;
    for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
        int32_t pred = (h.s1 * FILTERS[f][0] + h.s2 * FILTERS[f][1] + 32) >> 6;
        int32_t diff = x[i] - pred;
        int32_t q = e ? asr(diff + (1 << (e - 1)), e) : diff;
        if (q > 7) q = 7;
        if (q < -8) q = -8;
        int32_t dec = clamp16(asr(q * 4096, shift) + pred);
        int64_t d = x[i] - dec;
        err += (uint64_t)(d * d);
        nib[i] = (uint8_t)(q & 15);
        h.s2 = h.s1;
        h.s1 = dec;
    }
    *out = h;
    return err;
}

static void encode_block(const int32_t* x, History* h, int plain, uint8_t* out) {
    uint64_t best = UINT64_MAX;
    uint8_t nib[ADPCM_BLOCK_SAMPLES], best_nib[ADPCM_BLOCK_SAMPLES];
    History next, best_h = *h;
    int best_f = 0, best_shift = 12;
    for (int f = 0; f < (plain ? 1 : 5); f++) {
        int32_t p1 = h->s1, p2 = h->s2, peak = 0;
        for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
            int32_t r = x[i] - ((p1 * FILTERS[f][0] + p2 * FILTERS[f][1] + 32) >> 6);
            if (r < 0) r = -r;
            if (r > peak) peak = r;
            p2 = p1;
            p1 = x[i];
        }
        int e = 0;
        while (e < 12 && peak > (7 << e)) e++;
        for (int extra = 0; extra < 2 && e + extra <= 12; extra++) {
            int shift = 12 - e - extra;
            uint64_t err = try_block(x, f, shift, *h, nib, &next);
            if (err >= best) continue;
            best = err;
            best_f = f;
            best_shift = shift;
            best_h = next;
            memcpy(best_nib, nib, sizeof(nib));
        }
    }
    out[0] = (uint8_t)(best_shift | best_f << 4);
    out[1] = 0;
    for (int k = 0; k < 14; k++) out[2 + k] = (uint8_t)(best_nib[k * 2] | best_nib[k * 2 + 1] << 4);
    *h = best_h;
}

static void encode_channel(const int16_t* pcm, size_t frames, size_t stride, size_t blocks, size_t plain, uint8_t* out) {
    History h = {0, 0};
    int32_t x[ADPCM_BLOCK_SAMPLES];
    for (size_t b = 0; b < blocks; b++) {
        for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
            size_t at = b * ADPCM_BLOCK_SAMPLES + (size_t)i;
            x[i] = at < frames ? pcm[at * stride] : 0;
        }
        encode_block(x, &h, b == 0 || b == plain, out + b * ADPCM_BLOCK_BYTES);
    }
}

void adpcm_encode(const int16_t* pcm, size_t count, size_t stride, uint8_t* out) {
    size_t blocks = (count + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES;
    encode_channel(pcm, count, stride, blocks, 0, out);
}

static size_t round4(size_t v) { return (v + 3) & ~(size_t)3; }

int adpcm_stream_from_ogg(const uint8_t* ogg, size_t len, int rate, int max_channels, int loop, Buf* out) {
    Pcm p;
    if (!decode(ogg, len, &p)) return 0;
    if (max_channels < 2) mixdown(&p);
    size_t end = p.frames;
    long ls = 0;
    if (p.loop_start >= 0 && p.loop_length > 0 && (size_t)(p.loop_start + p.loop_length) <= p.frames) {
        ls = p.loop_start;
        end = (size_t)(p.loop_start + p.loop_length);
    }
    size_t frames;
    int16_t* pcm = resample(p.pcm, end, p.channels, p.rate, rate, &frames);
    free(p.pcm);
    size_t blocks = round4((frames + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES);
    if (!blocks) blocks = 4;
    uint32_t loop_block = NO_LOOP;
    if (loop) loop_block = (uint32_t)(((size_t)((double)ls * rate / p.rate) / ADPCM_BLOCK_SAMPLES) & ~(size_t)3);
    size_t page_blocks = STREAM_PAGE / ADPCM_BLOCK_BYTES, pages = (blocks + page_blocks - 1) / page_blocks;
    size_t chan_bytes = pages * STREAM_PAGE;
    uint8_t* enc = xcalloc(chan_bytes * (size_t)p.channels, 1);
    for (int c = 0; c < p.channels; c++) {
        encode_channel(pcm + c, frames, (size_t)p.channels, blocks, loop_block == NO_LOOP ? 0 : loop_block, enc + chan_bytes * (size_t)c);
    }
    free(pcm);
    size_t head = out->len;
    buf_zero(out, STREAM_PAGE);
    uint8_t* h = out->data + head;
    memcpy(h, "PSTR", 4);
    h[4] = (uint8_t)p.channels;
    h[6] = 1;
    for (int k = 0; k < 4; k++) {
        h[8 + k] = (uint8_t)((uint32_t)rate >> (8 * k));
        h[12 + k] = (uint8_t)((uint32_t)blocks >> (8 * k));
        h[16 + k] = (uint8_t)(loop_block >> (8 * k));
    }
    for (size_t pg = 0; pg < pages; pg++) {
        for (int c = 0; c < p.channels; c++) buf_put(out, enc + chan_bytes * (size_t)c + pg * STREAM_PAGE, STREAM_PAGE);
    }
    free(enc);
    return 1;
}

int adpcm_sample_from_ogg(const uint8_t* ogg, size_t len, int max_rate, Buf* out) {
    Pcm p;
    if (!decode(ogg, len, &p)) return 0;
    mixdown(&p);
    int rate = p.rate < max_rate ? p.rate : max_rate;
    size_t frames;
    int16_t* pcm = resample(p.pcm, p.frames, 1, p.rate, rate, &frames);
    free(p.pcm);
    size_t used = (frames + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES;
    if (!used) used = 1;
    size_t blocks = round4(used);
    uint8_t* enc = xcalloc(blocks * ADPCM_BLOCK_BYTES, 1);
    encode_channel(pcm, frames, 1, used, 0, enc);
    free(pcm);
    enc[1] |= 0x04;
    enc[(used - 1) * ADPCM_BLOCK_BYTES + 1] |= 0x01;
    buf_put(out, "PSFX", 4);
    buf_u32(out, (uint32_t)rate);
    buf_u32(out, (uint32_t)blocks);
    buf_u32(out, 0);
    buf_put(out, enc, blocks * ADPCM_BLOCK_BYTES);
    free(enc);
    return 1;
}
