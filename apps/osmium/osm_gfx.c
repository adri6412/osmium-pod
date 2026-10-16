/***************************************************************************
 *   Osmium Sound for FiiO M3K — drawing
 *
 *   A small software renderer on the RGB565 frame: anti-aliased rounded
 *   boxes, circles and needles, gradients, soft shadows, 4-bit alpha icons
 *   and glyphs (DejaVu, from assets/osm_fonts.c), pictures with an alpha
 *   plane, bilinear scaling for covers. Everything clips to one rectangle.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <string.h>
#include "config.h"
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "rbunicode.h"
#include "fixedpoint.h"
#include "osm_gfx.h"

fb_data osm_fb[SCR_H * SCR_W] CACHEALIGN_ATTR;

static int clip_x0, clip_y0, clip_x1, clip_y1;     /* [x0, x1) */
static int fallback_font = FONT_SYSFIXED;

#define PIX(x, y) osm_fb[(y) * SCR_W + (x)]

/* ── colour ────────────────────────────────────────────────────────── */

static inline fb_data pack(unsigned r, unsigned g, unsigned b)
{
    return FB_RGBPACK(r, g, b);
}

/* blend (r,g,b) at alpha a (0..256) onto the pixel */
static inline void blend_px(fb_data *d, unsigned r, unsigned g, unsigned b, unsigned a)
{
    if (a >= 256)
    {
        *d = pack(r, g, b);
        return;
    }
    if (a == 0)
        return;
    unsigned p = *d;
    unsigned dr = (p >> 8) & 0xF8, dg = (p >> 3) & 0xFC, db = (p << 3) & 0xF8;
    dr |= dr >> 5; dg |= dg >> 6; db |= db >> 5;
    dr += ((int)(r - dr) * (int)a) >> 8;
    dg += ((int)(g - dg) * (int)a) >> 8;
    db += ((int)(b - db) * (int)a) >> 8;
    *d = pack(dr, dg, db);
}

static inline void blend_c(fb_data *d, color_t c, unsigned cov /* 0..256 */)
{
    unsigned a = ((c >> 24) * cov + 128) >> 8;      /* 0..255 */
    a += a >> 7;                                     /* -> 0..256 */
    blend_px(d, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, a);
}

/* blend one RGB565 pixel over another */
static inline void blend_565(fb_data *d, fb_data s, unsigned a /* 0..256 */)
{
    if (a >= 256) { *d = s; return; }
    if (!a) return;
    unsigned r = (s >> 8) & 0xF8, g = (s >> 3) & 0xFC, b = (s << 3) & 0xF8;
    blend_px(d, r | r >> 5, g | g >> 6, b | b >> 5, a);
}

/* darken a pixel to keep/256 of its value */
static inline fb_data darken(fb_data p, unsigned keep)
{
    unsigned r = ((p >> 11) * keep) >> 8;
    unsigned g = (((p >> 5) & 0x3F) * keep) >> 8;
    unsigned b = ((p & 0x1F) * keep) >> 8;
    return (fb_data)((r << 11) | (g << 5) | b);
}

/* ── frame ─────────────────────────────────────────────────────────── */

void gfx_init(void)
{
    gfx_noclip();
    memset(osm_fb, 0, sizeof(osm_fb));
}

void gfx_present(void)
{
    lcd_bitmap(osm_fb, 0, 0, SCR_W, SCR_H);
    lcd_update();
}

void gfx_noclip(void)
{
    clip_x0 = clip_y0 = 0;
    clip_x1 = SCR_W;
    clip_y1 = SCR_H;
}

void gfx_clip(int x, int y, int w, int h)
{
    clip_x0 = MAX(0, x);
    clip_y0 = MAX(0, y);
    clip_x1 = MIN(SCR_W, x + w);
    clip_y1 = MIN(SCR_H, y + h);
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}

void gfx_get_clip(int *x, int *y, int *w, int *h)
{
    *x = clip_x0; *y = clip_y0;
    *w = clip_x1 - clip_x0; *h = clip_y1 - clip_y0;
}

void gfx_darken_span(fb_data *d, int n, unsigned keep)
{
    for (int i = 0; i < n; i++)
        d[i] = darken(d[i], keep);
}

void gfx_snapshot(fb_data *dst)
{
    memcpy(dst, osm_fb, sizeof(osm_fb));
}

void gfx_restore(const fb_data *src)
{
    memcpy(osm_fb, src, sizeof(osm_fb));
}

/* clip a box; false when nothing is left */
static inline bool clipbox(int *x, int *y, int *w, int *h)
{
    int x0 = MAX(*x, clip_x0), y0 = MAX(*y, clip_y0);
    int x1 = MIN(*x + *w, clip_x1), y1 = MIN(*y + *h, clip_y1);
    if (x1 <= x0 || y1 <= y0)
        return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* ── shapes ────────────────────────────────────────────────────────── */

void gfx_fill(int x, int y, int w, int h, color_t c)
{
    if (!clipbox(&x, &y, &w, &h))
        return;
    if ((c >> 24) == 0xFF)
    {
        fb_data p = pack((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        for (int j = 0; j < h; j++)
        {
            fb_data *d = &PIX(x, y + j);
            for (int i = 0; i < w; i++)
                d[i] = p;
        }
        return;
    }
    if ((c & 0x00FFFFFFu) == 0)
    {
        unsigned a = c >> 24;
        unsigned keep = 256 - (a + (a >> 7));
        for (int j = 0; j < h; j++)
        {
            fb_data *d = &PIX(x, y + j);
            for (int i = 0; i < w; i++)
                d[i] = darken(d[i], keep);
        }
        return;
    }
    unsigned a = c >> 24, cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    a += a >> 7;
    for (int j = 0; j < h; j++)
    {
        fb_data *d = &PIX(x, y + j);
        for (int i = 0; i < w; i++)
            blend_px(&d[i], cr, cg, cb, a);
    }
}

void gfx_clear(color_t c)
{
    int x, y, w, h;
    gfx_get_clip(&x, &y, &w, &h);
    gfx_noclip();
    gfx_fill(0, 0, SCR_W, SCR_H, c);
    gfx_clip(x, y, w, h);
}

/* integer square root of a 32-bit value */
static unsigned isqrt(unsigned long v)
{
    unsigned long r = 0, bit = 1UL << 30;
    while (bit > v)
        bit >>= 2;
    while (bit)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* coverage (0..256) of a pixel at distance d16 (1/16 px) from a circle edge
 * of radius r16 (inside positive) */
static inline unsigned edge_cov(int r16, int d16)
{
    int in = r16 - d16 + 8;            /* + half a pixel */
    if (in <= 0) return 0;
    if (in >= 16) return 256;
    return in * 16;
}

/* the coverage of pixel (px,py) in a rounded box, given its corner circle;
 * in2/out2 are the squared distances inside / beyond which the pixel is
 * fully in or out, so the square root is only needed along the edge */
static inline unsigned corner_cov(int px, int py, int cx16, int cy16, int r16,
                                  int in2, int out2)
{
    int dx = (px << 4) + 8 - cx16, dy = (py << 4) + 8 - cy16;
    int d2 = dx * dx + dy * dy;
    if (d2 <= in2)
        return 256;
    if (d2 >= out2)
        return 0;
    return edge_cov(r16, isqrt((unsigned long)d2));
}

static inline int sq(int v) { return v * v; }

void gfx_rrect(int x, int y, int w, int h, int r, color_t c)
{
    if (w <= 0 || h <= 0)
        return;
    r = MIN(r, MIN(w, h) / 2);
    if (r <= 0)
    {
        gfx_fill(x, y, w, h, c);
        return;
    }
    /* the middle band, the sides between the corners */
    gfx_fill(x, y + r, w, h - 2 * r, c);
    gfx_fill(x + r, y, w - 2 * r, r, c);
    gfx_fill(x + r, y + h - r, w - 2 * r, r, c);

    int r16 = r << 4;
    int in2 = sq(MAX(0, r16 - 8)), out2 = sq(r16 + 8);
    int cx[2] = { ((x + r) << 4), ((x + w - r) << 4) };
    int cy[2] = { ((y + r) << 4), ((y + h - r) << 4) };
    for (int k = 0; k < 4; k++)
    {
        int bx = (k & 1) ? x + w - r : x;
        int by = (k & 2) ? y + h - r : y;
        for (int j = 0; j < r; j++)
        {
            int py = by + j;
            if (py < clip_y0 || py >= clip_y1) continue;
            for (int i = 0; i < r; i++)
            {
                int px = bx + i;
                if (px < clip_x0 || px >= clip_x1) continue;
                unsigned cov = corner_cov(px, py, cx[k & 1], cy[k >> 1], r16, in2, out2);
                if (cov)
                    blend_c(&PIX(px, py), c, cov);
            }
        }
    }
}

void gfx_rrect_border(int x, int y, int w, int h, int r, int t, color_t c)
{
    if (w <= 0 || h <= 0 || t <= 0)
        return;
    r = MIN(r, MIN(w, h) / 2);
    int ri = MAX(0, r - t);
    /* straight edges */
    gfx_fill(x + r, y, w - 2 * r, t, c);
    gfx_fill(x + r, y + h - t, w - 2 * r, t, c);
    gfx_fill(x, y + r, t, h - 2 * r, c);
    gfx_fill(x + w - t, y + r, t, h - 2 * r, c);
    if (r <= 0)
        return;
    int r16 = r << 4, ri16 = ri << 4;
    int out2 = sq(r16 + 8), in2 = ri > 0 ? sq(MAX(0, ri16 - 8)) : -1;
    int cx[2] = { ((x + r) << 4), ((x + w - r) << 4) };
    int cy[2] = { ((y + r) << 4), ((y + h - r) << 4) };
    for (int k = 0; k < 4; k++)
    {
        int bx = (k & 1) ? x + w - r : x;
        int by = (k & 2) ? y + h - r : y;
        for (int j = 0; j < r; j++)
        {
            int py = by + j;
            if (py < clip_y0 || py >= clip_y1) continue;
            for (int i = 0; i < r; i++)
            {
                int px = bx + i;
                if (px < clip_x0 || px >= clip_x1) continue;
                int dx = (px << 4) + 8 - cx[k & 1], dy = (py << 4) + 8 - cy[k >> 1];
                int d2 = dx * dx + dy * dy;
                if (d2 >= out2 || d2 <= in2)
                    continue;               /* beyond the ring, or inside it */
                unsigned d = isqrt((unsigned long)d2);
                int outer = edge_cov(r16, d);
                int inner = ri > 0 ? (int)edge_cov(ri16, d) : 0;
                int cov = outer - inner;
                if (cov > 0)
                    blend_c(&PIX(px, py), c, cov);
            }
        }
    }
}

void gfx_circle(int cx, int cy, int r, color_t c)
{
    gfx_rrect(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

static inline color_t lerp_c(color_t a, color_t b, int t /* 0..256 */)
{
    unsigned r = 0;
    for (int s = 0; s < 32; s += 8)
    {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        r |= (unsigned)((ca + (((cb - ca) * t) >> 8)) & 0xFF) << s;
    }
    return r;
}

void gfx_hgrad(int x, int y, int w, int h, color_t c0, color_t c1)
{
    for (int i = 0; i < w; i++)
        gfx_fill(x + i, y, 1, h, lerp_c(c0, c1, w > 1 ? i * 256 / (w - 1) : 0));
}

void gfx_vgrad(int x, int y, int w, int h, color_t c0, color_t c1)
{
    for (int j = 0; j < h; j++)
        gfx_fill(x, y + j, w, 1, lerp_c(c0, c1, h > 1 ? j * 256 / (h - 1) : 0));
}

/* ── soft shadows and glows ─────────────────────────────────────────
 *
 * Both are a stack of growing, fading rounded boxes. Drawing the stack
 * every frame costs one blend per layer per pixel (a menu's shadow is
 * hundreds of thousands of blends); instead the stack is summed once into
 * an 8-bit mask, kept in a small cache, and laid down with a single blend
 * per pixel. */

#define MASK_SLOTS 4
#define MASK_BYTES ((SCR_W + 80) * (SCR_H + 80))
static struct mask {
    int key[6];
    int w, h;               /* of the mask */
    int ox, oy;             /* its origin relative to the box's x, y */
    unsigned long used;
    bool valid;
} masks[MASK_SLOTS];
static uint8_t mask_pool[MASK_SLOTS][MASK_BYTES];
static unsigned long mask_clock;

/* coverage (0..256) of pixel centre (px,py) in a rounded box */
static unsigned rrect_cov(int px, int py, int x, int y, int w, int h, int r)
{
    if (px < x || py < y || px >= x + w || py >= y + h)
        return 0;
    r = MIN(r, MIN(w, h) / 2);
    int cx, cy;
    if (px < x + r) cx = x + r;
    else if (px >= x + w - r) cx = x + w - r;
    else return 256;
    if (py < y + r) cy = y + r;
    else if (py >= y + h - r) cy = y + h - r;
    else return 256;
    int r16 = r << 4;
    return corner_cov(px, py, cx << 4, cy << 4, r16, sq(MAX(0, r16 - 8)), sq(r16 + 8));
}

static struct mask *mask_find(const int key[6], uint8_t **data)
{
    struct mask *victim = &masks[0];
    for (int i = 0; i < MASK_SLOTS; i++)
    {
        struct mask *m = &masks[i];
        if (m->valid && !memcmp(m->key, key, sizeof m->key))
        {
            m->used = ++mask_clock;
            *data = mask_pool[i];
            return m;
        }
        if (!m->valid || m->used < victim->used)
            victim = m;
    }
    int slot = victim - masks;
    memcpy(victim->key, key, sizeof victim->key);
    victim->valid = false;          /* the caller fills it */
    victim->used = ++mask_clock;
    *data = mask_pool[slot];
    return victim;
}

void gfx_shadow(int x, int y, int w, int h, int r, int dy, int blur, int alpha)
{
    /* the layers, relative to the box: as before, a cheap gaussian */
    int steps = MAX(1, blur / 2);
    int key[6] = { 1, w, h, r, dy, blur | alpha << 8 };
    uint8_t *a;
    struct mask *m = mask_find(key, &a);
    if (!m->valid)
    {
        m->ox = -blur / 2 - 1;
        m->oy = dy - blur / 2 - 1;
        m->w = w + blur + 2;
        m->h = h + blur + 2;
        if (m->w * m->h > MASK_BYTES)
            return;
        for (int j = 0; j < m->h; j++)
            for (int i = 0; i < m->w; i++)
            {
                int px = m->ox + i, py = m->oy + j;
                unsigned keep = 256;            /* what shows through, 0..256 */
                for (int s2 = steps; s2 >= 1; s2--)
                {
                    int g = s2 * blur / steps;
                    int la = alpha * (steps - s2 + 1) / (steps * steps / 2 + steps);
                    la = MIN(255, la);
                    unsigned cov = rrect_cov(px, py, -g / 2, dy - g / 2, w + g, h + g, r + g / 2);
                    if (cov)
                        keep = keep * (256 - ((unsigned)la * cov >> 8)) >> 8;
                }
                a[j * m->w + i] = (uint8_t)MIN(255, 256 - keep);
            }
        m->valid = true;
    }
    /* the box itself is drawn opaque over the shadow by every caller:
     * skip what it covers */
    int rr = MIN(r, MIN(w, h) / 2);
    for (int j = 0; j < m->h; j++)
    {
        int py = y + m->oy + j;
        if (py < clip_y0 || py >= clip_y1)
            continue;
        bool mid = py >= y + rr && py < y + h - rr;
        bool band = py >= y && py < y + h;
        const uint8_t *row = &a[j * m->w];
        fb_data *d = &PIX(0, py);
        for (int i = 0; i < m->w; i++)
        {
            int px = x + m->ox + i;
            if (px < clip_x0 || px >= clip_x1 || !row[i])
                continue;
            if (band && ((mid && px >= x && px < x + w) || (px >= x + rr && px < x + w - rr)))
                continue;
            d[px] = darken(d[px], 256 - row[i]);
        }
    }
}

void gfx_glow(int cx, int cy, int r, int blur, color_t c)
{
    /* the shape, at full strength, is cached; the colour's alpha scales it */
    int base = c >> 24;
    int key[6] = { 2, r, blur, 0, 0, 0 };
    uint8_t *a;
    struct mask *m = mask_find(key, &a);
    int R = r + blur + 1;
    if (!m->valid)
    {
        m->w = m->h = 2 * R;
        m->ox = m->oy = -R;
        if (m->w * m->h > MASK_BYTES)
            return;
        for (int j = 0; j < m->h; j++)
            for (int i = 0; i < m->w; i++)
            {
                int px = m->ox + i, py = m->oy + j;
                unsigned sum = 0;
                for (int s2 = blur; s2 >= 1; s2 -= 2)
                {
                    int rs = r + s2;
                    unsigned cov = rrect_cov(px, py, -rs, -rs, 2 * rs, 2 * rs, rs);
                    /* the layer's share of the alpha, as the old stack */
                    sum += cov * (unsigned)(blur - s2 + 2) * 255 / (unsigned)(blur * 3) >> 8;
                }
                a[j * m->w + i] = (uint8_t)MIN(255u, sum);
            }
        m->valid = true;
    }
    unsigned cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    for (int j = 0; j < m->h; j++)
    {
        int py = cy + m->oy + j;
        if (py < clip_y0 || py >= clip_y1)
            continue;
        const uint8_t *row = &a[j * m->w];
        for (int i = 0; i < m->w; i++)
        {
            int px = cx + m->ox + i;
            if (px < clip_x0 || px >= clip_x1 || !row[i])
                continue;
            unsigned la = (unsigned)row[i] * base / 255;
            la += la >> 7;
            blend_px(&PIX(px, py), cr, cg, cb, la);
        }
    }
}

void gfx_line16(int x0, int y0, int x1, int y1, int w16, color_t c)
{
    /* distance from each pixel centre to the segment, minus the radius */
    int hw = w16 / 2;
    int bx0 = (MIN(x0, x1) - hw - 16) >> 4, bx1 = (MAX(x0, x1) + hw + 16) >> 4;
    int by0 = (MIN(y0, y1) - hw - 16) >> 4, by1 = (MAX(y0, y1) + hw + 16) >> 4;
    bx0 = MAX(bx0, clip_x0); by0 = MAX(by0, clip_y0);
    bx1 = MIN(bx1, clip_x1 - 1); by1 = MIN(by1, clip_y1 - 1);
    long dx = x1 - x0, dy = y1 - y0;
    long len2 = dx * dx + dy * dy;
    for (int py = by0; py <= by1; py++)
    {
        for (int px = bx0; px <= bx1; px++)
        {
            long qx = (px << 4) + 8 - x0, qy = (py << 4) + 8 - y0;
            long t = len2 ? ((qx * dx + qy * dy) << 8) / len2 : 0;   /* 0..256 */
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            long ex = qx - ((dx * t) >> 8), ey = qy - ((dy * t) >> 8);
            unsigned d = isqrt((unsigned long)(ex * ex + ey * ey));
            unsigned cov = edge_cov(hw, d);
            if (cov)
                blend_c(&PIX(px, py), c, cov);
        }
    }
}

/* ── pictures ──────────────────────────────────────────────────────── */

static const struct osm_icon_bitmap *find_icon(enum osm_icon icon, int size)
{
    const struct osm_icon_bitmap *best = NULL;
    for (int i = 0; i < OSM_ICON_BITMAP_COUNT; i++)
    {
        const struct osm_icon_bitmap *b = &osm_icon_bitmaps[i];
        if (b->icon != icon)
            continue;
        if (b->size == size)
            return b;
        /* else the closest size not larger, or the smallest */
        if (!best || (b->size <= size && b->size > best->size) ||
            (best->size > size && b->size < best->size))
            best = b;
    }
    return best;
}

/* 4-bit alpha mask, rows padded to bytes, high nibble first, 15 = ink */
static void mask4(const uint8_t *bits, int w, int h, int x, int y, color_t c)
{
    int stride = (w + 1) / 2;
    for (int j = 0; j < h; j++)
    {
        int py = y + j;
        if (py < clip_y0 || py >= clip_y1) continue;
        const uint8_t *row = bits + j * stride;
        for (int i = 0; i < w; i++)
        {
            int px = x + i;
            if (px < clip_x0 || px >= clip_x1) continue;
            unsigned a = (i & 1) ? row[i >> 1] & 0xF : row[i >> 1] >> 4;
            if (a)
                blend_c(&PIX(px, py), c, a * 17 + (a >> 3));
        }
    }
}

void gfx_icon(enum osm_icon icon, int size, int x, int y, color_t c)
{
    const struct osm_icon_bitmap *b = find_icon(icon, size);
    if (!b)
        return;
    int off = (size - b->size) / 2;
    mask4(b->bits, b->size, b->size, x + off, y + off, c);
}

void gfx_image(const struct osm_image *img, int x, int y, int opacity)
{
    if (!img->rgb)
        return;
    x += img->x; y += img->y;
    if (!img->alpha && opacity >= 256)
    {
        /* opaque: whole rows */
        int x0 = MAX(x, clip_x0), x1 = MIN(x + img->w, clip_x1);
        for (int j = 0; j < img->h && x1 > x0; j++)
        {
            int py = y + j;
            if (py < clip_y0 || py >= clip_y1) continue;
            memcpy(&PIX(x0, py), img->rgb + j * img->w + (x0 - x),
                   (x1 - x0) * sizeof(fb_data));
        }
        return;
    }
    for (int j = 0; j < img->h; j++)
    {
        int py = y + j;
        if (py < clip_y0 || py >= clip_y1) continue;
        const uint16_t *s = img->rgb + j * img->w;
        const uint8_t *a = img->alpha ? img->alpha + j * img->w : NULL;
        for (int i = 0; i < img->w; i++)
        {
            int px = x + i;
            if (px < clip_x0 || px >= clip_x1) continue;
            unsigned al = a ? a[i] : 255;
            al = (al * opacity) >> 8;
            if (al)
                blend_565(&PIX(px, py), s[i], al + (al >> 7));
        }
    }
}

void gfx_image_rotated(const struct osm_image *img, int px16, int py16,
                       int dx16, int dy16, int deci_deg)
{
    if (!img->rgb || img->w <= 0)
        return;
    /* fp_sincos: a full turn is 2^32 */
    long cosv;
    long sinv = fp_sincos((uint32_t)(int32_t)((long long)deci_deg * 4294967296LL / 3600), &cosv);
    /* Q31 -> Q14 */
    long s14 = sinv >> 17, c14 = cosv >> 17;

    /* bounding box of the rotated picture */
    int corners[4][2] = { {0, 0}, {img->w << 4, 0}, {0, img->h << 4}, {img->w << 4, img->h << 4} };
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    for (int k = 0; k < 4; k++)
    {
        long ux = corners[k][0] - px16, uy = corners[k][1] - py16;
        long rx = (ux * c14 - uy * s14) >> 14, ry = (ux * s14 + uy * c14) >> 14;
        minx = MIN(minx, rx); maxx = MAX(maxx, rx);
        miny = MIN(miny, ry); maxy = MAX(maxy, ry);
    }
    int x0 = MAX(clip_x0, (dx16 + minx) >> 4), x1 = MIN(clip_x1 - 1, ((dx16 + maxx) >> 4) + 1);
    int y0 = MAX(clip_y0, (dy16 + miny) >> 4), y1 = MIN(clip_y1 - 1, ((dy16 + maxy) >> 4) + 1);

    for (int y = y0; y <= y1; y++)
    {
        for (int x = x0; x <= x1; x++)
        {
            /* back into the picture (inverse rotation), in 1/256 px */
            long vx = ((x << 4) + 8 - dx16), vy = ((y << 4) + 8 - dy16);
            long sx = ((vx * c14 + vy * s14) >> 10) + (px16 << 4) - 128;
            long sy = ((-vx * s14 + vy * c14) >> 10) + (py16 << 4) - 128;
            int ix = sx >> 8, iy = sy >> 8;
            if (ix < -1 || iy < -1 || ix >= img->w || iy >= img->h)
                continue;
            int fx = sx & 0xFF, fy = sy & 0xFF;
            /* bilinear on alpha; the colour of the nearest texel */
            unsigned acc = 0;
            for (int k = 0; k < 4; k++)
            {
                int tx = ix + (k & 1), ty = iy + (k >> 1);
                if (tx < 0 || ty < 0 || tx >= img->w || ty >= img->h)
                    continue;
                unsigned a = img->alpha ? img->alpha[ty * img->w + tx] : 255;
                unsigned wx = (k & 1) ? fx : 256 - fx, wy = (k >> 1) ? fy : 256 - fy;
                acc += a * wx * wy;
            }
            unsigned a = acc >> 16;
            if (!a)
                continue;
            int nx = MIN(img->w - 1, MAX(0, ix + (fx >> 7)));
            int ny = MIN(img->h - 1, MAX(0, iy + (fy >> 7)));
            blend_565(&PIX(x, y), img->rgb[ny * img->w + nx], a + (a >> 7));
        }
    }
}

void gfx_bitmap(const fb_data *src, int sw, int sh, int x, int y, int dw, int dh,
                int radius, int opacity)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    int r16 = radius << 4;
    /* source step in 1/256 px */
    long stx = ((long)sw << 8) / dw, sty = ((long)sh << 8) / dh;
    bool same = (sw == dw && sh == dh);
    int x0 = MAX(x, clip_x0), x1 = MIN(x + dw, clip_x1);
    for (int j = 0; j < dh; j++)
    {
        int py = y + j;
        if (py < clip_y0 || py >= clip_y1) continue;
        if (same && opacity >= 256 && x1 > x0 && j >= radius && j < dh - radius)
        {
            /* 1:1, opaque, away from the rounded corners: the whole row */
            memcpy(&PIX(x0, py), &src[j * sw + (x0 - x)], (x1 - x0) * sizeof(fb_data));
            continue;
        }
        long syf = j * sty + sty / 2 - 128;
        int sy0 = MAX(0, syf >> 8), sy1 = MIN(sh - 1, sy0 + 1);
        int fy = syf < 0 ? 0 : syf & 0xFF;
        for (int i = 0; i < dw; i++)
        {
            int px = x + i;
            if (px < clip_x0 || px >= clip_x1) continue;
            unsigned cov = 256;
            if (radius)
            {
                int cx = -1, cy = -1;
                if (i < radius) cx = (x + radius) << 4;
                else if (i >= dw - radius) cx = (x + dw - radius) << 4;
                if (j < radius) cy = (y + radius) << 4;
                else if (j >= dh - radius) cy = (y + dh - radius) << 4;
                if (cx >= 0 && cy >= 0)
                    cov = corner_cov(px, py, cx, cy, r16, sq(MAX(0, r16 - 8)), sq(r16 + 8));
                if (!cov)
                    continue;
            }
            cov = (cov * opacity) >> 8;
            fb_data pix;
            if (same)
                pix = src[j * sw + i];
            else
            {
                long sxf = i * stx + stx / 2 - 128;
                int sx0 = MAX(0, sxf >> 8), sx1 = MIN(sw - 1, sx0 + 1);
                int fx = sxf < 0 ? 0 : sxf & 0xFF;
                fb_data p00 = src[sy0 * sw + sx0], p01 = src[sy0 * sw + sx1];
                fb_data p10 = src[sy1 * sw + sx0], p11 = src[sy1 * sw + sx1];
                unsigned w00 = (256 - fx) * (256 - fy), w01 = fx * (256 - fy);
                unsigned w10 = (256 - fx) * fy, w11 = fx * fy;
#define CH(p, s, m) (((p) >> (s)) & (m))
                unsigned r = (CH(p00, 11, 31) * w00 + CH(p01, 11, 31) * w01 + CH(p10, 11, 31) * w10 + CH(p11, 11, 31) * w11) >> 16;
                unsigned g = (CH(p00, 5, 63) * w00 + CH(p01, 5, 63) * w01 + CH(p10, 5, 63) * w10 + CH(p11, 5, 63) * w11) >> 16;
                unsigned b = (CH(p00, 0, 31) * w00 + CH(p01, 0, 31) * w01 + CH(p10, 0, 31) * w10 + CH(p11, 0, 31) * w11) >> 16;
#undef CH
                pix = (r << 11) | (g << 5) | b;
            }
            blend_565(&PIX(px, py), pix, cov);
        }
    }
}

/* ── text ──────────────────────────────────────────────────────────── */

void gfx_set_fallback_font(int rockbox_font_id)
{
    fallback_font = rockbox_font_id;
}

static const struct osm_glyph *find_glyph(const struct osm_font *f, unsigned cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        unsigned m = f->glyphs[mid].cp;
        if (m == cp) return &f->glyphs[mid];
        if (m < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int gfx_font_height(int font)
{
    return osm_fonts[font].height;
}

int gfx_font_ascent(int font)
{
    return osm_fonts[font].ascent;
}

/* width of one character, from DejaVu or from the fallback font */
static int char_width(const struct osm_font *f, unsigned cp)
{
    const struct osm_glyph *g = find_glyph(f, cp);
    if (g)
        return g->adv;
    struct font *pf = font_get(fallback_font);
    if (pf)
        return font_get_width(pf, cp);
    return f->height / 2;
}

/* a glyph DejaVu lacks, drawn from a Rockbox font, its baseline on ours */
static int draw_fallback(const struct osm_font *f, int x, int y, unsigned cp, color_t c)
{
    struct font *pf = font_get(fallback_font);
    if (!pf)
        return f->height / 2;
    int w = font_get_width(pf, cp);
    const unsigned char *bits = font_get_bits(pf, cp);
    int top = y + f->ascent - pf->ascent;
    for (int j = 0; j < (int)pf->height; j++)
    {
        int py = top + j;
        if (py < clip_y0 || py >= clip_y1) continue;
        for (int i = 0; i < w; i++)
        {
            int px = x + i;
            if (px < clip_x0 || px >= clip_x1) continue;
            unsigned a;
            if (pf->depth)
            {
                /* 4-bit, low nibble first, 0 = opaque */
                int n = j * w + i;
                unsigned v = bits[n >> 1];
                v = (n & 1) ? v >> 4 : v & 0xF;
                a = (15 - v) * 17;
            }
            else
                a = (bits[(j >> 3) * w + i] >> (j & 7)) & 1 ? 256 : 0;
            if (a)
                blend_c(&PIX(px, py), c, a);
        }
    }
    return w;
}

static int text_run(int font, int x, int y, const char *s, color_t c, int spacing16, bool draw)
{
    const struct osm_font *f = &osm_fonts[font];
    int pen16 = x << 4;
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        ucschar_t cp;
        p = utf8decode(p, &cp);
        const struct osm_glyph *g = find_glyph(f, cp);
        int px = pen16 >> 4;
        int adv;
        if (g)
        {
            if (draw && g->w)
                mask4(osm_fonts[font].bits + g->off, g->w, g->h, px + g->xoff, y + g->yoff, c);
            adv = g->adv;
        }
        else if (cp == 0xFEFF || cp == 0x200B)
            adv = 0;                        /* BOM, zero width space */
        else
            adv = draw ? draw_fallback(f, px, y, cp, c) : char_width(f, cp);
        pen16 += (adv << 4) + spacing16;
    }
    if (*s)
        pen16 -= spacing16;
    return (pen16 >> 4) - x;
}

int gfx_text_width(int font, const char *s)
{
    return text_run(font, 0, 0, s, 0, 0, false);
}

int gfx_text_width_ls(int font, const char *s, int spacing16)
{
    return text_run(font, 0, 0, s, 0, spacing16, false);
}

int gfx_text(int font, int x, int y, const char *s, color_t c)
{
    return text_run(font, x, y, s, c, 0, true);
}

int gfx_text_ls(int font, int x, int y, const char *s, color_t c, int spacing16)
{
    return text_run(font, x, y, s, c, spacing16, true);
}

/* the longest prefix of s (in bytes) that fits in maxw with "…" after it */
static int fit_prefix(int font, const char *s, int maxw, bool *cut)
{
    const struct osm_font *f = &osm_fonts[font];
    int ell = char_width(f, 0x2026);
    int w = 0, last_ok = 0;
    const unsigned char *p = (const unsigned char *)s;
    *cut = false;
    while (*p)
    {
        ucschar_t cp;
        const unsigned char *q = utf8decode(p, &cp);
        int cw = char_width(f, cp);
        if (w + cw > maxw)
        {
            *cut = true;
            return last_ok;
        }
        w += cw;
        if (w + ell <= maxw)
            last_ok = q - (const unsigned char *)s;
        p = q;
    }
    return p - (const unsigned char *)s;
}

int gfx_text_fit(int font, int x, int y, int maxw, const char *s, color_t c, int align)
{
    if (maxw <= 0)
        return 0;
    int w = gfx_text_width(font, s);
    if (w <= maxw)
    {
        if (align == ALIGN_CENTER) x += (maxw - w) / 2;
        else if (align == ALIGN_RIGHT) x += maxw - w;
        return gfx_text(font, x, y, s, c);
    }
    char buf[256];
    bool cut;
    int n = fit_prefix(font, s, maxw, &cut);
    n = MIN(n, (int)sizeof(buf) - 4);
    memcpy(buf, s, n);
    /* no trailing space before the ellipsis */
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    strcpy(buf + n, "\xE2\x80\xA6");
    return gfx_text(font, x, y, buf, c);
}

int gfx_text_wrap(int font, int x, int y, int maxw, int lines, int lh,
                  const char *s, color_t c)
{
    const struct osm_font *f = &osm_fonts[font];
    int used = 0;
    while (*s && used < lines)
    {
        if (used == lines - 1)
        {
            gfx_text_fit(font, x, y + used * lh, maxw, s, c, ALIGN_LEFT);
            return used + 1;
        }
        /* take words while they fit */
        const unsigned char *p = (const unsigned char *)s;
        const unsigned char *brk = NULL;
        int w = 0;
        while (*p)
        {
            ucschar_t cp;
            const unsigned char *q = utf8decode(p, &cp);
            int cw = char_width(f, cp);
            if (w + cw > maxw)
                break;
            w += cw;
            if (cp == ' ')
                brk = p;
            p = q;
        }
        int n;
        if (!*p)
            n = p - (const unsigned char *)s;
        else if (brk)
            n = brk - (const unsigned char *)s;
        else
            n = p - (const unsigned char *)s;   /* one long word: cut it */
        char buf[256];
        n = MIN(n, (int)sizeof(buf) - 1);
        memcpy(buf, s, n);
        buf[n] = 0;
        gfx_text(font, x, y + used * lh, buf, c);
        used++;
        s += n;
        while (*s == ' ')
            s++;
    }
    return used;
}

/* ── the Osmium Sound mark (BrandMark.qml) ─────────────────────────── */

int gfx_brand(int x, int y, int cap, bool sound)
{
    /* DejaVu Sans Bold: the cap height is 0.729 em */
    int font = cap <= 7 ? OSM_FONT_B10 : cap <= 9 ? OSM_FONT_B12 :
               cap <= 10 ? OSM_FONT_B13 : cap <= 11 ? OSM_FONT_B15 :
               cap <= 13 ? OSM_FONT_B18 : OSM_FONT_B22;
    const struct osm_font *f = &osm_fonts[font];
    int capy = f->ascent - (f->height * 729 / 1000 * 1000 / 1170);
    /* the cap top inside the line box: ascent - cap height of that font */
    int fcap = (f->height * 1000 / 1164) * 729 / 1000;
    capy = f->ascent - fcap;
    int ty = y - capy;                       /* so the capitals start at y */
    static const int bar_x[] = { 0, 568, 1012, 1346, 1605, 1815, 1988 };
    int bw = MAX(1, (fcap * 74 + 500) / 1000);
    for (int i = 0; i < (int)ARRAYLEN(bar_x); i++)
        gfx_fill(x + (bar_x[i] * fcap + 500) / 1000, y, bw, fcap, C_GOLD);
    int bars_w = (1988 * fcap + 500) / 1000 + bw;
    int tx = x + bars_w + fcap / 2;
    int track16 = fcap * 178 / 1000 * 16 / 1;
    tx += gfx_text_ls(font, tx, ty, "OSMIUM", C_GOLD, track16 / 1);
    if (sound)
    {
        tx += fcap * 55 / 100;
        tx += gfx_text_ls(font, tx, ty, "SOUND", C_INK, track16);
    }
    return tx - x;
}
