#ifndef TB_PKG_H
#define TB_PKG_H

#include "util.h"

#define PKG_TITLE_ID "TCOAAL001"
#define PKG_CONTENT_ID "UP0001-" PKG_TITLE_ID "_00-0000000000000000"

typedef void (*Emit)(void* ctx, const void* data, size_t len);
typedef void (*Source)(const void* src, Emit emit, void* ctx);

typedef struct {
    const char* name;
    int dir;
    uint64_t size;
    const Buf* data;
    Source source;
    const void* source_ctx;
} PkgItem;

typedef struct {
    char* name;
    int dir;
    Buf data;
} PkgFile;

void sha1(const void* data, size_t len, uint8_t out[20]);
void ps3_sfo_build(const char* title, const char* title_id, Buf* out);
int self_valid(const uint8_t* data, size_t len);
uint64_t pkg_write(const char* path, const char* content_id, const PkgItem* items, int count, int threads);
int pkg_read(const uint8_t* data, size_t len, PkgFile** files, int* count, int threads);
void pkg_files_free(PkgFile* files, int count);

#endif
