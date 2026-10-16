/***************************************************************************
 *   Osmium Sound for FiiO M3K — drawing
 *
 *   The interface draws into its own RGB565 frame (osm_fb) and hands it to
 *   the LCD driver in one piece (gfx_present). Colours are 0xAARRGGBB: an
 *   alpha below 0xFF blends, as the translucent whites and golds of the
 *   Osmium theme need.
 ****************************************************************************/
#ifndef OSM_GFX_H
#define OSM_GFX_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "lcd.h"
#include "assets/osm_fonts.h"
#include "assets/osm_icons.h"
#include "assets/osm_images.h"

#define SCR_W LCD_WIDTH
#define SCR_H LCD_HEIGHT

typedef uint32_t color_t;       /* 0xAARRGGBB */

/* ── the Osmium palette (Theme.qml) ───────────────────────────────── */
#define C_DARK      0xFF0A0A0Au
#define C_PANEL     0xFF0F0F0Fu
#define C_GRAY      0xFF1A1A1Au
#define C_SURFACE   0xFF161616u
#define C_LIGHT     0xFF2A2A2Au
#define C_ACCENT    0xFF3A3A3Au
#define C_BORDER    0xFF252525u
#define C_GOLD      0xFFD4AF37u
#define C_SILVER    0xFFC0C0C0u
#define C_WHITE     0xFFFFFFFFu
#define C_BLACK     0xFF000000u
#define C_EMERALD   0xFF10B981u
#define C_RED400    0xFFF87171u
#define C_RED500    0xFFEF4444u
#define C_YELLOW400 0xFFFACC15u
#define C_INK       0xFFF1F0EAu     /* the logo's off-white */

/* the same colour at another opacity: A(C_GOLD, 0.3) */
#define ALPHA_OF(f)  ((uint32_t)((f) * 255.0f + 0.5f))
#define A(c, f)      (((c) & 0x00FFFFFFu) | (ALPHA_OF(f) << 24))
#define WA(f)        A(C_WHITE, f)
#define SILVER_A(f)  A(C_SILVER, f)
#define GOLD_A(f)    A(C_GOLD, f)
#define BLACK_A(f)   A(C_BLACK, f)

extern fb_data osm_fb[SCR_H * SCR_W];

/* ── frame ─────────────────────────────────────────────────────────── */
void gfx_init(void);
void gfx_present(void);                    /* the whole frame to the LCD */
void gfx_clip(int x, int y, int w, int h); /* intersects nothing: replaces */
void gfx_noclip(void);
void gfx_get_clip(int *x, int *y, int *w, int *h);

/* a copy of the frame, for transitions */
void gfx_snapshot(fb_data *dst);
void gfx_restore(const fb_data *src);
/* scale n pixels to keep/256 of their brightness */
void gfx_darken_span(fb_data *d, int n, unsigned keep);

/* ── shapes ────────────────────────────────────────────────────────── */
void gfx_fill(int x, int y, int w, int h, color_t c);
void gfx_clear(color_t c);
void gfx_rrect(int x, int y, int w, int h, int r, color_t c);
void gfx_rrect_border(int x, int y, int w, int h, int r, int t, color_t c);
void gfx_circle(int cx, int cy, int r, color_t c);        /* filled */
void gfx_hgrad(int x, int y, int w, int h, color_t c0, color_t c1);
void gfx_vgrad(int x, int y, int w, int h, color_t c0, color_t c1);
/* a soft shadow under a rounded box (box-shadow 0 dy blur rgba(0,0,0,a)) */
void gfx_shadow(int x, int y, int w, int h, int r, int dy, int blur, int alpha);
/* a soft glow around a circle (the gold play button) */
void gfx_glow(int cx, int cy, int r, int blur, color_t c);
/* a thick anti-aliased segment with round ends, coordinates in 1/16 px */
void gfx_line16(int x0, int y0, int x1, int y1, int w16, color_t c);

/* ── pictures ──────────────────────────────────────────────────────── */
void gfx_icon(enum osm_icon icon, int size, int x, int y, color_t c);
void gfx_image(const struct osm_image *img, int x, int y, int opacity);
/* the image rotated by `deci_deg` (1/10 degree, clockwise) around its pivot
 * (px16, py16), placed so the pivot lands on (dx16, dy16) */
void gfx_image_rotated(const struct osm_image *img, int px16, int py16,
                       int dx16, int dy16, int deci_deg);
/* RGB565 pixels, scaled to dw x dh (bilinear), with rounded corners */
void gfx_bitmap(const fb_data *src, int sw, int sh, int x, int y, int dw, int dh,
                int radius, int opacity);

/* ── text ──────────────────────────────────────────────────────────── */
enum { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT };

int  gfx_font_height(int font);
int  gfx_font_ascent(int font);
int  gfx_text_width(int font, const char *s);
int  gfx_text_width_ls(int font, const char *s, int spacing16);
/* draws at (x, y = top of the line); returns the width drawn */
int  gfx_text(int font, int x, int y, const char *s, color_t c);
/* letter-spaced (spacing in 1/16 px), as Osmium's small caps labels */
int  gfx_text_ls(int font, int x, int y, const char *s, color_t c, int spacing16);
/* clipped to maxw with an ellipsis; align within maxw */
int  gfx_text_fit(int font, int x, int y, int maxw, const char *s, color_t c, int align);
/* word-wrapped in up to `lines` lines of `lh` px; the last one elided.
 * Returns the number of lines used. */
int  gfx_text_wrap(int font, int x, int y, int maxw, int lines, int lh,
                   const char *s, color_t c);
/* the Rockbox font used for characters DejaVu lacks (CJK, ...) */
void gfx_set_fallback_font(int rockbox_font_id);

/* the Osmium Sound mark: gold bars, OSMIUM in gold, SOUND in off-white */
int  gfx_brand(int x, int y, int cap, bool sound);

/* the mark while it builds up (the boot animation): the same layout, with
 * each part where and how the animation has it */
#define BRAND_BARS    7
#define BRAND_LETTERS 6                 /* O S M I U M */
struct gfx_brand_look {
    int bars_h1000;                     /* height of the bars, 1/1000 of full */
    int bars_open[BRAND_BARS];          /* 1/1000 of the way from the first
                                           bar to their place */
    color_t bars_color[BRAND_BARS];
    int letters_alpha[BRAND_LETTERS];   /* 0..255 */
    int letters_dx[BRAND_LETTERS];      /* offset, 1/1000 of the cap height */
};
int  gfx_brand_ex(int x, int y, int cap, bool sound, const struct gfx_brand_look *look);

#endif
