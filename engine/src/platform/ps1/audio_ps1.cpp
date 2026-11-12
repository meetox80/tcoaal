#include "../../core/audio.h"

#include <hwregs_c.h>
#include <psxspu.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/sha256.h"
#include "../platform.h"
#include "disc.h"
#include "timebase.h"

namespace audio {

namespace {

constexpr uint32_t BLOCK = 16;
constexpr uint32_t SAMPLES = 28;
constexpr uint32_t PAGE_BLOCKS = 128;
constexpr uint32_t NO_LOOP = 0xFFFFFFFFu;
constexpr uint32_t READ_PAGES = 4;
constexpr uint32_t SAFETY = 16;
constexpr uint32_t SPU_RAM = 0x80000;
constexpr uint32_t BGM_RING = 24 * PAGE_BLOCKS;
constexpr uint32_t BGS_RING = 16 * PAGE_BLOCKS;
constexpr uint32_t RING_BASE = 0x1100;
constexpr uint32_t SE_BASE = RING_BASE + (BGM_RING + BGS_RING) * BLOCK * 2;
constexpr uint32_t TICKS_PER_SAMPLE_Q = timebase::PER_SECOND / 44100 * 4096;
constexpr int FIRST_SE_VOICE = 4;
constexpr int VOICES = 24;
constexpr int MAX_VOL = 0x3FFF;
constexpr int GAIN_ONE = 1 << 16;
enum Kind { K_BGM, K_BGS, K_ME, K_SE };

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

bool is_hashed(const std::string& n) {
    if (n.size() != 16) return false;
    for (char c : n) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

const std::string& entry_name(const char* folder, const std::string& name, const char* ext) {
    static std::unordered_map<std::string, std::string> names;
    static std::string key;
    key.assign(folder).append(1, '/').append(name);
    auto it = names.find(key);
    if (it == names.end()) {
        std::string base = is_hashed(name) ? std::string("audio/") + folder + "/" + name : hash_path(std::string("audio/") + folder + "/" + name + ".ogg");
        it = names.emplace(key, base + ext).first;
    }
    return it->second;
}

void upload(uint32_t addr, const void* data, uint32_t bytes) {
    SpuSetTransferStartAddr(addr);
    SpuWrite(static_cast<const uint32_t*>(data), bytes);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
}

uint16_t pitch_of(uint32_t rate, int percent) {
    uint32_t p = rate * 4096 / 44100 * static_cast<uint32_t>(percent < 10 ? 10 : percent) / 100;
    return static_cast<uint16_t>(p > 0x3FFF ? 0x3FFF : p);
}

int clamp_volume(int v) { return v < 0 ? 0 : v > MAX_VOL ? MAX_VOL : v; }
int pan_left(int level, int pan) { return pan > 0 ? level * (100 - pan) / 100 : level; }
int pan_right(int level, int pan) { return pan < 0 ? level * (100 + pan) / 100 : level; }

void set_volume(int voice, int left, int right) {
    SPU_CH_VOL_L(voice) = static_cast<uint16_t>(clamp_volume(left));
    SPU_CH_VOL_R(voice) = static_cast<uint16_t>(clamp_volume(right));
}

void key_on(int voice, uint32_t addr, uint32_t loop, uint16_t pitch) {
    SPU_CH_ADDR(voice) = getSPUAddr(addr);
    SPU_CH_LOOP_ADDR(voice) = getSPUAddr(loop);
    SPU_CH_FREQ(voice) = pitch;
    SPU_CH_ADSR1(voice) = 0x00FF;
    SPU_CH_ADSR2(voice) = 0x0000;
    SpuSetKey(1, 1u << voice);
}

int g_kind_volume[4] = {100, 100, 100, 100};

struct Stream {
    Stream(int first_voice, uint32_t capacity, uint32_t left, uint32_t right) : voice(first_voice), ring_blocks(capacity), ring{left, right} {}

    int voice;
    uint32_t ring_blocks;
    uint32_t ring[2];
    const disc::Entry* entry = nullptr;
    Track track;
    Kind kind = K_BGM;
    int channels = 0;
    uint32_t rate = 0, blocks = 0, loop_block = NO_LOOP;
    uint32_t start_block = 0, next_block = 0, written = 0, silent = 0;
    uint64_t consumed_q = 0, last_ticks = 0;
    uint16_t pitch = 0;
    bool active = false, ended = false, reading = false;
    uint32_t read_first = 0, read_count = 0, read_offset = 0;
    int32_t gain = GAIN_ONE, gain_target = GAIN_ONE, gain_step = 0;
    bool stop_after_fade = false;
    uint8_t* staging = nullptr;

    uint32_t played() const { return static_cast<uint32_t>(consumed_q / TICKS_PER_SAMPLE_Q / SAMPLES); }
    uint32_t free_blocks() const {
        uint32_t used = written - played();
        return used + SAFETY >= ring_blocks ? 0 : ring_blocks - used - SAFETY;
    }
};

Stream g_music(0, BGM_RING, RING_BASE, RING_BASE + BGM_RING * BLOCK);
Stream g_ambient(2, BGS_RING, RING_BASE + BGM_RING * BLOCK * 2, RING_BASE + BGM_RING * BLOCK * 2 + BGS_RING * BLOCK);

Stream* g_reader;

void refresh_volume(Stream& s) {
    int level = s.active ? static_cast<int>(static_cast<int64_t>(MAX_VOL) * s.track.volume / 100 * g_kind_volume[s.kind] / 100 * s.gain >> 16) : 0;
    int l = pan_left(level, s.track.pan), r = pan_right(level, s.track.pan);
    if (s.channels == 2) {
        set_volume(s.voice, l, 0);
        set_volume(s.voice + 1, 0, r);
    } else {
        set_volume(s.voice, l, r);
    }
}

void halt(Stream& s) {
    if (s.reading) {
        while (disc::poll() == 1) {
        }
        s.reading = false;
        g_reader = nullptr;
    }
    if (s.active) {
        SpuSetKey(0, 3u << s.voice);
        for (int c = 0; c < 2; c++) set_volume(s.voice + c, 0, 0);
    }
    s.active = false;
    s.entry = nullptr;
    s.ended = true;
}

void store(Stream& s, uint8_t* data, uint32_t page_stride, uint32_t count) {
    for (int c = 0; c < s.channels; c++) {
        uint32_t done = 0;
        while (done < count) {
            uint32_t src = s.read_offset + done, page = src / PAGE_BLOCKS, in_page = src % PAGE_BLOCKS;
            uint32_t at = (s.written + done) % s.ring_blocks;
            uint32_t n = PAGE_BLOCKS - in_page;
            if (n > count - done) n = count - done;
            if (n > s.ring_blocks - at) n = s.ring_blocks - at;
            uint8_t* p = data + page * page_stride + static_cast<uint32_t>(c) * disc::SECTOR + in_page * BLOCK;
            if (at == 0) p[1] = 0x04;
            if (at + n == s.ring_blocks) p[(n - 1) * BLOCK + 1] = 0x03;
            upload(s.ring[c] + at * BLOCK, p, n * BLOCK);
            done += n;
        }
    }
    s.written += count;
}

void advance(Stream& s, uint32_t count) {
    s.next_block += count;
    if (s.next_block < s.blocks) return;
    if (s.loop_block != NO_LOOP) s.next_block = s.loop_block;
    else s.ended = true;
}

uint32_t plan_read(Stream& s, uint32_t limit) {
    uint32_t offset = s.next_block % PAGE_BLOCKS;
    uint32_t count = READ_PAGES * PAGE_BLOCKS - offset;
    if (count > s.blocks - s.next_block) count = s.blocks - s.next_block;
    if (count > limit) count = limit & ~3u;
    s.read_first = s.next_block / PAGE_BLOCKS;
    s.read_offset = offset;
    s.read_count = count;
    return (offset + count + PAGE_BLOCKS - 1) / PAGE_BLOCKS;
}

uint32_t page_stride(const Stream& s) { return disc::SECTOR * static_cast<uint32_t>(s.channels); }

void pad_silence(Stream& s) {
    static uint8_t zero[PAGE_BLOCKS * BLOCK];
    uint32_t n = s.free_blocks() & ~3u;
    if (n > PAGE_BLOCKS) n = PAGE_BLOCKS;
    if (!n) return;
    for (int c = 0; c < s.channels; c++) {
        uint32_t at = s.written % s.ring_blocks, first = n < s.ring_blocks - at ? n : s.ring_blocks - at;
        upload(s.ring[c] + at * BLOCK, zero, first * BLOCK);
        if (first < n) upload(s.ring[c], zero, (n - first) * BLOCK);
    }
    s.written += n;
    s.silent += n;
}

bool fill_sync(Stream& s, uint32_t limit) {
    uint32_t pages = plan_read(s, limit);
    if (!s.read_count) return false;
    if (!disc::read_part(*s.entry, 1 + s.read_first * s.channels, pages * s.channels, s.staging)) return false;
    store(s, s.staging, page_stride(s), s.read_count);
    advance(s, s.read_count);
    return true;
}

void service(Stream& s) {
    if (!s.active) return;
    uint64_t now = timebase::now();
    s.consumed_q += (now - s.last_ticks) * s.pitch;
    s.last_ticks = now;
    if (s.reading) {
        int r = disc::poll();
        if (r == 1) return;
        s.reading = false;
        g_reader = nullptr;
        if (r == 0) {
            store(s, s.staging, page_stride(s), s.read_count);
            advance(s, s.read_count);
        }
    }
    if (s.ended) {
        if (s.played() >= s.written - s.silent && s.kind == K_ME) halt(s);
        else pad_silence(s);
        return;
    }
    uint32_t room = s.free_blocks();
    if (g_reader || room < READ_PAGES * PAGE_BLOCKS / 2) return;
    uint32_t pages = plan_read(s, room);
    if (!s.read_count) return;
    s.reading = disc::start_read(*s.entry, 1 + s.read_first * s.channels, pages * s.channels, s.staging);
    if (s.reading) g_reader = &s;
}

bool start(Stream& s, Kind kind, const char* folder, const Track& t, double pos) {
    halt(s);
    s.kind = kind;
    s.track = t;
    s.gain = s.gain_target = GAIN_ONE;
    s.gain_step = 0;
    s.stop_after_fade = false;
    const disc::Entry* e = disc::find(entry_name(folder, t.name, ".str").c_str());
    alignas(4) static uint8_t head[disc::SECTOR];
    if (!e || !disc::read_part(*e, 0, 1, head) || memcmp(head, "PSTR", 4) != 0) {
        plat::log("missing audio %s/%s", folder, t.name.c_str());
        return false;
    }
    s.entry = e;
    s.channels = head[4];
    s.rate = rd32(head + 8);
    s.blocks = rd32(head + 12);
    s.loop_block = rd32(head + 16);
    if (!s.staging) s.staging = static_cast<uint8_t*>(malloc(READ_PAGES * 2 * disc::SECTOR));
    uint32_t block = static_cast<uint32_t>(pos * s.rate / SAMPLES) & ~3u;
    if (block >= s.blocks) block = s.loop_block != NO_LOOP ? s.loop_block + (block - s.loop_block) % (s.blocks - s.loop_block) : 0;
    s.start_block = s.next_block = block & ~3u;
    s.written = s.silent = 0;
    s.consumed_q = 0;
    s.ended = false;
    s.pitch = pitch_of(s.rate, t.pitch);
    for (int k = 0; k < 2 && !s.ended && fill_sync(s, s.free_blocks()); k++) {
    }
    s.active = true;
    refresh_volume(s);
    for (int c = 0; c < s.channels; c++) key_on(s.voice + c, s.ring[c], s.ring[c], s.pitch);
    s.last_ticks = timebase::now();
    return true;
}

double position(const Stream& s) {
    if (!s.active || !s.rate) return 0;
    uint64_t samples = static_cast<uint64_t>(s.start_block) * SAMPLES + s.consumed_q / TICKS_PER_SAMPLE_Q;
    uint64_t total = static_cast<uint64_t>(s.blocks) * SAMPLES;
    if (samples >= total && s.loop_block != NO_LOOP) {
        uint64_t loop = static_cast<uint64_t>(s.loop_block) * SAMPLES;
        samples = loop + (samples - loop) % (total - loop);
    }
    return static_cast<double>(samples) / s.rate;
}

void fade(Stream& s, double seconds, int32_t target, bool stop) {
    int frames = static_cast<int>(seconds * 60);
    s.gain_target = target;
    s.stop_after_fade = stop;
    if (frames <= 0) {
        s.gain = target;
        s.gain_step = 0;
        if (stop) halt(s);
        return;
    }
    s.gain_step = (target - s.gain) / frames;
    if (!s.gain_step) s.gain_step = target > s.gain ? 1 : -1;
}

void step_fade(Stream& s) {
    if (!s.active || !s.gain_step) return;
    s.gain += s.gain_step;
    if ((s.gain_step > 0 && s.gain >= s.gain_target) || (s.gain_step < 0 && s.gain <= s.gain_target)) {
        s.gain = s.gain_target;
        s.gain_step = 0;
        if (s.stop_after_fade) {
            halt(s);
            return;
        }
    }
    refresh_volume(s);
}

struct Sample {
    uint32_t addr = 0, bytes = 0, rate = 0;
    uint32_t last = 0;
    uint64_t busy_until = 0;
    bool pinned = false;
};

struct Span {
    uint32_t addr, bytes;
};

std::unordered_map<std::string, Sample> g_samples;
std::vector<Span> g_free;
uint32_t g_clock;
int g_next_voice = FIRST_SE_VOICE;
uint64_t g_voice_until[VOICES];

void release_span(uint32_t addr, uint32_t bytes) {
    size_t i = 0;
    while (i < g_free.size() && g_free[i].addr < addr) i++;
    g_free.insert(g_free.begin() + i, Span{addr, bytes});
    if (i + 1 < g_free.size() && g_free[i].addr + g_free[i].bytes == g_free[i + 1].addr) {
        g_free[i].bytes += g_free[i + 1].bytes;
        g_free.erase(g_free.begin() + i + 1);
    }
    if (i > 0 && g_free[i - 1].addr + g_free[i - 1].bytes == g_free[i].addr) {
        g_free[i - 1].bytes += g_free[i].bytes;
        g_free.erase(g_free.begin() + i);
    }
}

bool take_span(uint32_t bytes, uint32_t& addr) {
    for (size_t i = 0; i < g_free.size(); i++) {
        if (g_free[i].bytes < bytes) continue;
        addr = g_free[i].addr;
        g_free[i].addr += bytes;
        g_free[i].bytes -= bytes;
        if (!g_free[i].bytes) g_free.erase(g_free.begin() + i);
        return true;
    }
    return false;
}

bool evict_sample(uint64_t now) {
    auto victim = g_samples.end();
    for (auto it = g_samples.begin(); it != g_samples.end(); ++it) {
        const Sample& s = it->second;
        if (s.pinned || !s.bytes || s.busy_until > now) continue;
        if (victim == g_samples.end() || s.last < victim->second.last) victim = it;
    }
    if (victim == g_samples.end()) return false;
    release_span(victim->second.addr, victim->second.bytes);
    g_samples.erase(victim);
    return true;
}

Sample* sample(const std::string& name, bool pinned) {
    auto it = g_samples.find(name);
    if (it != g_samples.end()) {
        it->second.last = ++g_clock;
        it->second.pinned |= pinned;
        return it->second.bytes ? &it->second : nullptr;
    }
    Sample& s = g_samples[name];
    s.last = ++g_clock;
    s.pinned = pinned;
    const disc::Entry* e = disc::find(entry_name("se", name, ".vag").c_str());
    if (!e || e->size <= 16) {
        plat::log("missing audio se/%s", name.c_str());
        return nullptr;
    }
    auto* raw = static_cast<uint8_t*>(malloc(disc::sectors(e->size) * disc::SECTOR));
    if (!raw) return nullptr;
    uint32_t bytes = e->size - 16, addr = 0;
    uint64_t now = timebase::now();
    bool ok = disc::read(*e, raw) && memcmp(raw, "PSFX", 4) == 0;
    while (ok && !take_span(bytes, addr)) ok = evict_sample(now);
    if (ok) {
        upload(addr, raw + 16, bytes);
        s.addr = addr;
        s.bytes = bytes;
        s.rate = rd32(raw + 4);
    }
    free(raw);
    return ok ? &s : nullptr;
}

int se_voice(uint64_t now) {
    for (int k = 0; k < VOICES - FIRST_SE_VOICE; k++) {
        int v = FIRST_SE_VOICE + (g_next_voice - FIRST_SE_VOICE + k) % (VOICES - FIRST_SE_VOICE);
        if (g_voice_until[v] <= now) return v;
    }
    int oldest = FIRST_SE_VOICE;
    for (int v = FIRST_SE_VOICE; v < VOICES; v++) {
        if (g_voice_until[v] < g_voice_until[oldest]) oldest = v;
    }
    return oldest;
}

Track g_bgm_track;
bool g_bgm_valid = false, g_bgm_held = false, g_me_valid = false;
double g_bgm_pos = 0;

void start_bgm(double pos) {
    start(g_music, K_BGM, "bgm", g_bgm_track, pos);
}

void end_me(bool resume) {
    if (g_me_valid) halt(g_music);
    if (g_me_valid && resume && g_bgm_held && g_bgm_valid) {
        g_bgm_held = false;
        start_bgm(g_bgm_pos);
        g_music.gain = 0;
        refresh_volume(g_music);
        fade(g_music, 0.5, GAIN_ONE, false);
    }
    g_me_valid = false;
}

}

void init() {
    SpuInit();
    SPU_MASTER_VOL_L = MAX_VOL;
    SPU_MASTER_VOL_R = MAX_VOL;
    g_free.clear();
    g_free.push_back(Span{SE_BASE, SPU_RAM - SE_BASE});
}

void shutdown() {
    halt(g_music);
    halt(g_ambient);
}

void update() {
    service(g_music);
    service(g_ambient);
    step_fade(g_music);
    step_fade(g_ambient);
    if (g_me_valid && !g_music.active) end_me(true);
}

void set_volumes(int bgm, int bgs, int me, int se) {
    g_kind_volume[K_BGM] = bgm;
    g_kind_volume[K_BGS] = bgs;
    g_kind_volume[K_ME] = me;
    g_kind_volume[K_SE] = se;
    refresh_volume(g_music);
    refresh_volume(g_ambient);
}

void play_bgm(const Track& t, double pos) {
    if (g_bgm_valid && g_bgm_track.name == t.name && !t.name.empty() && (g_music.active || g_bgm_held)) {
        g_bgm_track = t;
        if (g_music.active && !g_me_valid) {
            g_music.track = t;
            g_music.pitch = pitch_of(g_music.rate, t.pitch);
            for (int c = 0; c < g_music.channels; c++) SPU_CH_FREQ(g_music.voice + c) = g_music.pitch;
            refresh_volume(g_music);
        }
    } else {
        stop_bgm();
        g_bgm_track = t;
        if (!t.name.empty()) {
            if (g_me_valid) {
                g_bgm_held = true;
                g_bgm_pos = pos;
            } else {
                start_bgm(pos);
            }
        }
    }
    g_bgm_track = t;
    g_bgm_valid = !t.name.empty();
}

void stop_bgm() {
    if (!g_me_valid) halt(g_music);
    g_bgm_held = false;
    g_bgm_valid = false;
    g_bgm_track = Track();
}

void fadeout_bgm(double seconds) {
    if (!g_bgm_valid) return;
    if (g_music.active && !g_me_valid) fade(g_music, seconds, 0, true);
    g_bgm_held = false;
    g_bgm_valid = false;
    g_bgm_track = Track();
}

void fadein_bgm(double seconds) {
    if (!g_music.active || g_me_valid) return;
    g_music.gain = 0;
    refresh_volume(g_music);
    fade(g_music, seconds, GAIN_ONE, false);
}

bool bgm_playing() { return g_music.active && !g_me_valid; }

Track current_bgm() { return g_bgm_valid ? g_bgm_track : Track(); }

Track save_bgm() {
    if (!g_bgm_valid) {
        Track t;
        t.volume = 0;
        return t;
    }
    Track t = g_bgm_track;
    t.pos = g_bgm_held || g_me_valid ? g_bgm_pos : position(g_music);
    return t;
}

void replay_bgm(const Track& t) {
    if (g_bgm_valid && g_bgm_track.name == t.name) {
        play_bgm(t, g_bgm_track.pos);
        return;
    }
    play_bgm(t, t.pos);
    fadein_bgm(0.5);
}

void play_bgs(const Track& t, double pos) {
    if (g_ambient.active && g_ambient.track.name == t.name && !t.name.empty()) {
        g_ambient.track = t;
        g_ambient.pitch = pitch_of(g_ambient.rate, t.pitch);
        for (int c = 0; c < g_ambient.channels; c++) SPU_CH_FREQ(g_ambient.voice + c) = g_ambient.pitch;
        refresh_volume(g_ambient);
        return;
    }
    halt(g_ambient);
    if (!t.name.empty()) start(g_ambient, K_BGS, "bgs", t, pos);
}

void stop_bgs() { halt(g_ambient); }

void fadeout_bgs(double seconds) {
    if (g_ambient.active) fade(g_ambient, seconds, 0, true);
}

Track current_bgs() { return g_ambient.active && !g_ambient.stop_after_fade ? g_ambient.track : Track(); }

void play_me(const Track& t) {
    end_me(t.name.empty());
    if (t.name.empty()) return;
    if (g_bgm_valid && g_music.active) {
        g_bgm_pos = position(g_music);
        g_bgm_held = true;
    }
    halt(g_music);
    g_me_valid = start(g_music, K_ME, "me", t, 0);
}

void stop_me() { end_me(true); }

bool me_playing() { return g_me_valid && g_music.active; }

void play_se(const Track& t) {
    if (t.name.empty()) return;
    Sample* s = sample(t.name, false);
    if (!s) return;
    uint64_t now = timebase::now();
    int v = se_voice(now);
    g_next_voice = v + 1;
    uint16_t pitch = pitch_of(s->rate, t.pitch);
    uint64_t length = static_cast<uint64_t>(s->bytes / BLOCK) * SAMPLES * TICKS_PER_SAMPLE_Q / (pitch ? pitch : 1);
    g_voice_until[v] = now + length;
    if (s->busy_until < now + length) s->busy_until = now + length;
    SpuSetKey(0, 1u << v);
    int level = static_cast<int>(static_cast<int64_t>(MAX_VOL) * t.volume / 100 * g_kind_volume[K_SE] / 100);
    set_volume(v, pan_left(level, t.pan), pan_right(level, t.pan));
    key_on(v, s->addr, s->addr, pitch);
}

void stop_se() {
    SpuSetKey(0, 0xFFFFFFu & ~0xFu);
    for (int v = FIRST_SE_VOICE; v < VOICES; v++) g_voice_until[v] = 0;
}

void preload_se(const std::string& name, bool pinned) {
    if (!name.empty()) sample(name, pinned);
}

void play_voice(const std::string&) {}
void stop_voice() {}
bool voice_playing() { return false; }
void set_voice_volume(int) {}

void stop_all() {
    stop_bgm();
    stop_bgs();
    stop_me();
    stop_se();
}

void on_resume() {}

double take_mix_stats(double& max_us) {
    max_us = 0;
    return 0;
}

void write_dump(const std::string&) {}

}
