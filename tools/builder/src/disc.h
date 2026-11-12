#ifndef TB_DISC_H
#define TB_DISC_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define DISC_DATA 2048
#define DISC_RAW 2352
#define DISC_EXE "TCOAAL.EXE"
#define DISC_PAK "TCOAAL.PAK"
#define DISC_EXE_SECTORS 1024
#define DISC_HEAD_SECTORS 32

typedef struct {
    const char* name;
    const uint8_t* data;
    size_t len;
    uint32_t reserve;
} DiscFile;

uint32_t disc_edc(const uint8_t* p, size_t n);
void disc_seal(uint8_t* raw, uint32_t lba, uint8_t submode);
void disc_build(const char* volume, const DiscFile* files, int count, Buf* out);
int disc_find(const uint8_t* bin, size_t len, const char* name, uint32_t* lba, uint32_t* size, uint32_t* room);
int disc_replace(uint8_t* bin, size_t len, const char* name, const uint8_t* data, size_t size);
void disc_cue(const char* bin_name, Buf* out);
int exe_check(const uint8_t* data, size_t len);
int ps1_disc_build(const uint8_t* exe, size_t exe_len, const uint8_t* pak, size_t pak_len, Buf* out);

#endif
