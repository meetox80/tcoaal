#include "vpk.h"

#include <stdlib.h>
#include <string.h>

#include "../third_party/miniz.h"

static const char TEMPLATE[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                               "<livearea style=\"a1\" format-ver=\"01.00\" content-rev=\"1\">\n"
                               "  <livearea-background>\n"
                               "    <image>bg.png</image>\n"
                               "  </livearea-background>\n"
                               "  <gate>\n"
                               "    <startup-image>startup.png</startup-image>\n"
                               "  </gate>\n"
                               "</livearea>\n";

static int finish(mz_zip_archive* zip, Buf* out) {
    void* data = NULL;
    size_t size = 0;
    int ok = mz_zip_writer_finalize_heap_archive(zip, &data, &size);
    mz_zip_writer_end(zip);
    if (!ok) return 0;
    buf_put(out, data, size);
    mz_free(data);
    return 1;
}

static int add(mz_zip_archive* zip, const char* name, const void* data, size_t len) {
    return mz_zip_writer_add_mem(zip, name, data, len, MZ_BEST_COMPRESSION);
}

int vpk_build(const Vpk* v, Buf* out) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20)) return 0;
    int ok = add(&zip, "eboot.bin", v->eboot, v->eboot_len) && add(&zip, "sce_sys/param.sfo", v->sfo, v->sfo_len);
    if (ok && v->icon0.len) ok = add(&zip, "sce_sys/icon0.png", v->icon0.data, v->icon0.len);
    if (ok && v->bg.len && v->startup.len) {
        ok = add(&zip, "sce_sys/livearea/contents/bg.png", v->bg.data, v->bg.len) &&
             add(&zip, "sce_sys/livearea/contents/startup.png", v->startup.data, v->startup.len) &&
             add(&zip, "sce_sys/livearea/contents/template.xml", TEMPLATE, sizeof(TEMPLATE) - 1);
    }
    if (!ok) {
        mz_zip_writer_end(&zip);
        return 0;
    }
    return finish(&zip, out);
}

int vpk_is_vpk(const uint8_t* data, size_t len) {
    mz_zip_archive in;
    memset(&in, 0, sizeof(in));
    if (len < 4 || memcmp(data, "PK\3\4", 4) != 0 || !mz_zip_reader_init_mem(&in, data, len, 0)) return 0;
    int ok = mz_zip_reader_locate_file(&in, "eboot.bin", NULL, 0) >= 0 && mz_zip_reader_locate_file(&in, "sce_sys/param.sfo", NULL, 0) >= 0;
    mz_zip_reader_end(&in);
    return ok;
}

int vpk_replace_eboot(const uint8_t* vpk, size_t len, const uint8_t* eboot, size_t eboot_len, Buf* out) {
    mz_zip_archive in, zip;
    memset(&in, 0, sizeof(in));
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&in, vpk, len, 0)) return 0;
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20)) {
        mz_zip_reader_end(&in);
        return 0;
    }
    int ok = 1, found = 0;
    mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n && ok; i++) {
        char name[512];
        mz_zip_reader_get_filename(&in, i, name, sizeof(name));
        if (!strcmp(name, "eboot.bin")) {
            found = 1;
            continue;
        }
        ok = mz_zip_writer_add_from_zip_reader(&zip, &in, i);
    }
    ok = ok && found && add(&zip, "eboot.bin", eboot, eboot_len);
    mz_zip_reader_end(&in);
    if (!ok) {
        mz_zip_writer_end(&zip);
        return 0;
    }
    return finish(&zip, out);
}
