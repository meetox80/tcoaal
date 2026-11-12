#ifndef TB_ISO_H
#define TB_ISO_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define ISO_SECTOR 2048
#define PS2_BOOT_NAME "TCAL_030.12"
#define PS2_PAK_NAME "TCOAAL.PAK"

typedef struct {
    const char* name;
    const uint8_t* data;
    size_t len;
} IsoFile;

void iso_build(const char* volume, const IsoFile* files, int count, Buf* out);
int iso_find(const uint8_t* iso, size_t len, const char* name, size_t* off, size_t* size);
int elf_check(const uint8_t* data, size_t len);
void ps2_system_cnf(Buf* out);
int ps2_iso_build(const uint8_t* elf, size_t elf_len, const uint8_t* pak, size_t pak_len, Buf* out);

#endif
