#include "core/audio.h"
#include "core/image.h"
#include "game/data.h"
#include "game/scenes.h"
#include "platform/gfx.h"
#include "platform/platform.h"

#include <algorithm>

int main(int argc, char** argv) {
    if (!plat::init(argc, argv)) return 1;
    audio::init();
    images::set_budget(profile::IMAGE_BUDGET);
    if (!data::load()) {
        plat::log("data missing");
        plat::shutdown();
        return 1;
    }
    size_t budget = profile::IMAGE_BUDGET;
    if (profile::TEX_LAYOUT == TexLayout::PspSwizzle && plat::heap_limit() && plat::heap_limit() < (32u << 20)) images::set_compact(true);
    if (size_t limit = plat::heap_limit()) {
        size_t used = plat::heap_used();
        size_t room = limit > used ? limit - used : 0;
        budget = std::min(budget, room / 2);
        images::set_budget(budget);
        gfx::reserve(budget + budget / 4);
    }
    plat::log("heap %uKB of %uKB, images %uKB", (unsigned)(plat::heap_used() >> 10), (unsigned)(plat::heap_limit() >> 10), (unsigned)(budget >> 10));
#ifdef PSN00BSDK
    const Value& ses = data::root()["ses"];
    for (size_t i = 0; i < ses.size(); i++) audio::preload_se(ses[i].str(), true);
#endif
    scenes::run(scenes::make_boot());
    if (plat::options().dump_audio > 0) audio::write_dump(plat::save_root() + "/audio_dump.raw");
    plat::log("exit after %llu frames, heap %uKB", (unsigned long long)plat::frame_count(), (unsigned)(plat::heap_used() >> 10));
    images::shutdown();
    plat::shutdown();
    return 0;
}
