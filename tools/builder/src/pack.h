#ifndef TB_PACK_H
#define TB_PACK_H

#include "pkg.h"
#include "util.h"

#define PACK_VERSION 2

typedef struct {
    char* name;
    Buf data;
    char* error;
} PackEntry;

typedef struct {
    PackEntry* items;
    int count;
    int cap;
} Pack;

PackEntry* pack_add(Pack* p, const char* name);
uint64_t pack_size(Pack* p);
void pack_stream(const void* pack, Emit emit, void* ctx);
void pack_build(Pack* p, Buf* out);
int pack_write(Pack* p, const char* path);
void pack_free(Pack* p);

#define PACK_SECTOR 2048
#define PACK_DISC_VERSION 3

uint32_t pack_hash(const char* name);
void pack_build_disc(Pack* p, Buf* out);

typedef struct {
    Buf part[8];
} Pbp;

int pbp_read(const uint8_t* data, size_t len, Pbp* out);
void pbp_write(const Pbp* p, Buf* out);
void pbp_free(Pbp* p);
void sfo_build(const char* title, Buf* out);

#define CTR_PAK_NAME "tcoaal.pak"
#define CTR_ICON_SIZE 0x1680
#define CTR_BANNER_NAME "banner.bin"
#define CTR_SOUND_NAME "banner.bcwav"
#define CTR_BANNER_W 256
#define CTR_BANNER_H 128
#define CTR_BANNER_SIZE (CTR_BANNER_W * CTR_BANNER_H * 2)

int ctr_check(const uint8_t* data, size_t len);
int ctr_build(const uint8_t* engine, size_t engine_len, const uint8_t* icon, const Buf* pak, Buf* out);
int ctr_unpack(const uint8_t* data, size_t len, Buf* icon, Buf* pak);
int ctr_parts(const uint8_t* data, size_t len, size_t* smdh, size_t* romfs);
int ctr_pak_version(const uint8_t* data, size_t len);
int ctr_pak_entry(const uint8_t* data, size_t len, const char* name, size_t* off, size_t* size);
void ctr_icon_encode(const uint8_t* rgba, int size, Buf* out);
void ctr_banner_encode(const uint8_t* rgba, Buf* out);

#endif
