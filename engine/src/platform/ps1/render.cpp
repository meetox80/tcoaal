#include "../../core/image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace render {

namespace {

using fixed::q16;

constexpr q16 SNAP = fixed::ONE / 20;

void blit_fixed(const Image* img, q16 sx0, q16 sy0, q16 sw, q16 sh, q16 x0, q16 y0, q16 x1, q16 y1, uint32_t color, gfx::Blend blend) {
    using fixed::mul;
    using fixed::pixel;
    q16 tx0 = mul(sx0, img->qx), ty0 = mul(sy0, img->qy);
    q16 tx1 = mul(sx0 + sw, img->qx), ty1 = mul(sy0 + sh, img->qy);
    if (tx1 <= tx0 || ty1 <= ty0) return;
    q16 kx = x1 - x0 == tx1 - tx0 ? fixed::ONE : fixed::quotient(x1 - x0, tx1 - tx0);
    q16 ky = y1 - y0 == ty1 - ty0 ? fixed::ONE : fixed::quotient(y1 - y0, ty1 - ty0);
    for (const ImageChunk& c : img->chunks) {
        q16 left = c.x << fixed::FRAC, top = c.y << fixed::FRAC;
        q16 cx0 = std::max(tx0, left), cy0 = std::max(ty0, top);
        q16 cx1 = std::min(tx1, left + (c.w << fixed::FRAC)), cy1 = std::min(ty1, top + (c.h << fixed::FRAC));
        if (cx1 <= cx0 || cy1 <= cy0) continue;
        gfx::sprite_px(c.tex, pixel(cx0 - left), pixel(cy0 - top), pixel(cx1 - left), pixel(cy1 - top), pixel(x0 + mul(cx0 - tx0, kx)),
                       pixel(y0 + mul(cy0 - ty0, ky)), pixel(x0 + mul(cx1 - tx0, kx)), pixel(y0 + mul(cy1 - ty0, ky)), color, blend);
    }
}

}

void blit_screen(const Image* img, float srcx, float srcy, float srcw, float srch, float x0, float y0, float x1, float y1, uint32_t color,
                 gfx::Blend blend, bool) {
    if (!img || srcw <= 0 || srch <= 0) return;
    using fixed::from_float;
    blit_fixed(img, from_float(srcx), from_float(srcy), from_float(srcw), from_float(srch), from_float(x0), from_float(y0), from_float(x1),
               from_float(y1), color, blend);
}

void blit(const Image* img, float srcx, float srcy, float srcw, float srch, float dx, float dy, float scalex, float scaley, uint32_t color,
          gfx::Blend blend) {
    if (!img || srcw <= 0 || srch <= 0) return;
    using fixed::from_float;
    using fixed::mul;
    q16 sw = from_float(srcw), sh = from_float(srch), ox = from_float(dx), oy = from_float(dy);
    q16 x0 = qsx(ox), y0 = qsy(oy);
    q16 x1 = qsx(ox + mul(sw, from_float(scalex))), y1 = qsy(oy + mul(sh, from_float(scaley)));
    if (std::abs(std::abs(x1 - x0) - mul(sw, img->qx)) < SNAP && std::abs(std::abs(y1 - y0) - mul(sh, img->qy)) < SNAP) {
        q16 rx = fixed::snap(x0), ry = fixed::snap(y0);
        x1 += rx - x0;
        y1 += ry - y0;
        x0 = rx;
        y0 = ry;
    }
    blit_fixed(img, from_float(srcx), from_float(srcy), sw, sh, x0, y0, x1, y1, color, blend);
}

void blit_rotated(const Image* img, float srcx, float srcy, float srcw, float srch, float dx, float dy, float ox, float oy, float scalex,
                  float scaley, float angle, uint32_t color, gfx::Blend blend) {
    if (!img) return;
    if (std::fabs(angle) < 1e-4f) {
        blit(img, srcx, srcy, srcw, srch, dx - ox * scalex, dy - oy * scaley, scalex, scaley, color, blend);
        return;
    }
    float c = std::cos(angle), s = std::sin(angle);
    float tx0 = srcx * img->fx, ty0 = srcy * img->fy;
    float tx1 = (srcx + srcw) * img->fx, ty1 = (srcy + srch) * img->fy;
    for (const ImageChunk& ch : img->chunks) {
        float cx0 = std::max(tx0, (float)ch.x), cy0 = std::max(ty0, (float)ch.y);
        float cx1 = std::min(tx1, (float)(ch.x + ch.w)), cy1 = std::min(ty1, (float)(ch.y + ch.h));
        if (cx1 <= cx0 || cy1 <= cy0) continue;
        float lx[4] = {cx0, cx1, cx1, cx0}, ly[4] = {cy0, cy0, cy1, cy1};
        float xy[8], uv[8];
        for (int k = 0; k < 4; k++) {
            float ox_l = (lx[k] - tx0) / img->fx - ox, oy_l = (ly[k] - ty0) / img->fy - oy;
            float px = ox_l * scalex, py = oy_l * scaley;
            float rx = px * c - py * s, ry = px * s + py * c;
            xy[k * 2] = sx(dx + rx);
            xy[k * 2 + 1] = sy(dy + ry);
            uv[k * 2] = lx[k] - ch.x;
            uv[k * 2 + 1] = ly[k] - ch.y;
        }
        gfx::draw_quad(ch.tex, uv, xy, color, blend, true);
    }
}

void fill(float x, float y, float w, float h, uint32_t color, gfx::Blend blend) {
    gfx::fill(std::round(sx(x)), std::round(sy(y)), std::round(sx(x + w)), std::round(sy(y + h)), color, blend);
}

void fill_screen(uint32_t color, gfx::Blend blend) { gfx::fill(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, color, blend); }

void tone(int r, int g, int b, int gray) {
    if (gray > 0) gfx::desaturate(gray / 255.0f);
    int ar = std::max(0, r), ag = std::max(0, g), ab = std::max(0, b);
    int sr = std::max(0, -r), sg = std::max(0, -g), sb = std::max(0, -b);
    if (ar || ag || ab) gfx::fill(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(ar, ag, ab, 255), gfx::Blend::Add);
    if (sr || sg || sb) gfx::fill(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(sr, sg, sb, 255), gfx::Blend::Subtract);
}

uint32_t color(int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    return gfx::rgba(alpha, alpha, alpha, alpha);
}

uint32_t color_rgba(int r, int g, int b, int a) {
    a = std::clamp(a, 0, 255);
    return gfx::premul(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), a);
}

}
