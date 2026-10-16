/***************************************************************************
 *   Osmium Sound for FiiO M3K — Now Playing
 *
 *   NowPlaying.qml on a 240x320 portrait screen. The backdrop is the cover,
 *   tiny, stretched and dimmed to 20 % under a gradient to black; then the
 *   header (clock pill, NOW PLAYING, favourite, battery), the cover with
 *   its shadow — or the analogue VU meters with the LED status plate —, the
 *   title, the artist in gold, the album, the format chip between the
 *   times, the gold progress bar, the transport with the gold Play disc
 *   and its glow, and the volume.
 *
 *   Keys: Select play/pause · Left/Right previous/next (held: seek) ·
 *   Down cover <-> VU meters · Up the queue · wheel volume · long Select
 *   or long Menu: the menu · Menu or Back: back to the library.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "playlist.h"
#include "playback.h"
#include "powermgmt.h"
#include "misc.h"
#include "settings.h"
#include "timefuncs.h"
#include "tagcache.h"
#include "string-extra.h"
#include "fixedpoint.h"
#include "osm_ui.h"
#include "osm_screens.h"
#include "osm_player.h"
#include "osm_library.h"
#include "osm_settings.h"

/* the frame without what moves (Osmium's "staticLayer") */
static fb_data layer[SCR_W * SCR_H] CACHEALIGN_ATTR;
static uint32_t layer_key = 0;

static bool queue_open;
static struct osm_list queue_list;
static long seek_step;

/* ── layout ────────────────────────────────────────────────────────── */

#define HEADER_H    30
#define COVER_SIZE  140
#define COVER_Y     34

struct np_layout {
    int title_y, artist_y, album_y;     /* album_y < 0: no album line */
    int times_y, bar_y, controls_cy, volume_y;
    int title_font;
};

static void layout(struct np_layout *l)
{
    if (global_settings.osm_np_view == OSM_NP_VU)
    {
        l->title_y = 212; l->artist_y = 230; l->album_y = -1;
        l->title_font = OSM_FONT_B13;
    }
    else
    {
        l->title_y = 180; l->artist_y = 199; l->album_y = 215;
        l->title_font = OSM_FONT_B15;
    }
    l->times_y = 248;
    l->bar_y = 264;
    l->controls_cy = 286;
    l->volume_y = 310;
    if (global_settings.osm_np_view != OSM_NP_VU)
    {
        l->times_y = 234;
        l->bar_y = 250;
        l->controls_cy = 280;
    }
}

static const struct osm_vu_skin *vu_skin(void)
{
    int s = global_settings.osm_vu_skin;
    if ((unsigned)s >= OSM_VU_SKIN_COUNT)
        s = 0;
    return &osm_vu_skins[s];
}

/* ── the static layer ──────────────────────────────────────────────── */

static void draw_backdrop(void)
{
    gfx_clear(C_DARK);
    if (player.cur.valid && player.have_backdrop)
    {
        /* bg-cover scale-125 blur-lg at 20 %: the tiny cover stretched
         * square over the whole height is already soft */
        int s = SCR_H + 60;
        gfx_bitmap(player.backdrop, player.backdrop_w, player.backdrop_h,
                   (SCR_W - s) / 2, -30, s, s, 0, 52);
    }
    /* from transparent to black at the bottom */
    gfx_vgrad(0, 0, SCR_W, SCR_H / 2, 0x00000000u, BLACK_A(0.6));
    gfx_vgrad(0, SCR_H / 2, SCR_W, SCR_H / 2, BLACK_A(0.6), BLACK_A(0.95));
}

static void draw_header(void)
{
    /* the clock in a pill (tap = screensaver in Osmium) */
    struct tm *tm = get_time();
    char clock[8];
    if (global_settings.timeformat)
        snprintf(clock, sizeof clock, "%d:%02d", (tm->tm_hour + 11) % 12 + 1, tm->tm_min);
    else
        snprintf(clock, sizeof clock, "%02d:%02d", tm->tm_hour, tm->tm_min);
    int cw = gfx_text_width(OSM_FONT_R12, clock) + 18;
    gfx_rrect(8, 6, cw, 20, 10, WA(0.10));
    gfx_text(OSM_FONT_R12, 17, 8, clock, C_WHITE);

    char up[48];
    ui_upper(up, sizeof up, osm_str(S_NOW_PLAYING));
    int lw = gfx_text_width_ls(OSM_FONT_R10, up, 40);
    gfx_text_ls(OSM_FONT_R10, (SCR_W - lw) / 2, 10, up, SILVER_A(0.7), 40);

    /* the heart and the battery */
    int xr = SCR_W - 8;
    xr -= ui_draw_battery(xr, 11) + 8;
    bool fav = player.cur.valid && player_is_favorite(player.cur.path);
    gfx_icon(fav ? ICON_HEART_FILL : ICON_HEART, 16, xr - 16, 7, fav ? C_GOLD : SILVER_A(0.6));
    if (player_sleep_minutes() > 0)
        gfx_icon(ICON_MOON, 14, xr - 16 - 20, 8, C_GOLD);
}

static void draw_cover(void)
{
    int x = (SCR_W - COVER_SIZE) / 2, y = COVER_Y;
    /* shadow 0 20px 60px rgba(0,0,0,.7) */
    gfx_shadow(x, y, COVER_SIZE, COVER_SIZE, 14, 10, 22, 190);
    if (player.cur.valid && player.have_cover)
    {
        gfx_rrect(x, y, COVER_SIZE, COVER_SIZE, 14, C_BLACK);
        int w = player.cover_w, h = player.cover_h;
        int dw = COVER_SIZE, dh = COVER_SIZE;
        if (w > h) dh = COVER_SIZE * h / w;
        else if (h > w) dw = COVER_SIZE * w / h;
        gfx_bitmap(player.cover, w, h, x + (COVER_SIZE - dw) / 2, y + (COVER_SIZE - dh) / 2,
                   dw, dh, 14, 256);
    }
    else
    {
        gfx_rrect(x, y, COVER_SIZE, COVER_SIZE, 14, C_LIGHT);
        gfx_icon(ICON_MUSIC, 44, x + (COVER_SIZE - 44) / 2, y + (COVER_SIZE - 44) / 2,
                 SILVER_A(0.3));
    }
    gfx_rrect_border(x, y, COVER_SIZE, COVER_SIZE, 14, 1, WA(0.08));
}

/* LedBar.qml: Hi-Res / PCM / DSD, BitPerfect, ReplayGain, and the mark */
static void draw_led_plate(int x, int y)
{
    const struct osm_led_plate *p = &osm_led_plate;
    gfx_image(&p->base, x, y, 256);
    if (player.cur.valid)
    {
        gfx_image(&p->pcm, x, y, 256);
        if (player_led_hires())
            gfx_image(&p->hires, x, y, 256);
        if (player_led_bitperfect())
            gfx_image(&p->bitperfect, x, y, 256);
        if (player_led_replaygain())
            gfx_image(&p->replaygain, x, y, 256);
    }
    /* the seven gold bars where the DSP tile was */
    int cx = x + p->w * 7585 / 8970, cy = y + p->h / 2;
    static const int bx[] = { 0, 568, 1012, 1346, 1605, 1815, 1988 };
    int cap = 10;
    int w = 1988 * cap / 1000 + 1;
    for (unsigned i = 0; i < ARRAYLEN(bx); i++)
        gfx_fill(cx - w / 2 + bx[i] * cap / 1000, cy - cap / 2, 1, cap, C_GOLD);
}

static void draw_texts(const struct np_layout *l)
{
    const struct osm_track *t = &player.cur;
    int x = 16, w = SCR_W - 32;
    if (!t->valid)
    {
        gfx_text_fit(l->title_font, x, l->title_y, w, osm_str(S_NO_TRACK), C_WHITE, ALIGN_LEFT);
        return;
    }
    gfx_text_fit(l->title_font, x, l->title_y, w, t->title, C_WHITE, ALIGN_LEFT);
    gfx_text_fit(global_settings.osm_np_view == OSM_NP_VU ? OSM_FONT_R11 : OSM_FONT_R13,
                 x, l->artist_y, w,
                 t->artist[0] ? t->artist : osm_str(S_UNKNOWN_ARTIST), C_GOLD, ALIGN_LEFT);
    if (l->album_y >= 0)
        gfx_text_fit(OSM_FONT_R11, x, l->album_y, w, t->album, SILVER_A(0.7), ALIGN_LEFT);
}

static void draw_controls(const struct np_layout *l)
{
    int cy = l->controls_cy;
    bool sh = global_settings.playlist_shuffle;
    int rep = global_settings.repeat_mode;
    gfx_icon(ICON_SHUFFLE, 16, 22 - 8, cy - 8, sh ? C_GOLD : SILVER_A(0.6));
    gfx_icon(ICON_SKIP_BACK, 22, 70 - 11, cy - 11, C_SILVER);
    /* the gold disc with its glow (0 0 24px gold/40) */
    gfx_glow(SCR_W / 2, cy, 21, 10, GOLD_A(0.4));
    gfx_circle(SCR_W / 2, cy, 21, C_GOLD);
    if (player.playing)
        gfx_icon(ICON_PAUSE_FILL, 22, SCR_W / 2 - 11, cy - 11, C_BLACK);
    else
        gfx_icon(ICON_PLAY_FILL, 22, SCR_W / 2 - 11 + 2, cy - 11, C_BLACK);
    gfx_icon(ICON_SKIP_FORWARD, 22, SCR_W - 70 - 11, cy - 11, C_SILVER);
    gfx_icon(rep == REPEAT_ONE ? ICON_REPEAT_1 : ICON_REPEAT, 16, SCR_W - 22 - 8, cy - 8,
             rep != REPEAT_OFF ? C_GOLD : SILVER_A(0.6));
}

static void draw_volume(const struct np_layout *l)
{
    int v = player_volume_percent();
    int y = l->volume_y;
    gfx_icon(v == 0 ? ICON_VOLUME_X : ICON_VOLUME_2, 14, 16, y - 5, SILVER_A(0.7));
    int bx = 38, bw = SCR_W - bx - 44;
    gfx_rrect(bx, y, bw, 4, 2, WA(0.10));
    int kx = bx + bw * v / 100;
    gfx_circle(kx, y + 2, 5, C_GOLD);
    char b[8];
    snprintf(b, sizeof b, "%d", v);
    gfx_text(OSM_FONT_M10, SCR_W - 16 - gfx_text_width(OSM_FONT_M10, b), y - 5, b, SILVER_A(0.6));
}

static uint32_t state_key(void)
{
    struct tm *tm = get_time();
    uint32_t k = player.serial * 2654435761u;
    k ^= (player.playing << 1) | (player.have_cover << 2) | (player.have_backdrop << 3) |
         (global_settings.playlist_shuffle << 4) | (global_settings.repeat_mode << 5) |
         (global_settings.osm_np_view << 8) | (global_settings.osm_vu_skin << 10) |
         ((uint32_t)player_volume_percent() << 14) | ((uint32_t)tm->tm_min << 22) |
         ((uint32_t)player_sleep_minutes() << 28);
    k ^= (player.cur.valid && player_is_favorite(player.cur.path)) ? 0x80000000u : 0;
    k ^= battery_level() * 7919u;
    return k ? k : 1;
}

static void build_layer(void)
{
    struct np_layout l;
    layout(&l);
    gfx_noclip();
    draw_backdrop();
    draw_header();
    if (global_settings.osm_np_view == OSM_NP_VU)
    {
        const struct osm_vu_skin *s = vu_skin();
        /* the meters' faces (the needles and the glass move on top), and
         * the plate under the meters */
        gfx_image(&s->under, 0, 34, 256);
        draw_led_plate((SCR_W - osm_led_plate.w) / 2, 34 + s->h + 4);
    }
    else
        draw_cover();
    draw_texts(&l);
    draw_controls(&l);
    draw_volume(&l);
    gfx_snapshot(layer);
}

/* ── what moves ────────────────────────────────────────────────────── */

static void draw_vu(void)
{
    const struct osm_vu_skin *s = vu_skin();
    int y0 = 34;
    int levels[2];
    player_vu_levels(&levels[0], &levels[1]);
    for (int m = 0; m < 2; m++)
    {
        int a = s->angle_min + (s->angle_max - s->angle_min) * levels[m] / 1000;
        int px = s->meters[m][0], py = s->meters[m][1] + (y0 << 4);
        if (s->needle.w)
            gfx_image_rotated(&s->needle, s->pivot_x, s->pivot_y, px, py, a);
        else
        {
            long cosv;
            long sinv = fp_sincos((uint32_t)(int32_t)((long long)a * 4294967296LL / 3600), &cosv);
            int len = s->line_h;
            int ex = px + (int)(((long long)sinv * len) >> 31);
            int ey = py - (int)(((long long)cosv * len) >> 31);
            /* a soft shadow, then the needle */
            gfx_line16(px + 16, py, ex + 16, ey, s->line_w + 8, BLACK_A(0.3));
            uint16_t c = s->line_color;
            color_t col = 0xFF000000u | ((c >> 11) << 19) | (((c >> 5) & 63) << 10) | ((c & 31) << 3);
            gfx_line16(px, py, ex, ey, s->line_w, col);
            if (s->line_cap)
                gfx_circle(px >> 4, py >> 4, MAX(2, s->line_cap / 32), col);
        }
    }
    gfx_image(&s->over, 0, y0, 256);
}

static void draw_progress(const struct np_layout *l)
{
    const struct osm_track *t = &player.cur;
    int x = 16, w = SCR_W - 32;
    char a[16] = "0:00", b[16] = "0:00";
    if (t->valid)
    {
        ui_format_time(a, sizeof a, t->elapsed);
        ui_format_time(b, sizeof b, t->length);
    }
    int ty = l->times_y;
    gfx_text(OSM_FONT_M10, x, ty, a, SILVER_A(0.6));
    gfx_text(OSM_FONT_M10, x + w - gfx_text_width(OSM_FONT_M10, b), ty, b, SILVER_A(0.6));
    /* the format chip between the times */
    if (t->valid && t->chip[0])
    {
        int cw = MIN(gfx_text_width(OSM_FONT_R10, t->chip) + 12, w - 80);
        int cx = (SCR_W - cw) / 2;
        gfx_rrect(cx, ty - 2, cw, 16, 4, WA(0.05));
        gfx_rrect_border(cx, ty - 2, cw, 16, 4, 1, WA(0.05));
        gfx_text_fit(OSM_FONT_R10, cx + 6, ty, cw - 12, t->chip, SILVER_A(0.5), ALIGN_CENTER);
    }
    int by = l->bar_y;
    gfx_rrect(x, by, w, 4, 2, WA(0.10));
    if (t->valid && t->length)
    {
        int pw = (int)((long long)w * MIN(t->elapsed, t->length) / t->length);
        if (pw > 0)
        {
            /* the gold to yellow ramp, squeezed into the filled part */
            gfx_clip(x, by, pw, 4);
            gfx_rrect(x, by, MAX(pw, 4), 4, 2, C_GOLD);
            gfx_hgrad(x + 2, by, MAX(0, pw - 4), 4, C_GOLD, C_YELLOW400);
            gfx_noclip();
        }
    }
}

/* ── the queue (QueueDrawer.qml) ───────────────────────────────────── */

static struct qcache {
    int index;
    unsigned serial;
    char title[96];
    char artist[64];
} qcache[12];

static void queue_title(int index, char *title, int tlen, char *artist, int alen)
{
    struct qcache *c = &qcache[index % 12];
    if (c->index == index && c->serial == player.serial)
    {
        strmemccpy(title, c->title, tlen);
        strmemccpy(artist, c->artist, alen);
        return;
    }
    struct playlist_track_info info;
    title[0] = artist[0] = 0;
    if (playlist_get_track_info(NULL, index, &info) >= 0)
    {
        struct lib_item it = { .path = NULL };
        (void)it;
        int32_t idx = lib_index_of_path(info.filename);
        if (idx >= 0)
        {
            static struct tagcache_search tcs;
            if (tagcache_search(&tcs, tag_filename))
            {
                tagcache_retrieve(&tcs, idx, tag_title, title, tlen);
                tagcache_retrieve(&tcs, idx, tag_artist, artist, alen);
                tagcache_search_finish(&tcs);
            }
            if (!strcmp(title, "<Untagged>")) title[0] = 0;
            if (!strcmp(artist, "<Untagged>")) artist[0] = 0;
        }
        if (!title[0])
        {
            const char *base = strrchr(info.filename, '/');
            strip_extension(title, tlen, base ? base + 1 : info.filename);
        }
    }
    c->index = index;
    c->serial = player.serial;
    strmemccpy(c->title, title, sizeof c->title);
    strmemccpy(c->artist, artist, sizeof c->artist);
}

static void paint_queue_row(int i, int x, int y, int w, int h, bool sel, void *d)
{
    (void)d;
    char title[96], artist[64];
    queue_title(i, title, sizeof title, artist, sizeof artist);
    bool playing = i == playlist_get_display_index() - 1;
    ui_draw_row(x, y, w, h, sel, playing ? ICON_PLAY_FILL : ICON_COUNT, title, artist, NULL,
                false, playing);
}

static void draw_queue(void)
{
    /* a sheet over Now Playing */
    gfx_fill(0, 0, SCR_W, SCR_H, BLACK_A(0.6));
    int y = 26;
    gfx_rrect(0, y, SCR_W, SCR_H - y + 20, 18, C_PANEL);
    gfx_fill(0, y + 18, SCR_W, 1, 0x00000000u);
    gfx_rrect(SCR_W / 2 - 18, y + 6, 36, 4, 2, WA(0.15));
    gfx_icon(ICON_LIST_MUSIC, 16, 14, y + 16, C_GOLD);
    gfx_text(OSM_FONT_B15, 36, y + 15, osm_str(S_QUEUE), C_WHITE);
    char n[24];
    snprintf(n, sizeof n, osm_str(S_TRACK_COUNT), playlist_amount());
    gfx_text(OSM_FONT_R11, SCR_W - 14 - gfx_text_width(OSM_FONT_R11, n), y + 19, n, SILVER_A(0.6));
    queue_list.count = playlist_amount();
    if (queue_list.count == 0)
    {
        gfx_text_fit(OSM_FONT_R12, 16, y + 80, SCR_W - 32, osm_str(S_EMPTY_QUEUE),
                     SILVER_A(0.6), ALIGN_CENTER);
        return;
    }
    gfx_clip(queue_list.x, queue_list.y, queue_list.w, queue_list.h);
    list_draw(&queue_list, paint_queue_row, NULL);
    gfx_noclip();
    list_draw_scrollbar(&queue_list);
}

static void open_queue(void)
{
    list_init(&queue_list, playlist_amount(), 0, 26 + 44, SCR_W - 2, SCR_H - 26 - 50, 42);
    list_set_sel(&queue_list, MAX(0, playlist_get_display_index() - 1));
    for (int i = 0; i < 12; i++)
        qcache[i].index = -1;
    queue_open = true;
    osm_dirty();
}

static int queue_menu_index;
static void queue_menu_cb(int sel, void *d)
{
    (void)d;
    if (sel == 0)
    {
        /* play from here */
        int cur = playlist_get_display_index() - 1;
        int steps = queue_menu_index - cur;
        if (steps)
            audio_skip(steps);
    }
    else if (sel == 1)
    {
        struct playlist_track_info info;
        if (playlist_get_track_info(NULL, queue_menu_index, &info) >= 0)
        {
            playlist_delete(NULL, info.index);
            for (int i = 0; i < 12; i++)
                qcache[i].index = -1;
            osm_toast(ICON_TRASH_2, osm_str(S_REMOVE_FROM_QUEUE));
        }
    }
    else if (sel == 2)
    {
        if (osm_dialog_wait(osm_str(S_CLEAR_QUEUE), NULL, osm_str(S_YES), osm_str(S_NO)))
        {
            playlist_remove_all_tracks(NULL);
            for (int i = 0; i < 12; i++)
                qcache[i].index = -1;
        }
    }
    osm_dirty();
}

static bool queue_key(int key, int ev)
{
    if (list_key(&queue_list, key, ev))
    {
        osm_dirty();
        return true;
    }
    if (key == K_SELECT && ev == EV_SHORT && queue_list.count)
    {
        int cur = playlist_get_display_index() - 1;
        int steps = queue_list.sel - cur;
        if (steps)
            audio_skip(steps);
        else if (!player.playing)
            player_toggle_play();
        queue_open = false;
        osm_dirty();
        return true;
    }
    if ((key == K_SELECT || key == K_MENU) && ev == EV_LONG && queue_list.count)
    {
        static struct osm_menu_item m[3];
        m[0] = (struct osm_menu_item){ ICON_PLAY_FILL, osm_str(S_PLAY), false };
        m[1] = (struct osm_menu_item){ ICON_TRASH_2, osm_str(S_REMOVE_FROM_QUEUE), false };
        m[2] = (struct osm_menu_item){ ICON_X, osm_str(S_CLEAR_QUEUE), false };
        queue_menu_index = queue_list.sel;
        osm_menu(osm_str(S_QUEUE), m, 3, 0, queue_menu_cb, NULL);
        return true;
    }
    if ((key == K_BACK || key == K_LEFT || key == K_MENU) && ev == EV_SHORT)
    {
        queue_open = false;
        osm_dirty();
        return true;
    }
    return key != K_PLAY;       /* nothing else gets through */
}

/* ── the screen ────────────────────────────────────────────────────── */

static void np_draw(void)
{
    uint32_t key = state_key();
    if (key != layer_key)
    {
        build_layer();
        layer_key = key;
    }
    else
        gfx_restore(layer);
    struct np_layout l;
    layout(&l);
    if (global_settings.osm_np_view == OSM_NP_VU)
        draw_vu();
    draw_progress(&l);
    if (queue_open)
        draw_queue();
}

static bool np_animating(void)
{
    return global_settings.osm_np_view == OSM_NP_VU && !queue_open;
}

/* the menu of Now Playing */
enum { NPM_FAV, NPM_ALBUM, NPM_ARTIST, NPM_QUEUE, NPM_VIEW, NPM_SLEEP };
static int npm_acts[8];

static const int sleep_choices[] = { 0, 15, 30, 45, 60, 90, 120 };

static void sleep_cb(int sel, void *d)
{
    (void)d;
    if (sel < 0)
        return;
    player_set_sleep(sleep_choices[sel]);
    if (sleep_choices[sel])
    {
        char b[48];
        snprintf(b, sizeof b, osm_str(S_SLEEP_IN), sleep_choices[sel]);
        osm_toast(ICON_MOON, b);
    }
    else
        osm_toast(ICON_MOON, osm_str(S_SLEEP_OFF));
    layer_key = 0;
}

static void open_sleep_menu(void)
{
    static struct osm_menu_item m[ARRAYLEN(sleep_choices)];
    static char labels[ARRAYLEN(sleep_choices)][24];
    int cur = player_sleep_minutes(), sel = 0;
    for (unsigned i = 0; i < ARRAYLEN(sleep_choices); i++)
    {
        if (sleep_choices[i])
            snprintf(labels[i], sizeof labels[i], osm_str(S_MINUTES), sleep_choices[i]);
        else
            strmemccpy(labels[i], osm_str(S_SLEEP_OFF), sizeof labels[i]);
        m[i].icon = ICON_MOON;
        m[i].label = labels[i];
        m[i].checked = false;
    }
    if (!cur)
        m[0].checked = true;
    osm_menu(osm_str(S_SLEEP_TIMER), m, ARRAYLEN(sleep_choices), sel, sleep_cb, NULL);
}

static void np_menu_cb(int sel, void *d)
{
    (void)d;
    if (sel < 0)
        return;
    char album[160], artist[160];
    switch (npm_acts[sel])
    {
        case NPM_FAV:
        {
            bool now = player_toggle_favorite(player.cur.path);
            osm_toast(ICON_HEART, osm_str(now ? S_ADDED_TO_FAVORITES : S_REMOVED_FROM_FAVORITES));
            break;
        }
        case NPM_ALBUM:
            if (lib_album_of_track(player.cur.path, album, sizeof album, artist, sizeof artist))
            {
                osm_show_now_playing(false);
                album_open(album, artist);
            }
            break;
        case NPM_ARTIST:
            if (lib_album_of_track(player.cur.path, album, sizeof album, artist, sizeof artist)
                && artist[0])
            {
                struct lib_query q;
                memset(&q, 0, sizeof q);
                q.view = LIB_ARTIST_ALBUMS;
                strmemccpy(q.key, artist, sizeof q.key);
                osm_show_now_playing(false);
                browse_open(&q, artist);
            }
            break;
        case NPM_QUEUE:
            open_queue();
            break;
        case NPM_VIEW:
            global_settings.osm_np_view = !global_settings.osm_np_view;
            settings_save();
            break;
        case NPM_SLEEP:
            open_sleep_menu();
            break;
    }
    layer_key = 0;
    osm_dirty();
}

static void open_np_menu(void)
{
    static struct osm_menu_item m[8];
    int n = 0;
    if (player.cur.valid)
    {
        bool fav = player_is_favorite(player.cur.path);
        m[n] = (struct osm_menu_item){ fav ? ICON_HEART_FILL : ICON_HEART,
               osm_str(fav ? S_REMOVE_FROM_FAVORITES : S_ADD_TO_FAVORITES), false };
        npm_acts[n++] = NPM_FAV;
        if (lib_ready())
        {
            m[n] = (struct osm_menu_item){ ICON_DISC, osm_str(S_GO_TO_ALBUM), false };
            npm_acts[n++] = NPM_ALBUM;
            m[n] = (struct osm_menu_item){ ICON_USER, osm_str(S_GO_TO_ARTIST), false };
            npm_acts[n++] = NPM_ARTIST;
        }
    }
    m[n] = (struct osm_menu_item){ ICON_LIST_MUSIC, osm_str(S_QUEUE), false };
    npm_acts[n++] = NPM_QUEUE;
    bool vu = global_settings.osm_np_view == OSM_NP_VU;
    m[n] = (struct osm_menu_item){ vu ? ICON_DISC : ICON_VU_METER,
           osm_str(vu ? S_NP_VIEW_COVER : S_NP_VIEW_VU), false };
    npm_acts[n++] = NPM_VIEW;
    m[n] = (struct osm_menu_item){ ICON_MOON, osm_str(S_SLEEP_TIMER), player_sleep_minutes() > 0 };
    npm_acts[n++] = NPM_SLEEP;
    osm_menu(osm_str(S_NOW_PLAYING), m, n, 0, np_menu_cb, NULL);
}

static bool np_key(int key, int ev)
{
    if (queue_open)
        return queue_key(key, ev);

    switch (key)
    {
        case K_SELECT:
            if (ev == EV_SHORT)
                player_toggle_play();
            else if (ev == EV_LONG)
                open_np_menu();
            else
                return false;
            return true;

        case K_LEFT:
        case K_RIGHT:
        {
            int dir = key == K_RIGHT ? 1 : -1;
            if (ev == EV_PRESS)
            {
                seek_step = 0;
                return true;
            }
            if (ev == EV_SHORT)
            {
                if (dir > 0) player_next();
                else player_prev();
                return true;
            }
            if (ev == EV_LONG)
                seek_step = 2000;
            if (ev == EV_REPEAT && seek_step)
            {
                /* faster the longer it is held */
                player_seek_by(dir * seek_step);
                seek_step = MIN(seek_step + 500, 15000);
                osm_dirty();
                return true;
            }
            return ev != EV_PRESS;
        }

        case K_UP:
            if (ev == EV_PRESS)
                open_queue();
            return true;

        case K_DOWN:
            if (ev == EV_PRESS)
            {
                global_settings.osm_np_view = !global_settings.osm_np_view;
                settings_save();
                osm_dirty();
            }
            return true;

        case K_SCROLL_FWD:
        case K_SCROLL_BACK:
            if (global_settings.osm_wheel_volume)
            {
                player_volume_step(key == K_SCROLL_FWD ? 1 : -1);
                osm_toast_volume();
            }
            else
                player_seek_by(key == K_SCROLL_FWD ? 5000 : -5000);
            osm_dirty();
            return true;

        case K_MENU:
            if (ev == EV_LONG)
            {
                open_np_menu();
                return true;
            }
            return false;
    }
    if (ev == EV_SHORT || ev == EV_LONG)
        seek_step = 0;
    return false;
}

static void np_enter(void *arg)
{
    (void)arg;
    queue_open = false;
    layer_key = 0;
    seek_step = 0;
}

static void np_leave(void)
{
    queue_open = false;
}

const struct osm_screen scr_nowplaying = {
    .name = "nowplaying",
    .enter = np_enter,
    .leave = np_leave,
    .draw = np_draw,
    .key = np_key,
    .animating = np_animating,
};
