/***************************************************************************
 *   Osmium Sound for FiiO M3K — browsing the library
 *
 *   LibraryList.qml, AlbumPage.qml and the album grid of Osmium on the
 *   M3K: rows as dark cards, the albums as a grid of covers, an album page
 *   with its cover, Play and Shuffle and the tracks; a long press (Select
 *   or Menu) opens the menu of the item.
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
#include "settings.h"
#include "audio.h"
#include "playlist.h"
#include "tagcache.h"
#include "string-extra.h"
#include "osm_ui.h"
#include "osm_screens.h"
#include "osm_player.h"
#include "osm_library.h"

#define LEVELS 12

/* one entry per library page on the stack */
static struct page {
    bool album;                 /* an album page (else a list) */
    struct lib_query q;
    char title[96];
    struct osm_list list;
    int  extra;                 /* synthetic rows before the items ("All tracks") */
    bool grid;
    int  serial;                /* library serial it was loaded with */
    int  count;
    /* album page */
    int  focus;                 /* -2 Play, -1 Shuffle, >= 0 a track */
    int  scroll;
    int32_t cover_idx;
    long total_ms;
} pages[LEVELS];
static int level = -1;          /* the page on top */
static int loaded_level = -1;   /* whose list is in lib_items */

static int32_t playing_idx = -1;
static unsigned playing_serial = ~0u;

/* the jump letter while scrolling fast */
static long jump_until;

#define LIST_TOP   (STATUSBAR_H + CRUMBS_H)
#define LIST_H     (CONTENT_BOTTOM - LIST_TOP)

static bool is_album_view(int v)
{
    return v == LIB_ALBUMS || v == LIB_NEW_ALBUMS || v == LIB_ARTIST_ALBUMS ||
           v == LIB_GENRE_ALBUMS || v == LIB_YEAR_ALBUMS || v == LIB_COMPOSER_ALBUMS;
}

static const char *display_name(const struct lib_item *it)
{
    if (it->name[0] == '\x7f')
        return osm_str(it->kind == IT_ALBUM ? S_UNKNOWN_ALBUM : S_UNKNOWN_ARTIST);
    return it->name;
}

static const char *album_key(const char *name)
{
    return name[0] == '\x7f' ? UNTAGGED : name;
}

/* make sure lib_items holds the list of page p */
static void ensure_loaded(int p)
{
    struct page *pg = &pages[p];
    if (loaded_level == p && pg->serial == osm_library_serial())
        return;
    int n = lib_load(&pg->q);
    pg->count = n < 0 ? 0 : n;
    pg->serial = osm_library_serial();
    loaded_level = p;
    int sel = pg->list.sel;
    pg->list.count = pg->count + pg->extra;
    if (!pg->album)
        list_set_sel(&pg->list, sel);
    if (pg->album)
    {
        pg->total_ms = 0;
        for (int i = 0; i < pg->count; i++)
            pg->total_ms += lib_items[i].num;
        pg->cover_idx = pg->count ? lib_items[0].idx : -1;
        if (pg->focus >= pg->count)
            pg->focus = pg->count - 1;
    }
}

static void update_playing(void)
{
    if (playing_serial == player.serial)
        return;
    playing_serial = player.serial;
    playing_idx = player.cur.valid ? lib_index_of_path(player.cur.path) : -1;
}

static bool is_playing_item(const struct lib_item *it)
{
    if (!player.cur.valid)
        return false;
    if (it->path)
        return !strcmp(it->path, player.cur.path);
    return it->idx >= 0 && it->idx == playing_idx;
}

/* ── crumbs ────────────────────────────────────────────────────────── */

static void draw_crumbs(void)
{
    const char *parts[LEVELS];
    for (int i = 0; i <= level; i++)
        parts[i] = pages[i].title;
    ui_draw_crumbs(parts, level + 1);
}

/* ── list rows ─────────────────────────────────────────────────────── */

static enum osm_icon kind_icon(int kind)
{
    switch (kind)
    {
        case IT_ARTIST:     return ICON_USER;
        case IT_ALBUM:      return ICON_DISC;
        case IT_GENRE:      return ICON_TAG;
        case IT_YEAR:       return ICON_CALENDAR;
        case IT_COMPOSER:   return ICON_PIANO;
        case IT_TRACK:      return ICON_MUSIC;
        case IT_DIR:        return ICON_FOLDER;
        case IT_FILE:       return ICON_MUSIC;
        case IT_PLAYLIST:   return ICON_LIST_MUSIC;
        case IT_ALL_TRACKS: return ICON_LIST_MUSIC;
    }
    return ICON_MUSIC;
}

static void draw_thumb(int32_t idx, int x, int y, int size, int radius)
{
    int w, h;
    const uint16_t *px = lib_thumb(idx, &w, &h);
    if (px)
    {
        /* letterboxed if it is not square */
        int dw = size, dh = size;
        if (w > h) dh = size * h / w;
        else if (h > w) dw = size * w / h;
        gfx_rrect(x, y, size, size, radius, C_BLACK);
        gfx_bitmap(px, w, h, x + (size - dw) / 2, y + (size - dh) / 2, dw, dh, radius, 256);
    }
    else
    {
        gfx_rrect(x, y, size, size, radius, C_LIGHT);
        int is = size >= 80 ? 44 : size >= 30 ? 16 : 14;
        gfx_icon(ICON_DISC, is, x + (size - is) / 2, y + (size - is) / 2, SILVER_A(0.35));
    }
}

static void paint_row(int i, int x, int y, int w, int h, bool sel, void *d)
{
    struct page *pg = d;
    if (i < pg->extra)
    {
        ui_draw_row(x, y, w, h, sel, ICON_LIST_MUSIC, osm_str(S_ALL_TRACKS), NULL, NULL, true, false);
        return;
    }
    const struct lib_item *it = &lib_items[i - pg->extra];
    char right[24] = "";
    const char *sub = it->sub;
    bool chevron = false;
    switch (it->kind)
    {
        case IT_ARTIST: case IT_GENRE: case IT_COMPOSER: case IT_DIR:
            chevron = true;
            break;
        case IT_YEAR:
            snprintf(right, sizeof right, "%d", (int)it->num2);
            chevron = true;
            break;
        case IT_PLAYLIST:
            chevron = true;
            break;
        case IT_TRACK: case IT_FILE:
            if (it->num > 0)
                ui_format_time(right, sizeof right, it->num);
            break;
        case IT_ALBUM:
        {
            /* a small cover instead of the icon */
            int cs = h - 10;
            bool playing = false;
            int cx = x + 6, cw = w - 12;
            if (sel)
                ui_draw_focus(cx, y + 1, cw, h - 3, 10);
            else
                gfx_rrect(cx, y + 1, cw, h - 3, 10, C_SURFACE);
            draw_thumb(it->idx, cx + 5, y + 4, cs - 2, 6);
            int tx = cx + cs + 10, tw = cw - cs - 10 - 26;
            gfx_text_fit(OSM_FONT_R13, tx, y + h / 2 - 15, tw, display_name(it),
                         playing ? C_GOLD : C_WHITE, ALIGN_LEFT);
            gfx_text_fit(OSM_FONT_R11, tx, y + h / 2 + 1, tw,
                         it->sub && it->sub[0] ? it->sub : osm_str(S_UNKNOWN_ARTIST),
                         SILVER_A(0.6), ALIGN_LEFT);
            gfx_icon(ICON_CHEVRON_RIGHT, 14, cx + cw - 22, y + (h - 14) / 2, SILVER_A(0.5));
            return;
        }
    }
    ui_draw_row(x, y, w, h, sel, kind_icon(it->kind), display_name(it), sub, right,
                chevron, is_playing_item(it));
}

/* a cell of the album grid (the grid of Browser.qml: cover, title, artist) */
static void paint_cell(int i, int x, int y, int w, int h, bool sel, void *d)
{
    struct page *pg = d;
    int pad = 5;
    int cs = w - 2 * pad;
    int cx = x + pad, cy = y + 4;
    if (sel)
        ui_draw_focus(cx - 3, cy - 3, cs + 6, h - 2, 12);
    if (i < pg->extra)
    {
        gfx_rrect(cx, cy, cs, cs, 10, C_GRAY);
        gfx_icon(ICON_LIST_MUSIC, 26, cx + (cs - 26) / 2, cy + (cs - 26) / 2, sel ? C_GOLD : C_WHITE);
        gfx_text_fit(OSM_FONT_R11, cx, cy + cs + 5, cs, osm_str(S_ALL_TRACKS),
                     sel ? C_GOLD : C_WHITE, ALIGN_LEFT);
        return;
    }
    const struct lib_item *it = &lib_items[i - pg->extra];
    draw_thumb(it->idx, cx, cy, cs, 10);
    gfx_text_fit(OSM_FONT_R11, cx, cy + cs + 4, cs, display_name(it),
                 sel ? C_GOLD : C_WHITE, ALIGN_LEFT);
    gfx_text_fit(OSM_FONT_R10, cx, cy + cs + 18, cs,
                 it->sub && it->sub[0] ? it->sub : osm_str(S_UNKNOWN_ARTIST),
                 SILVER_A(0.55), ALIGN_LEFT);
}

static void draw_empty(const char *msg, const char *detail)
{
    int y = LIST_TOP + 50;
    gfx_circle(SCR_W / 2, y + 20, 26, WA(0.05));
    gfx_icon(ICON_MUSIC, 20, SCR_W / 2 - 10, y + 10, SILVER_A(0.5));
    gfx_text_wrap(OSM_FONT_R12, 24, y + 60, SCR_W - 48, 4, 17, msg, SILVER_A(0.6));
    if (detail)
        gfx_text_wrap(OSM_FONT_R11, 24, y + 104, SCR_W - 48, 3, 15, detail, C_GOLD);
}

/* how far the database build has got, for the empty page */
static const char *library_progress(char *buf, int len)
{
    struct tagcache_stat *st = tagcache_get_stat();
    if (!st)
        return NULL;
    if (st->commit_step > 0)
        snprintf(buf, len, osm_str(S_LIB_SAVE_PROGRESS), st->commit_step,
                 tagcache_get_max_commit_step());
    else if (st->processed_entries > 0)
        snprintf(buf, len, osm_str(S_LIB_SCAN_PROGRESS), st->processed_entries);
    else
        return osm_str(S_LIB_FIRST_TIME);
    return buf;
}

static void browse_draw(void)
{
    struct page *pg = &pages[level];
    ensure_loaded(level);
    update_playing();
    gfx_clear(C_DARK);
    ui_draw_statusbar(true, NULL);
    draw_crumbs();
    if (pg->list.count == 0)
    {
        bool db = pg->q.view != LIB_FOLDER && pg->q.view != LIB_PLAYLISTS &&
                  pg->q.view != LIB_PLAYLIST && pg->q.view != LIB_FAVORITES;
        struct tagcache_stat *st = tagcache_get_stat();
        char progress[64];
        bool preparing = db && !lib_ready();
        draw_empty(osm_str(preparing && st && st->commit_delayed ? S_LIB_RESTART :
                           preparing ? S_LIBRARY_NOT_READY :
                           pg->q.view == LIB_ALBUMS || pg->q.view == LIB_ARTISTS ?
                           S_LIBRARY_EMPTY : S_EMPTY),
                   preparing && !(st && st->commit_delayed) ?
                   library_progress(progress, sizeof progress) : NULL);
    }
    else
    {
        gfx_clip(0, LIST_TOP, SCR_W, LIST_H);
        list_draw(&pg->list, pg->grid ? paint_cell : paint_row, pg);
        gfx_noclip();
        list_draw_scrollbar(&pg->list);
        if (jump_until && TIME_BEFORE(current_tick, jump_until) && pg->list.sel >= pg->extra)
        {
            char letter[8];
            lib_initial(lib_items[pg->list.sel - pg->extra].name, letter);
            ui_draw_jump_letter(letter);
        }
    }
    ui_draw_mini_player();
}

static bool browse_animating(void)
{
    if (jump_until && TIME_AFTER(current_tick, jump_until))
    {
        jump_until = 0;
        osm_dirty();
    }
    return jump_until != 0;
}

/* ── actions ───────────────────────────────────────────────────────── */

static void push_query(int view, const char *key, const char *key2, int num, const char *title)
{
    struct lib_query q;
    memset(&q, 0, sizeof q);
    q.view = view;
    if (key) strmemccpy(q.key, key, sizeof q.key);
    if (key2) strmemccpy(q.key2, key2, sizeof q.key2);
    q.num = num;
    browse_open(&q, title);
}

static void open_item(struct page *pg, int i)
{
    if (i < pg->extra)
    {
        /* "All tracks" of an artist */
        push_query(LIB_ARTIST_TRACKS, pg->q.key, NULL, 0, osm_str(S_ALL_TRACKS));
        return;
    }
    const struct lib_item *it = &lib_items[i - pg->extra];
    switch (it->kind)
    {
        case IT_ARTIST:
            push_query(LIB_ARTIST_ALBUMS, it->name, NULL, 0, display_name(it));
            break;
        case IT_GENRE:
            push_query(LIB_GENRE_ALBUMS, it->name, NULL, 0, it->name);
            break;
        case IT_COMPOSER:
            push_query(LIB_COMPOSER_ALBUMS, it->name, NULL, 0, it->name);
            break;
        case IT_YEAR:
            push_query(LIB_YEAR_ALBUMS, NULL, NULL, it->num, it->name);
            break;
        case IT_ALBUM:
            album_open(it->name, it->sub);
            break;
        case IT_DIR:
            push_query(LIB_FOLDER, it->path, NULL, 0, it->name);
            break;
        case IT_PLAYLIST:
            push_query(LIB_PLAYLIST, it->path, NULL, 0, it->name);
            break;
        case IT_FILE:
            if (pg->q.view == LIB_FOLDER)
            {
                player_play_folder_from(it->path);
                break;
            }
            /* fall through: a playlist, the favourites */
        case IT_TRACK:
            player_play_fill(lib_fill, NULL, lib_track_position(i - pg->extra),
                             global_settings.playlist_shuffle);
            break;
    }
}

static int fill_dir(struct playlist_insert_context *ctx, void *data)
{
    (void)ctx;
    /* folders are added by the playlist itself (recursive) */
    return playlist_insert_directory(NULL, data, PLAYLIST_INSERT_LAST, false, true, NULL) >= 0;
}

/* the tracks an item stands for, loaded into lib_items */
static bool load_tracks_of(const struct lib_item *it, const char *artist_key)
{
    struct lib_query q;
    memset(&q, 0, sizeof q);
    switch (it->kind)
    {
        case IT_ARTIST:   q.view = LIB_ARTIST_TRACKS; strmemccpy(q.key, it->name, sizeof q.key); break;
        case IT_GENRE:    q.view = LIB_GENRE_TRACKS; strmemccpy(q.key, it->name, sizeof q.key); break;
        case IT_COMPOSER: q.view = LIB_COMPOSER_TRACKS; strmemccpy(q.key, it->name, sizeof q.key); break;
        case IT_YEAR:     q.view = LIB_YEAR_TRACKS; q.num = it->num; break;
        case IT_ALBUM:
            q.view = LIB_ALBUM;
            strmemccpy(q.key, album_key(it->name), sizeof q.key);
            strmemccpy(q.key2, it->sub ? it->sub : "", sizeof q.key2);
            break;
        case IT_ALL_TRACKS:
            q.view = LIB_ARTIST_TRACKS;
            strmemccpy(q.key, artist_key ? artist_key : "", sizeof q.key);
            break;
        case IT_PLAYLIST:
            q.view = LIB_PLAYLIST;
            strmemccpy(q.key, it->path, sizeof q.key);
            break;
        default:
            return false;
    }
    loaded_level = -1;          /* the page's own list is gone: reload later */
    return lib_load(&q) > 0;
}

void browse_play_item(const struct lib_item *it, bool shuffle)
{
    if (it->kind == IT_DIR)
    {
        if (playlist_create(it->path, NULL) >= 0)
        {
            playlist_insert_directory(NULL, it->path, PLAYLIST_INSERT_LAST, false, true, NULL);
            global_settings.playlist_shuffle = shuffle;
            int start = shuffle && playlist_amount() > 0 ?
                        playlist_shuffle(current_tick, rand() % playlist_amount()) : 0;
            playlist_start(start, 0, 0);
        }
        return;
    }
    if (it->kind == IT_TRACK || it->kind == IT_FILE)
    {
        /* one track: just it */
        char path[MAX_PATH];
        const char *p[1] = { path };
        if (lib_item_path(it, path, sizeof path))
            player_play_files(p, 1, 0, false);
        return;
    }
    if (load_tracks_of(it, NULL))
        player_play_fill(lib_fill, NULL, shuffle ? -1 : 0, shuffle);
}

/* ── the item menu ─────────────────────────────────────────────────── */

enum { ACT_PLAY, ACT_SHUFFLE, ACT_NEXT, ACT_QUEUE, ACT_FAV, ACT_ALBUM, ACT_ARTIST };
static int menu_acts[OSM_MENU_MAX];
static struct lib_item menu_item;
static char menu_name[160], menu_sub[160], menu_path[MAX_PATH];
static int menu_index;          /* in lib_items, for single tracks */

static void item_menu_cb(int sel, void *data)
{
    (void)data;
    if (sel < 0)
        return;
    struct lib_item *it = &menu_item;
    int act = menu_acts[sel];
    bool is_track = it->kind == IT_TRACK || it->kind == IT_FILE;
    switch (act)
    {
        case ACT_PLAY:
        case ACT_SHUFFLE:
            browse_play_item(it, act == ACT_SHUFFLE);
            osm_show_now_playing(true);
            break;
        case ACT_NEXT:
        case ACT_QUEUE:
        {
            bool ok;
            if (is_track)
            {
                const char *p[1] = { menu_path };
                ok = player_queue_files(p, 1, act == ACT_NEXT);
            }
            else if (it->kind == IT_DIR)
                ok = player_queue_fill(fill_dir, (void *)it->path, act == ACT_NEXT);
            else
                ok = load_tracks_of(it, NULL) && player_queue_fill(lib_fill, NULL, act == ACT_NEXT);
            if (ok)
                osm_toast(act == ACT_NEXT ? ICON_LIST_START : ICON_LIST_PLUS,
                          osm_str(act == ACT_NEXT ? S_PLAYING_NEXT : S_QUEUED));
            break;
        }
        case ACT_FAV:
        {
            bool now = player_toggle_favorite(menu_path);
            osm_toast(ICON_HEART, osm_str(now ? S_ADDED_TO_FAVORITES : S_REMOVED_FROM_FAVORITES));
            if (level >= 0 && pages[level].q.view == LIB_FAVORITES)
                loaded_level = -1;
            break;
        }
        case ACT_ALBUM:
        {
            char album[160], artist[160];
            if (lib_album_of_track(menu_path, album, sizeof album, artist, sizeof artist))
                album_open(album, artist);
            break;
        }
        case ACT_ARTIST:
        {
            char album[160], artist[160];
            if (lib_album_of_track(menu_path, album, sizeof album, artist, sizeof artist) && artist[0])
                push_query(LIB_ARTIST_ALBUMS, artist, NULL, 0, artist);
            break;
        }
    }
    (void)menu_index;
    osm_dirty();
}

void browse_item_menu(const struct lib_item *it, const char *title)
{
    struct osm_menu_item m[OSM_MENU_MAX];
    int n = 0;
    menu_item = *it;
    strmemccpy(menu_name, it->name, sizeof menu_name);
    strmemccpy(menu_sub, it->sub ? it->sub : "", sizeof menu_sub);
    menu_item.name = menu_name;
    menu_item.sub = menu_sub;
    menu_path[0] = 0;
    if (it->path)
    {
        strmemccpy(menu_path, it->path, sizeof menu_path);
        menu_item.path = menu_path;
    }
    bool is_track = it->kind == IT_TRACK || it->kind == IT_FILE;
    if (is_track && !menu_path[0])
        lib_item_path(it, menu_path, sizeof menu_path);

#define ADD(ic, s, a) do { m[n].icon = ic; m[n].label = osm_str(s); m[n].checked = false; menu_acts[n++] = a; } while (0)
    ADD(ICON_PLAY_FILL, S_PLAY, ACT_PLAY);
    if (!is_track)
        ADD(ICON_SHUFFLE, S_SHUFFLE, ACT_SHUFFLE);
    ADD(ICON_LIST_START, S_PLAY_NEXT, ACT_NEXT);
    ADD(ICON_LIST_PLUS, S_ADD_TO_QUEUE, ACT_QUEUE);
    if (is_track && menu_path[0])
    {
        bool fav = player_is_favorite(menu_path);
        ADD(fav ? ICON_HEART_FILL : ICON_HEART,
            fav ? S_REMOVE_FROM_FAVORITES : S_ADD_TO_FAVORITES, ACT_FAV);
        if (lib_ready() && it->idx >= 0)
        {
            ADD(ICON_DISC, S_GO_TO_ALBUM, ACT_ALBUM);
            ADD(ICON_USER, S_GO_TO_ARTIST, ACT_ARTIST);
        }
    }
#undef ADD
    osm_menu(title, m, n, 0, item_menu_cb, NULL);
}

/* ── keys ──────────────────────────────────────────────────────────── */

static bool browse_key(int key, int ev)
{
    struct page *pg = &pages[level];
    ensure_loaded(level);
    if (list_key(&pg->list, key, ev))
    {
        /* fast through a long list: the letter in the middle */
        if (pg->list.speed > 6 && pg->list.count > 40 && !pg->grid)
            jump_until = current_tick + HZ / 2;
        osm_dirty();
        return true;
    }
    if (pg->list.count == 0)
        return false;
    int i = pg->list.sel;
    if ((key == K_SELECT && ev == EV_SHORT) || (key == K_RIGHT && ev == EV_SHORT && !pg->grid))
    {
        open_item(pg, i);
        osm_dirty();
        return true;
    }
    if ((key == K_SELECT || key == K_MENU) && ev == EV_LONG)
    {
        if (i < pg->extra)
        {
            struct lib_item all = { .name = osm_str(S_ALL_TRACKS), .kind = IT_ALL_TRACKS, .idx = -1 };
            /* the artist's tracks */
            struct lib_item art = { .name = pg->q.key, .kind = IT_ARTIST, .idx = -1 };
            (void)all;
            browse_item_menu(&art, osm_str(S_ALL_TRACKS));
        }
        else
            browse_item_menu(&lib_items[i - pg->extra], display_name(&lib_items[i - pg->extra]));
        return true;
    }
    if (key == K_MENU && ev == EV_SHORT)
        return false;                   /* Now Playing */
    if (key == K_LEFT && ev == EV_SHORT && !pg->grid)
    {
        osm_pop();
        return true;
    }
    return false;
}

static void browse_enter(void *arg)
{
    (void)arg;
}

static void browse_leave(void)
{
    if (level >= 0)
        level--;
    loaded_level = -1;
}

static void browse_resume(void)
{
    /* the list under us was replaced: reload it, keep the selection */
    loaded_level = -1;
}

void browse_open(const struct lib_query *q, const char *title)
{
    if (level + 1 >= LEVELS)
        return;
    struct page *pg = &pages[++level];
    memset(pg, 0, sizeof(*pg));
    pg->q = *q;
    strmemccpy(pg->title, title, sizeof pg->title);
    pg->extra = q->view == LIB_ARTIST_ALBUMS ? 1 : 0;
    pg->grid = is_album_view(q->view) && global_settings.osm_album_view == 0;
    if (pg->grid)
    {
        list_init(&pg->list, 0, 2, LIST_TOP + 2, SCR_W - 6, LIST_H - 2, 0);
        pg->list.cols = 2;
        pg->list.row_h = (pg->list.w / 2) - 10 + 4 + 34;
    }
    else
        list_init(&pg->list, 0, 0, LIST_TOP + 4, SCR_W - 2, LIST_H - 4,
                  (is_album_view(q->view) ? 46 : 42));
    loaded_level = -1;
    ensure_loaded(level);
    osm_push(&scr_browse, NULL);
}

const struct osm_screen scr_browse = {
    .name = "browse",
    .enter = browse_enter,
    .resume = browse_resume,
    .leave = browse_leave,
    .draw = browse_draw,
    .key = browse_key,
    .animating = browse_animating,
    .shows_mini_player = true,
};

/* ── the album page ────────────────────────────────────────────────── */

#define AP_HEADER  112
#define AP_BUTTONS 46
#define AP_LABEL   24
#define AP_ROW     38

static int ap_track_y(int i)
{
    return AP_HEADER + AP_BUTTONS + AP_LABEL + i * AP_ROW;
}

static void ap_follow(struct page *pg)
{
    int y0, y1;
    if (pg->focus < 0)
    {
        y0 = 0;
        y1 = AP_HEADER + AP_BUTTONS;
    }
    else
    {
        y0 = ap_track_y(pg->focus) - 8;
        y1 = ap_track_y(pg->focus) + AP_ROW + 8;
    }
    if (y0 < pg->scroll)
        pg->scroll = MAX(0, y0);
    else if (y1 > pg->scroll + LIST_H)
        pg->scroll = y1 - LIST_H;
}

static void album_draw(void)
{
    struct page *pg = &pages[level];
    ensure_loaded(level);
    update_playing();
    gfx_clear(C_DARK);
    gfx_clip(0, LIST_TOP, SCR_W, LIST_H);
    int y = LIST_TOP - pg->scroll;

    /* header: cover, title, artist, tracks and time */
    int cs = 96;
    draw_thumb(pg->cover_idx, 12, y + 10, cs, 12);
    int tx = 12 + cs + 12, tw = SCR_W - tx - 10;
    const char *title = pg->q.key[0] && strcmp(pg->q.key, UNTAGGED) ? pg->q.key
                                                                     : osm_str(S_UNKNOWN_ALBUM);
    int lines = gfx_text_wrap(OSM_FONT_B15, tx, y + 12, tw, 3, 19, title, C_WHITE);
    int ty = y + 12 + lines * 19 + 3;
    gfx_text_fit(OSM_FONT_R12, tx, ty, tw,
                 pg->q.key2[0] ? pg->q.key2 : osm_str(S_UNKNOWN_ARTIST), C_GOLD, ALIGN_LEFT);
    char info[48], t[16];
    ui_format_time(t, sizeof t, pg->total_ms);
    if (pg->count == 1)
        snprintf(info, sizeof info, "%s \xC2\xB7 %s", osm_str(S_ONE_TRACK), t);
    else
    {
        char n[24];
        snprintf(n, sizeof n, osm_str(S_TRACK_COUNT), pg->count);
        snprintf(info, sizeof info, "%s \xC2\xB7 %s", n, t);
    }
    gfx_text_fit(OSM_FONT_R11, tx, ty + 18, tw, info, SILVER_A(0.6), ALIGN_LEFT);

    /* Play (gold) and Shuffle (dark) pills */
    int by = y + AP_HEADER;
    /* as wide as their words (Riproduci is longer than Play), within the row */
    int ph = 32, gap = 10, avail = SCR_W - 24 - gap;
    int pw = MAX(96, 34 + gfx_text_width(OSM_FONT_B13, osm_str(S_PLAY)) + 16);
    int sw = MAX(96, 32 + gfx_text_width(OSM_FONT_R13, osm_str(S_SHUFFLE)) + 16);
    if (pw + sw > avail)
    {
        int over = pw + sw - avail;
        int cut_p = over * pw / (pw + sw);
        pw -= cut_p;
        sw -= over - cut_p;
    }
    bool pf = pg->focus == -2, sf = pg->focus == -1;
    gfx_rrect(12, by, pw, ph, 16, C_GOLD);
    if (pf)
        gfx_rrect_border(10, by - 2, pw + 4, ph + 4, 18, 2, C_WHITE);
    gfx_icon(ICON_PLAY_FILL, 14, 12 + 14, by + 9, C_BLACK);
    gfx_text_fit(OSM_FONT_B13, 12 + 34, by + 8, pw - 34 - 10, osm_str(S_PLAY),
                 C_BLACK, ALIGN_LEFT);
    int sx = 12 + pw + gap;
    gfx_rrect(sx, by, sw, ph, 16, sf ? GOLD_A(0.15) : WA(0.08));
    gfx_rrect_border(sx, by, sw, ph, 16, sf ? 2 : 1, sf ? C_GOLD : WA(0.10));
    gfx_icon(ICON_SHUFFLE, 14, sx + 13, by + 9, sf ? C_GOLD : C_WHITE);
    gfx_text_fit(OSM_FONT_R13, sx + 32, by + 8, sw - 32 - 10, osm_str(S_SHUFFLE),
                 sf ? C_GOLD : C_WHITE, ALIGN_LEFT);

    /* TRACKS */
    char tracks_label[32];
    ui_upper(tracks_label, sizeof tracks_label, osm_str(S_TRACKS));
    gfx_text_ls(OSM_FONT_B10, 14, y + AP_HEADER + AP_BUTTONS + 6, tracks_label,
                SILVER_A(0.6), 20);

    for (int i = 0; i < pg->count; i++)
    {
        int ry = y + ap_track_y(i);
        if (ry + AP_ROW < LIST_TOP || ry > LIST_TOP + LIST_H)
            continue;
        const struct lib_item *it = &lib_items[i];
        bool sel = pg->focus == i, playing = is_playing_item(it);
        int cx = 6, cw = SCR_W - 12;
        if (sel)
            ui_draw_focus(cx, ry + 1, cw, AP_ROW - 3, 10);
        else if (playing)
        {
            gfx_rrect(cx, ry + 1, cw, AP_ROW - 3, 10, GOLD_A(0.10));
            gfx_rrect_border(cx, ry + 1, cw, AP_ROW - 3, 10, 1, GOLD_A(0.5));
        }
        else
            gfx_rrect(cx, ry + 1, cw, AP_ROW - 3, 10, C_SURFACE);
        /* the number, or a gold ▶ on the one playing */
        if (playing)
            gfx_icon(ICON_PLAY_FILL, 12, cx + 10, ry + (AP_ROW - 12) / 2, C_GOLD);
        else
        {
            char num[12];
            int tn = it->num2 % 1000;
            snprintf(num, sizeof num, "%d", tn > 0 ? tn : i + 1);
            int nw = gfx_text_width(OSM_FONT_R11, num);
            gfx_text(OSM_FONT_R11, cx + 22 - nw, ry + (AP_ROW - 13) / 2, num, SILVER_A(0.5));
        }
        char dur[16] = "";
        if (it->num > 0)
            ui_format_time(dur, sizeof dur, it->num);
        int dw = gfx_text_width(OSM_FONT_M10, dur);
        gfx_text(OSM_FONT_M10, cx + cw - 10 - dw, ry + (AP_ROW - 13) / 2, dur, SILVER_A(0.6));
        int ttx = cx + 32, ttw = cw - 32 - dw - 18;
        bool show_artist = it->sub && it->sub[0] && strcasecmp(it->sub, pg->q.key2);
        if (show_artist)
        {
            gfx_text_fit(playing ? OSM_FONT_B12 : OSM_FONT_R12, ttx, ry + 4, ttw, it->name,
                         playing ? C_GOLD : C_WHITE, ALIGN_LEFT);
            gfx_text_fit(OSM_FONT_R10, ttx, ry + 20, ttw, it->sub, SILVER_A(0.55), ALIGN_LEFT);
        }
        else
            gfx_text_fit(playing ? OSM_FONT_B12 : OSM_FONT_R12, ttx, ry + (AP_ROW - 15) / 2, ttw,
                         it->name, playing ? C_GOLD : C_WHITE, ALIGN_LEFT);
    }
    gfx_noclip();

    /* a scrollbar for long albums */
    int content = ap_track_y(pg->count) + 8;
    if (content > LIST_H)
    {
        int th = MAX(16, LIST_H * LIST_H / content);
        int tyy = LIST_TOP + (LIST_H - th) * pg->scroll / MAX(1, content - LIST_H);
        gfx_rrect(SCR_W - 4, tyy, 3, th, 1, GOLD_A(0.5));
    }
    ui_draw_statusbar(true, NULL);
    draw_crumbs();
    ui_draw_mini_player();
}

static bool album_page_key(int key, int ev)
{
    struct page *pg = &pages[level];
    ensure_loaded(level);
    bool move = ev == EV_PRESS || ev == EV_REPEAT;
    int f = pg->focus;
    if (move)
    {
        switch (key)
        {
            case K_UP: case K_SCROLL_BACK:
                if (f >= 0) f = f == 0 ? -2 : f - 1;
                else if (key == K_SCROLL_BACK && f == -1) f = -2;
                break;
            case K_DOWN: case K_SCROLL_FWD:
                if (f < 0)
                {
                    if (key == K_SCROLL_FWD && f == -2) f = -1;
                    else if (pg->count) f = 0;
                }
                else if (f < pg->count - 1) f++;
                break;
            case K_LEFT:
                if (f == -1) f = -2;
                else if (ev == EV_PRESS && f == -2) { osm_pop(); return true; }
                else return false;
                break;
            case K_RIGHT:
                if (f == -2) f = -1;
                else return false;
                break;
            default:
                return false;
        }
        pg->focus = f;
        ap_follow(pg);
        osm_dirty();
        return true;
    }
    if (key == K_SELECT && ev == EV_SHORT)
    {
        if (pg->count <= 0)
            return true;
        if (f == -2 || f == -1)
            player_play_fill(lib_fill, NULL, f == -1 ? -1 : 0, f == -1);
        else
            player_play_fill(lib_fill, NULL, f, global_settings.playlist_shuffle);
        osm_dirty();
        return true;
    }
    if ((key == K_SELECT || key == K_MENU) && ev == EV_LONG && f >= 0 && f < pg->count)
    {
        browse_item_menu(&lib_items[f], lib_items[f].name);
        return true;
    }
    return false;
}

void album_open(const char *album, const char *artist)
{
    if (level + 1 >= LEVELS)
        return;
    struct page *pg = &pages[++level];
    memset(pg, 0, sizeof(*pg));
    pg->album = true;
    pg->q.view = LIB_ALBUM;
    strmemccpy(pg->q.key, album_key(album), sizeof pg->q.key);
    strmemccpy(pg->q.key2, artist ? artist : "", sizeof pg->q.key2);
    strmemccpy(pg->title, album[0] == '\x7f' ? osm_str(S_UNKNOWN_ALBUM) : album,
               sizeof pg->title);
    pg->focus = -2;
    loaded_level = -1;
    ensure_loaded(level);
    /* start on the track playing, when it is on this album */
    update_playing();
    for (int i = 0; i < pg->count; i++)
        if (is_playing_item(&lib_items[i]))
        {
            pg->focus = i;
            ap_follow(pg);
            break;
        }
    osm_push(&scr_album, NULL);
}

const struct osm_screen scr_album = {
    .name = "album",
    .enter = browse_enter,
    .resume = browse_resume,
    .leave = browse_leave,
    .draw = album_draw,
    .key = album_page_key,
    .shows_mini_player = true,
};
