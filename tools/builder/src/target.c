#include "target.h"

#include <string.h>

static const Target TARGETS[TARGET_COUNT] = {
    {TARGET_PSP, "psp", "PSP", 0.375, 512, TEX_PSP, 512, 3, 0, 0},
    {TARGET_VITA, "vita", "PS Vita", 0.75, 2048, TEX_LINEAR, 2048, 3, 0, 0},
    {TARGET_3DS, "3ds", "3DS", 0.3125, 512, TEX_PICA, 512, 3, 0, 1},
    {TARGET_WII, "wii", "Wii", 0.5, 1024, TEX_GX, 1024, 3, 0, 0},
    {TARGET_WIIU, "wiiu", "Wii U", 1.0, 4096, TEX_LINEAR, 2048, 3, 0, 0},
    {TARGET_PS3, "ps3", "PS3", 1.0, 4096, TEX_ARGB, 2048, 3, 0, 0},
    {TARGET_PS2, "ps2", "PS2", 0.375, 512, TEX_GS, 512, 3, 0, 0},
    {TARGET_XENON, "xbox360", "Xbox 360", 1.0, 4096, TEX_ARGB, 2048, 3, 0, 0},
    {TARGET_PS1, "ps1", "PS1", 0.5, 252, TEX_PSX, 252, 1, 252, 0},
};

const Target* g_target = &TARGETS[TARGET_PSP];

const Target* target_get(TargetId id) { return id >= 0 && id < TARGET_COUNT ? &TARGETS[id] : NULL; }

const Target* target_find(const char* name) {
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (!strcmp(TARGETS[i].name, name)) return &TARGETS[i];
    }
    return NULL;
}

void target_set(const Target* t) { g_target = t; }
