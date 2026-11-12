#include "pad.h"

#include <psxapi.h>
#include <psxpad.h>

namespace pad {

namespace {

alignas(4) uint8_t ports[2][34];
uint16_t now, before;

}

void init() {
    InitPAD(ports[0], sizeof(ports[0]), ports[1], sizeof(ports[1]));
    InitCARD(1);
    StartPAD();
    StartCARD();
    ChangeClearPAD(0);
    _bu_init();
}

void poll() {
    before = now;
    auto* p = reinterpret_cast<const PADTYPE*>(ports[0]);
    bool usable = p->stat == 0 && (p->type == PAD_ID_DIGITAL || p->type == PAD_ID_ANALOG_STICK || p->type == PAD_ID_ANALOG);
    now = usable ? static_cast<uint16_t>(~p->btn) : 0;
}

uint16_t held() { return now; }

uint16_t pressed() { return now & ~before; }

}
