#include "card.h"

#include <psxapi.h>
#include <string.h>

#include <map>

#include "disc.h"
#include "timebase.h"

namespace card {

namespace {

constexpr size_t FRAME = 128;
constexpr size_t BLOCK = 8192;
constexpr size_t HEAD = 2 * FRAME;
constexpr int MODE_READ = 1;
constexpr int MODE_WRITE = 2;
constexpr int MODE_CREATE = 0x200;
const char PREFIX[] = "bu00:BASCUS-00000TCOAAL";

struct File {
    bool known = false;
    bool exists = false;
    std::vector<uint8_t> image;
};

std::map<char, File> g_files;
uint8_t g_icon[16 * 2 + FRAME];
bool g_icon_ok;

struct Quiet {
    Quiet() {
        disc::settle();
        timebase::hold();
    }
    ~Quiet() { timebase::release(); }
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;
};

std::string path_of(char slot) { return std::string(PREFIX) + slot; }

uint16_t full_width(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<uint16_t>(0x8260 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return static_cast<uint16_t>(0x8281 + (c - 'a'));
    if (c >= '0' && c <= '9') return static_cast<uint16_t>(0x824F + (c - '0'));
    return 0x8140;
}

void title_frame(uint8_t* f, const char* title, size_t blocks) {
    memset(f, 0, FRAME);
    f[0] = 'S';
    f[1] = 'C';
    f[2] = 0x11;
    f[3] = static_cast<uint8_t>(blocks);
    for (size_t i = 0; title[i] && i < 31; i++) {
        uint16_t c = full_width(title[i]);
        f[4 + i * 2] = static_cast<uint8_t>(c >> 8);
        f[5 + i * 2] = static_cast<uint8_t>(c);
    }
    if (g_icon_ok) memcpy(f + 0x60, g_icon, 32);
}

File& fetch(char slot) {
    File& f = g_files[slot];
    if (f.known) return f;
    f.known = true;
    Quiet quiet;
    int fd = open(path_of(slot).c_str(), MODE_READ);
    if (fd < 0) return f;
    uint8_t head[FRAME];
    if (read(fd, head, FRAME) == static_cast<int>(FRAME) && head[0] == 'S' && head[1] == 'C' && head[3]) {
        f.image.resize(head[3] * BLOCK);
        memcpy(f.image.data(), head, FRAME);
        f.exists = read(fd, f.image.data() + FRAME, f.image.size() - FRAME) == static_cast<int>(f.image.size() - FRAME);
    }
    close(fd);
    if (!f.exists) f.image.clear();
    return f;
}

bool write_frames(char slot, const std::vector<uint8_t>& image, const std::vector<uint8_t>& old) {
    int fd = open(path_of(slot).c_str(), MODE_WRITE);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t at = 0; at < image.size() && ok; at += FRAME) {
        if (old.size() == image.size() && !memcmp(image.data() + at, old.data() + at, FRAME)) continue;
        size_t end = at + FRAME;
        while (end < image.size() && (old.size() != image.size() || memcmp(image.data() + end, old.data() + end, FRAME))) end += FRAME;
        ok = lseek(fd, static_cast<uint32_t>(at), 0) >= 0 && write(fd, image.data() + at, end - at) == static_cast<int>(end - at);
        at = end - FRAME;
    }
    close(fd);
    return ok;
}

}

void init() {
    const disc::Entry* e = disc::find("card/icon.bin");
    alignas(4) static uint8_t sector[disc::SECTOR];
    g_icon_ok = e && e->size >= sizeof(g_icon) && disc::read(*e, sector);
    if (g_icon_ok) memcpy(g_icon, sector, sizeof(g_icon));
}

bool present(char slot) { return fetch(slot).exists; }

bool load(char slot, std::vector<uint8_t>& payload) {
    File& f = fetch(slot);
    if (!f.exists || f.image.size() < HEAD + 4) return false;
    uint32_t n;
    memcpy(&n, f.image.data() + HEAD, 4);
    if (n > f.image.size() - HEAD - 4) return false;
    payload.assign(f.image.data() + HEAD + 4, f.image.data() + HEAD + 4 + n);
    return true;
}

bool store(char slot, const char* title, const std::vector<uint8_t>& payload) {
    Quiet quiet;
    File& f = fetch(slot);
    size_t blocks = (HEAD + 4 + payload.size() + BLOCK - 1) / BLOCK;
    if (blocks > 15) return false;
    std::vector<uint8_t> image(blocks * BLOCK, 0);
    title_frame(image.data(), title, blocks);
    if (g_icon_ok) memcpy(image.data() + FRAME, g_icon + 32, FRAME);
    uint32_t n = static_cast<uint32_t>(payload.size());
    memcpy(image.data() + HEAD, &n, 4);
    memcpy(image.data() + HEAD + 4, payload.data(), payload.size());
    if (f.exists && f.image.size() != image.size()) {
        ::erase(path_of(slot).c_str());
        f.exists = false;
        f.image.clear();
    }
    if (!f.exists) {
        int fd = open(path_of(slot).c_str(), MODE_CREATE | static_cast<int>(blocks << 16));
        if (fd < 0) return false;
        close(fd);
    }
    if (!write_frames(slot, image, f.image)) {
        f.known = false;
        return false;
    }
    f.exists = true;
    f.image = std::move(image);
    return true;
}

bool erase(char slot) {
    File& f = fetch(slot);
    if (!f.exists) return true;
    Quiet quiet;
    bool ok = ::erase(path_of(slot).c_str()) != 0;
    f.exists = false;
    f.image.clear();
    return ok;
}

}
