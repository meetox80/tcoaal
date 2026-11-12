#include "fs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "../platform/platform.h"

namespace fs {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    size_t got = size ? std::fread(out.data(), 1, (size_t)size, f) : 0;
    std::fclose(f);
    return got == (size_t)size;
}

static bool write_direct(const std::string& path, const void* data, size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    return (std::fclose(f) == 0) && ok;
}

bool write_file(const std::string& path, const void* data, size_t size) {
    std::string tmp = path + ".tmp";
    if (!write_direct(tmp, data, size)) return false;
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
    bool ok = write_direct(path, data, size);
    std::remove(tmp.c_str());
    return ok;
}

bool exists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool remove_file(const std::string& path) { return std::remove(path.c_str()) == 0; }

std::string save_path(const std::string& rel) { return plat::save_root() + "/" + rel; }

struct Entry {
    uint32_t name;
    uint32_t offset;
    uint32_t size;
};

struct DataFile {
    FILE* f = nullptr;
    std::string path;
    int64_t base = 0;
    int64_t size = 0;
    int64_t pos = 0;
};

static std::string g_source;
static std::string g_dir;
static bool g_packed = false;
static int64_t g_base = 0;
static std::vector<Entry> g_entries;
static std::vector<char> g_names;

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool read_at(FILE* f, int64_t off, void* dst, size_t n) { return std::fseek(f, (long)off, SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n; }

static int64_t pbp_pack(FILE* f) {
    uint8_t h[40];
    if (!read_at(f, 0, h, sizeof(h))) return -1;
    return rd32(h + 36);
}

static int64_t x3dsx_pack(FILE* f) {
    uint8_t h[0x2C];
    if (!read_at(f, 0, h, sizeof(h)) || !rd32(h + 0x28)) return -1;
    int64_t romfs = rd32(h + 0x28);
    uint8_t r[0x28], e[0x10];
    if (!read_at(f, romfs, r, sizeof(r)) || !read_at(f, romfs + rd32(r + 0x1C), e, sizeof(e))) return -1;
    return romfs + rd32(r + 0x24) + rd32(e + 8);
}

static int64_t iso_pack(FILE* f) {
    uint8_t pvd[190];
    if (!read_at(f, 16 * 2048, pvd, sizeof(pvd)) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    uint32_t root = rd32(pvd + 158), len = std::min<uint32_t>(rd32(pvd + 166), 64 * 2048);
    std::vector<uint8_t> dir(len);
    if (!read_at(f, (int64_t)root * 2048, dir.data(), len)) return -1;
    static const char name[] = "TCOAAL.PAK;1";
    for (size_t pos = 0; pos + 33 < len;) {
        uint8_t rl = dir[pos];
        if (!rl) {
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (pos + rl > len) break;
        if (dir[pos + 32] == sizeof(name) - 1 && std::memcmp(&dir[pos + 33], name, sizeof(name) - 1) == 0) return (int64_t)rd32(&dir[pos + 2]) * 2048;
        pos += rl;
    }
    return -1;
}

static int64_t pack_base(FILE* f) {
    uint8_t h[4];
    if (!read_at(f, 0, h, 4)) return -1;
    if (std::memcmp(h, "TPAK", 4) == 0) return 0;
    int64_t base = std::memcmp(h, "\0PBP", 4) == 0 ? pbp_pack(f) : std::memcmp(h, "3DSX", 4) == 0 ? x3dsx_pack(f) : iso_pack(f);
    if (base < 0 || !read_at(f, base, h, 4) || std::memcmp(h, "TPAK", 4) != 0) return -1;
    return base;
}

bool has_pack(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = pack_base(f) >= 0;
    std::fclose(f);
    return ok;
}

int pack_version(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    int64_t base = pack_base(f);
    uint8_t h[8];
    int v = base >= 0 && read_at(f, base, h, sizeof(h)) ? (int)rd32(h + 4) : -1;
    std::fclose(f);
    return v;
}

static bool load_index(FILE* f, int64_t base) {
    uint8_t h[16];
    if (std::fseek(f, (long)base, SEEK_SET) != 0 || std::fread(h, 1, 16, f) != 16) return false;
    if (std::memcmp(h, "TPAK", 4) != 0 || rd32(h + 4) != PACK_VERSION) return false;
    uint32_t count = rd32(h + 8), names = rd32(h + 12);
    std::vector<uint8_t> raw((size_t)count * 12);
    if (count && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) return false;
    g_names.assign(names + 1, 0);
    if (names && std::fread(g_names.data(), 1, names, f) != names) return false;
    g_entries.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        g_entries[i] = {rd32(&raw[i * 12]), rd32(&raw[i * 12 + 4]), rd32(&raw[i * 12 + 8])};
        if (g_entries[i].name >= names) return false;
    }
    return true;
}

bool init_data(const std::string& source) {
    g_source = source;
    g_packed = false;
    g_entries.clear();
    g_names.clear();
    struct stat st;
    bool is_dir = stat(source.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    FILE* f = is_dir ? nullptr : std::fopen(source.c_str(), "rb");
    if (f) {
        int64_t base = pack_base(f);
        bool ok = base >= 0 && load_index(f, base);
        std::fclose(f);
        if (ok) {
            g_packed = true;
            g_base = base;
            plat::log("data: pack %s (%d files)", source.c_str(), (int)g_entries.size());
            return true;
        }
        g_entries.clear();
        size_t slash = source.find_last_of('/');
        g_dir = (slash == std::string::npos ? std::string(".") : source.substr(0, slash)) + "/data";
        plat::log("data: no pack in %s, using %s", source.c_str(), g_dir.c_str());
        return true;
    }
    g_dir = source;
    plat::log("data: folder %s", g_dir.c_str());
    return true;
}

bool data_packed() { return g_packed; }

static const Entry* find(const std::string& rel) {
    const char* key = rel.c_str();
    auto it = std::lower_bound(g_entries.begin(), g_entries.end(), key, [](const Entry& e, const char* k) {
        return std::strcmp(&g_names[e.name], k) < 0;
    });
    if (it == g_entries.end() || std::strcmp(&g_names[it->name], key) != 0) return nullptr;
    return &*it;
}

static FILE* open_retry(const std::string& path) {
    plat::io_begin();
    FILE* f = std::fopen(path.c_str(), "rb");
    for (int attempt = 0; !f && attempt < 10; attempt++) {
        plat::io_end();
        plat::sleep_ms(50);
        plat::io_begin();
        f = std::fopen(path.c_str(), "rb");
    }
    plat::io_end();
#if defined(__3DS__)
    if (f) std::setvbuf(f, nullptr, _IOFBF, 64 * 1024);
#endif
    return f;
}

static bool reopen(DataFile* d, int64_t pos) {
    if (d->f) std::fclose(d->f);
    d->f = open_retry(d->path);
    return d->f && std::fseek(d->f, (long)(d->base + pos), SEEK_SET) == 0;
}

DataFile* open_data(const std::string& rel) {
    if (g_packed) {
        const Entry* e = find(rel);
        if (!e) return nullptr;
        FILE* f = open_retry(g_source);
        if (!f) return nullptr;
        DataFile* d = new DataFile();
        d->f = f;
        d->path = g_source;
        d->base = g_base + e->offset;
        d->size = e->size;
        if (std::fseek(f, (long)d->base, SEEK_SET) != 0) {
            data_close(d);
            return nullptr;
        }
        return d;
    }
    std::string path = g_dir + "/" + rel;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    DataFile* d = new DataFile();
    d->f = f;
    d->path = path;
    std::fseek(f, 0, SEEK_END);
    d->size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (d->size < 0) {
        data_close(d);
        return nullptr;
    }
    return d;
}

size_t data_read(DataFile* d, void* dst, size_t size) {
    int64_t left = d->size - d->pos;
    if (left <= 0) return 0;
    if ((int64_t)size > left) size = (size_t)left;
    size_t got = 0;
    plat::io_begin();
    for (int attempt = 0; attempt < 4 && got < size; attempt++) {
        if ((attempt || !d->f) && !reopen(d, d->pos + (int64_t)got)) continue;
        got += std::fread((uint8_t*)dst + got, 1, size - got, d->f);
    }
    plat::io_end();
    d->pos += (int64_t)got;
    return got;
}

int data_seek(DataFile* d, int64_t off, int whence) {
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? d->pos + off : d->size + off;
    if (p < 0 || p > d->size) return -1;
    plat::io_begin();
    bool ok = d->f && std::fseek(d->f, (long)(d->base + p), SEEK_SET) == 0;
    plat::io_end();
    if (!ok && !reopen(d, p)) return -1;
    d->pos = p;
    return 0;
}

int64_t data_tell(DataFile* d) { return d->pos; }

void data_close(DataFile* d) {
    if (!d) return;
    if (d->f) std::fclose(d->f);
    delete d;
}

bool read_data(const std::string& rel, std::vector<uint8_t>& out) {
    DataFile* d = open_data(rel);
    if (!d) return false;
    out.resize((size_t)d->size);
    size_t got = d->size ? data_read(d, out.data(), out.size()) : 0;
    bool ok = got == out.size();
    data_close(d);
    return ok;
}

bool data_exists(const std::string& rel) {
    if (g_packed) return find(rel) != nullptr;
    DataFile* d = open_data(rel);
    if (!d) return false;
    data_close(d);
    return true;
}

}
