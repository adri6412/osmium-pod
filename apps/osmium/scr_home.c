/***************************************************************************
 *   Osmium Sound for FiiO M3K — the home screen
 *
 *   The right panel of Osmium's main screen on a portrait 240x320: the
 *   tab bar (Music, Discover, Settings), the nine tiles of the library home
 *   and, at the bottom, the mini player. The tab bar takes the focus from
 *   the first row of tiles going up; Left and Right then change tab.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "settings.h"
#include "audio.h"
#include "osm_ui.h"
#include "osm_screens.h"
#include "osm_player.h"
#include "osm_library.h"

enum { TAB_MUSIC, TAB_DISCOVER, TAB_SETTINGS, TAB_COUNT };

static int tab = TAB_MUSIC;
static bool tabs_focused;
static int tile_sel;

#define TABS_Y   STATUSBAR_H
#define TABS_H   30
#define BODY_Y   (TABS_Y + TABS_H)

static const struct {
    enum osm_icon icon;
    enum osm_str_id label;
    int view;
} tiles[9] = {
    { ICON_HEART,      S_FAVORITES, LIB_FAVORITES },
    { ICON_USER,       S_ARTISTS,   LIB_ARTISTS },
    { ICON_DISC,       S_ALBUMS,    LIB_ALBUMS },
    { ICON_TAG,        S_GENRES,    LIB_GENRES },
    { ICON_CALENDAR,   S_YEARS,     LIB_YEARS },
    { ICON_PIANO,      S_COMPOSERS, LIB_COMPOSERS },
    { ICON_SPARKLES,   S_NEW_MUSIC, LIB_NEW_ALBUMS },
    { ICON_FOLDER,     S_FOLDERS,   LIB_FOLDER },
    { ICON_LIST_MUSIC, S_PLAYLISTS, LIB_PLAYLISTS },
};

/* ── Discover ──────────────────────────────────────────────────────── */

static const struct {
    enum osm_icon icon;
    enum osm_str_id title, sub;
    int view;           /* -1: an action */
} mixes[] = {
    { ICON_INFINITY,    S_MIX_RANDOM, S_MIX_RANDOM_SUB, -1 },
    { ICON_DICES,       S_MIX_ALBUM,  S_MIX_ALBUM_SUB,  -2 },
    { ICON_TRENDING_UP, S_MIX_MOST,   S_MIX_MOST_SUB,   LIB_MOST_PLAYED },
    { ICON_HISTORY,     S_MIX_RECENT, S_MIX_RECENT_SUB, LIB_RECENTLY_PLAYED },
    { ICON_SPARKLES,    S_MIX_NEW,    S_MIX_NEW_SUB,    LIB_RECENTLY_ADDED },
    { ICON_SPROUT,      S_MIX_NEVER,  S_MIX_NEVER_SUB,  LIB_NEVER_PLAYED },
};
#define MIX_COUNT ((int)ARRAYLEN(mixes))
static struct osm_list mix_list;

/* ── drawing ───────────────────────────────────────────────────────── */

static void draw_tabs(void)
{
    static const struct { enum osm_icon icon; enum osm_str_id label; } t[TAB_COUNT] = {
        { ICON_MUSIC, S_TAB_MUSIC }, { ICON_COMPASS, S_TAB_DISCOVER }, { ICON_SETTINGS, S_TAB_SETTINGS },
    };
    gfx_fill(0, TABS_Y, SCR_W, TABS_H, C_PANEL);
    gfx_fill(0, TABS_Y + TABS_H - 1, SCR_W, 1, A(C_BORDER, 0.6));
    /* widths: icon + label, spread evenly */
    int widths[TAB_COUNT], total = 0;
    for (int i = 0; i < TAB_COUNT; i++)
    {
        widths[i] = 16 + 5 + gfx_text_width(OSM_FONT_R12, osm_str(t[i].label));
        total += widths[i];
    }
    int gap = (SCR_W - 16 - total) / TAB_COUNT;
    int x = 8 + gap / 2;
    for (int i = 0; i < TAB_COUNT; i++)
    {
        bool on = i == tab;
        color_t c = on ? C_WHITE : SILVER_A(0.55);
        if (on && tabs_focused)
            gfx_rrect(x - 6, TABS_Y + 3, widths[i] + 12, TABS_H - 7, 8, WA(0.08));
        gfx_icon(t[i].icon, 16, x, TABS_Y + 6, on ? C_WHITE : c);
        gfx_text(OSM_FONT_R12, x + 21, TABS_Y + 7, osm_str(t[i].label), c);
        if (on)
            gfx_fill(x - 2, TABS_Y + TABS_H - 3, widths[i] + 4, 2, C_GOLD);
        x += widths[i] + gap;
    }
}

static void tile_rect(int i, int *x, int *y, int *w, int *h)
{
    const int mx = 8, gap = 6;
    int area_h = CONTENT_BOTTOM - BODY_Y - 2 * gap;
    *w = (SCR_W - 2 * mx - 2 * gap) / 3;
    *h = (area_h - 2 * gap) / 3;
    *x = mx + (i % 3) * (*w + gap);
    *y = BODY_Y + gap + (i / 3) * (*h + gap);
}

static void draw_music(void)
{
    for (int i = 0; i < 9; i++)
    {
        int x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        bool sel = !tabs_focused && i == tile_sel;
        /* bg-hifi-gray, border hifi-border, rounded-xl (Browser.qml) */
        gfx_rrect(x, y, w, h, 12, C_GRAY);
        if (sel)
            ui_draw_focus(x, y, w, h, 12);
        else
            gfx_rrect_border(x, y, w, h, 12, 1, C_BORDER);
        color_t c = sel ? C_GOLD : C_WHITE;
        gfx_icon(tiles[i].icon, 26, x + (w - 26) / 2, y + h / 2 - 22, c);
        gfx_text_fit(OSM_FONT_R11, x + 2, y + h / 2 + 9, w - 4, osm_str(tiles[i].label),
                     sel ? C_GOLD : C_WHITE, ALIGN_CENTER);
    }
}

static void paint_mix(int i, int x, int y, int w, int h, bool sel, void *d)
{
    (void)d;
    int cx = x + 6, cw = w - 12;
    if (sel)
        ui_draw_focus(cx, y + 2, cw, h - 4, 12);
    else
    {
        gfx_rrect(cx, y + 2, cw, h - 4, 12, C_SURFACE);
        gfx_rrect_border(cx, y + 2, cw, h - 4, 12, 1, C_BORDER);
    }
    /* the gold-tinted icon box of the settings rows */
    gfx_rrect(cx + 10, y + (h - 30) / 2, 30, 30, 8, GOLD_A(0.15));
    gfx_icon(mixes[i].icon, 18, cx + 16, y + (h - 18) / 2, C_GOLD);
    int tx = cx + 50, tw = cw - 50 - 26;
    gfx_text_fit(OSM_FONT_R13, tx, y + h / 2 - 16, tw, osm_str(mixes[i].title), C_WHITE, ALIGN_LEFT);
    gfx_text_fit(OSM_FONT_R11, tx, y + h / 2 + 1, tw, osm_str(mixes[i].sub), SILVER_A(0.6), ALIGN_LEFT);
    gfx_icon(ICON_CHEVRON_RIGHT, 14, cx + cw - 22, y + (h - 14) / 2, SILVER_A(0.5));
}

static void draw_discover(void)
{
    gfx_text(OSM_FONT_B18, 14, BODY_Y + 8, osm_str(S_DISCOVER_TITLE), C_WHITE);
    gfx_text_fit(OSM_FONT_R11, 14, BODY_Y + 32, SCR_W - 28, osm_str(S_DISCOVER_SUB),
                 SILVER_A(0.7), ALIGN_LEFT);
    mix_list.count = MIX_COUNT;
    mix_list.x = 0;
    mix_list.y = BODY_Y + 52;
    mix_list.w = SCR_W;
    mix_list.h = CONTENT_BOTTOM - mix_list.y - 2;
    mix_list.row_h = 50;
    if (tabs_focused)
    {
        /* no focus ring while the tab bar has it */
        int keep = mix_list.sel;
        mix_list.sel = -1;
        gfx_clip(0, mix_list.y, SCR_W, mix_list.h);
        list_draw(&mix_list, paint_mix, NULL);
        mix_list.sel = keep;
    }
    else
    {
        gfx_clip(0, mix_list.y, SCR_W, mix_list.h);
        list_draw(&mix_list, paint_mix, NULL);
    }
    gfx_noclip();
    list_draw_scrollbar(&mix_list);
}

static void home_draw(void)
{
    gfx_clear(C_DARK);
    ui_draw_statusbar(true, NULL);
    draw_tabs();
    gfx_clip(0, BODY_Y, SCR_W, CONTENT_BOTTOM - BODY_Y);
    switch (tab)
    {
        case TAB_MUSIC:    draw_music(); break;
        case TAB_DISCOVER: draw_discover(); break;
        case TAB_SETTINGS: settings_home_draw(BODY_Y); break;
    }
    gfx_noclip();
    ui_draw_mini_player();
}

/* ── actions ───────────────────────────────────────────────────────── */

static void open_tile(int i)
{
    struct lib_query q;
    memset(&q, 0, sizeof q);
    q.view = tiles[i].view;
    if (q.view == LIB_FOLDER)
        strcpy(q.key, "/");
    browse_open(&q, osm_str(tiles[i].label));
}

static void start_mix(int i)
{
    struct lib_query q;
    memset(&q, 0, sizeof q);
    if (mixes[i].view == -1)
    {
        /* everything, shuffled */
        q.view = LIB_ALL_TRACKS;
        if (lib_load(&q) > 0 && player_play_fill(lib_fill, NULL, -1, true))
            osm_show_now_playing(true);
        else
            osm_toast(ICON_INFO, osm_str(S_LIBRARY_NOT_READY));
        return;
    }
    if (mixes[i].view == -2)
    {
        /* one album at random, in order */
        q.view = LIB_ALBUMS;
        int n = lib_load(&q);
        if (n <= 0)
        {
            osm_toast(ICON_INFO, osm_str(S_LIBRARY_NOT_READY));
            return;
        }
        const struct lib_item *it = &lib_items[rand() % n];
        struct lib_query a;
        memset(&a, 0, sizeof a);
        a.view = LIB_ALBUM;
        strmemccpy(a.key, it->name[0] == '\x7f' ? "<Untagged>" : it->name, sizeof a.key);
        strmemccpy(a.key2, it->sub ? it->sub : "", sizeof a.key2);
        if (lib_load(&a) > 0 && player_play_fill(lib_fill, NULL, 0, false))
            osm_show_now_playing(true);
        return;
    }
    q.view = mixes[i].view;
    browse_open(&q, osm_str(mixes[i].title));
}

static void set_tab(int t)
{
    tab = (t + TAB_COUNT) % TAB_COUNT;
    if (tab == TAB_SETTINGS)
        settings_home_enter();
    osm_dirty();
}

static bool home_key(int key, int ev)
{
    bool move = ev == EV_PRESS || ev == EV_REPEAT;
    if (tabs_focused)
    {
        if (move && (key == K_LEFT || key == K_SCROLL_BACK))
            set_tab(tab - 1);
        else if (move && (key == K_RIGHT || key == K_SCROLL_FWD))
            set_tab(tab + 1);
        else if ((move && key == K_DOWN) || (ev == EV_SHORT && key == K_SELECT))
            tabs_focused = false;
        else
            return key != K_MENU && key != K_PLAY && key != K_BACK;
        osm_dirty();
        return true;
    }

    switch (tab)
    {
        case TAB_MUSIC:
            if (move)
            {
                int n = tile_sel;
                switch (key)
                {
                    case K_UP:
                        if (n < 3) { tabs_focused = true; osm_dirty(); return true; }
                        n -= 3;
                        break;
                    case K_DOWN:        n = MIN(8, n + 3); break;
                    case K_LEFT:
                        if (n % 3 == 0) { set_tab(tab - 1); return true; }
                        n--;
                        break;
                    case K_RIGHT:
                        if (n % 3 == 2) { set_tab(tab + 1); return true; }
                        n++;
                        break;
                    case K_SCROLL_FWD:  n = (n + 1) % 9; break;
                    case K_SCROLL_BACK: n = (n + 8) % 9; break;
                    default: return false;
                }
                tile_sel = n;
                osm_dirty();
                return true;
            }
            if (ev == EV_SHORT && key == K_SELECT)
            {
                open_tile(tile_sel);
                return true;
            }
            return false;

        case TAB_DISCOVER:
            if (move && key == K_UP && mix_list.sel == 0)
            {
                tabs_focused = true;
                osm_dirty();
                return true;
            }
            if (move && (key == K_LEFT || key == K_RIGHT))
            {
                set_tab(tab + (key == K_LEFT ? -1 : 1));
                return true;
            }
            if (list_key(&mix_list, key, ev))
            {
                osm_dirty();
                return true;
            }
            if (ev == EV_SHORT && key == K_SELECT)
            {
                start_mix(mix_list.sel);
                return true;
            }
            return false;

        case TAB_SETTINGS:
            if (move && (key == K_LEFT || key == K_RIGHT))
            {
                set_tab(tab + (key == K_LEFT ? -1 : 1));
                return true;
            }
            if (settings_home_key(key, ev))
                return true;
            if (move && key == K_UP)
            {
                tabs_focused = true;
                osm_dirty();
                return true;
            }
            return false;
    }
    return false;
}

static void home_enter(void *arg)
{
    (void)arg;
    list_init(&mix_list, MIX_COUNT, 0, BODY_Y + 52, SCR_W, 160, 50);
}

/* back home (Back held): the Music tab, as after power on */
void home_reset(void)
{
    tab = TAB_MUSIC;
    tabs_focused = false;
    tile_sel = 0;
    osm_dirty();
}

const struct osm_screen scr_home = {
    .name = "home",
    .enter = home_enter,
    .draw = home_draw,
    .key = home_key,
    .shows_mini_player = true,
};
