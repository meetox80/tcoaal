#include "timebase.h"

#include <hwregs_c.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>

namespace timebase {

namespace {

constexpr uint32_t RUN = (1 << 5) | (1 << 6) | (2 << 8);
constexpr uint32_t FREE = 2 << 8;
constexpr uint32_t WRAP = 1 << 16;
constexpr uint32_t PENDING = 1 << IRQ_TIMER2;
constexpr uint32_t CALIBRATION_FIELDS = 64;

volatile uint32_t g_wraps;
uint64_t g_base;
uint32_t g_origin_field;
uint32_t g_held_field;
uint32_t g_nominal_field;
uint64_t g_field_ticks_num = 1;
uint32_t g_field_ticks_den = 1;
int g_depth;

void on_wrap() { g_wraps++; }

uint32_t counted() {
    uint32_t lo = TIMER_VALUE(2) & 0xFFFF, hi = g_wraps;
    if ((IRQ_STAT & PENDING) && lo < WRAP / 2) hi++;
    return hi << 16 | lo;
}

void restart(uint32_t mode) {
    TIMER_CTRL(2) = mode;
    IRQ_STAT = static_cast<uint16_t>(~PENDING);
    g_wraps = 0;
}

}

void init() {
    g_nominal_field = PER_SECOND / (GetVideoMode() == MODE_PAL ? 50 : 60);
    g_field_ticks_num = g_nominal_field;
    VSync(0);
    EnterCriticalSection();
    InterruptCallback(IRQ_TIMER2, on_wrap);
    restart(RUN);
    g_base = 0;
    g_origin_field = static_cast<uint32_t>(VSync(-1));
    ExitCriticalSection();
}

uint64_t now() {
    FastEnterCriticalSection();
    uint64_t t = g_base + counted();
    FastExitCriticalSection();
    return t;
}

void hold() {
    if (g_depth++) return;
    VSync(0);
    FastEnterCriticalSection();
    g_base += counted();
    restart(FREE);
    g_held_field = static_cast<uint32_t>(VSync(-1));
    uint32_t span = g_held_field - g_origin_field;
    if (span >= CALIBRATION_FIELDS) {
        g_field_ticks_num = g_base;
        g_field_ticks_den = span;
    }
    FastExitCriticalSection();
}

void release() {
    if (--g_depth) return;
    VSync(0);
    FastEnterCriticalSection();
    uint32_t lo = TIMER_VALUE(2) & 0xFFFF;
    uint64_t fields = static_cast<uint32_t>(VSync(-1)) - g_held_field;
    uint64_t guess = fields * g_field_ticks_num / g_field_ticks_den + WRAP / 2;
    uint64_t wraps = guess > lo ? (guess - lo) >> 16 : 0;
    g_base += (wraps << 16) + lo;
    restart(RUN);
    FastExitCriticalSection();
}

}
