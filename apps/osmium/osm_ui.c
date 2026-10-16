/***************************************************************************
 *   Osmium Sound for FiiO M3K — the interface framework
 *
 *   The main loop, the keys, the screen stack and its transitions, the
 *   overlays (toast, menu sheet, dialog) and the pieces every library
 *   screen shares: status bar, mini player, lists and rows.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "panic.h"
#include "button.h"
#include "backlight.h"
#include "powermgmt.h"
#include "power.h"
#include "timefuncs.h"
#include "audio.h"
#include "settings.h"
#include "misc.h"
#include "lang.h"
#include "language.h"
#include "string-extra.h"
#include "usb.h"
#include "splash.h"
#include "osmium.h"
#include "osm_ui.h"
#include "osm_player.h"
#include "osm_screens.h"
#include "osm_settings.h"
#include "osm_library.h"

/* ── state ─────────────────────────────────────────────────────────── */

#define STACK_MAX 16
static struct { const struct osm_screen *s; } stack[STACK_MAX];
static int depth;

static bool np_shown;
static bool dirty = true;
static unsigned int ui_thread_id;
static bool keys_locked;
static long last_input;

/* transitions */
enum { TR_NONE, TR_PUSH, TR_POP, TR_UP, TR_DOWN };
static int pending_transition = TR_NONE;
static fb_data snap_old[SCR_W * SCR_H] CACHEALIGN_ATTR;
static fb_data snap_new[SCR_W * SCR_H] CACHEALIGN_ATTR;

/* toast */
static struct {
    enum osm_icon icon;
    char text[96];
    long until;
    bool volume;
    volatile bool posted;       /* set from another thread */
} toast;

/* menu sheet */
static struct {
    bool open;
    char title[64];
    struct osm_menu_item items[OSM_MENU_MAX];
    int count;
    struct osm_list list;
    void (*cb)(int, void *);
    void *data;
} menu;

/* dialog */
static struct {
    bool open;
    char title[64];
    char message[200];
    char yes[24], no[24];
    int sel;                    /* 0 = no, 1 = yes */
    void (*cb)(bool, void *);
    void *data;
} dialog;

/* the core's progress splash (database, playlist loading) */
static struct {
    bool on;
    int cur, total;
    char text[96];
    long delay_until;
    long delay;
} progress;

/* ── screens ───────────────────────────────────────────────────────── */

void osm_dirty(void)
{
    dirty = true;
}

const struct osm_screen *osm_top(void)
{
    return depth ? stack[depth - 1].s : NULL;
}

int osm_depth(void)
{
    return depth;
}

void osm_push(const struct osm_screen *s, void *arg)
{
    if (depth >= STACK_MAX)
        return;
    stack[depth++].s = s;
    if (s->enter)
        s->enter(arg);
    if (depth > 1)
        pending_transition = TR_PUSH;
    dirty = true;
}

void osm_replace(const struct osm_screen *s, void *arg)
{
    if (!depth)
    {
        osm_push(s, arg);
        return;
    }
    const struct osm_screen *old = stack[depth - 1].s;
    if (old->leave)
        old->leave();
    stack[depth - 1].s = s;
    if (s->enter)
        s->enter(arg);
    dirty = true;
}

void osm_pop(void)
{
    if (depth <= 1)
        return;
    const struct osm_screen *old = stack[depth - 1].s;
    if (old->leave)
        old->leave();
    depth--;
    if (stack[depth - 1].s->resume)
        stack[depth - 1].s->resume();
    pending_transition = TR_POP;
    dirty = true;
}

void osm_pop_to_root(void)
{
    if (depth <= 1)
        return;
    while (depth > 1)
    {
        const struct osm_screen *old = stack[depth - 1].s;
        if (old->leave)
            old->leave();
        depth--;
    }
    if (stack[0].s->resume)
        stack[0].s->resume();
    pending_transition = TR_POP;
    dirty = true;
}

void osm_show_now_playing(bool show)
{
    if (show == np_shown)
        return;
    np_shown = show;
    if (show)
        scr_nowplaying.enter(NULL);
    else if (scr_nowplaying.leave)
        scr_nowplaying.leave();
    pending_transition = show ? TR_UP : TR_DOWN;
    dirty = true;
}

bool osm_now_playing_shown(void)
{
    return np_shown;
}

long osm_last_input(void)
{
    return last_input;
}

bool osm_keys_locked(void)
{
    return keys_locked;
}

/* the firmware asks this for the backlight on hold */
bool is_keys_locked(void)
{
    return keys_locked;
}

/* ── overlays ──────────────────────────────────────────────────────── */

void osm_toast(enum osm_icon icon, const char *msg)
{
    toast.icon = icon;
    strmemccpy(toast.text, msg, sizeof toast.text);
    toast.until = current_tick + HZ * 3 / 2;
    toast.volume = false;
    toast.posted = true;
    dirty = true;
}

void osm_toast_volume(void)
{
    toast.volume = true;
    toast.until = current_tick + HZ;
    toast.posted = true;
    dirty = true;
}

void osm_menu(const char *title, const struct osm_menu_item *items, int count,
              int selected, void (*cb)(int, void *), void *data)
{
    count = MIN(count, OSM_MENU_MAX);
    strmemccpy(menu.title, title ? title : "", sizeof menu.title);
    memcpy(menu.items, items, count * sizeof(items[0]));
    menu.count = count;
    menu.cb = cb;
    menu.data = data;
    int rows = MIN(count, 6);
    int h = rows * 34;
    list_init(&menu.list, count, 10, SCR_H - 12 - h, SCR_W - 20, h, 34);
    list_set_sel(&menu.list, selected < 0 ? 0 : selected);
    menu.open = true;
    dirty = true;
}

bool osm_menu_open(void)
{
    return menu.open;
}

void osm_dialog(const char *title, const char *message, const char *yes,
                const char *no, void (*cb)(bool, void *), void *data)
{
    strmemccpy(dialog.title, title ? title : "", sizeof dialog.title);
    strmemccpy(dialog.message, message ? message : "", sizeof dialog.message);
    strmemccpy(dialog.yes, yes ? yes : osm_str(S_OK), sizeof dialog.yes);
    strmemccpy(dialog.no, no ? no : osm_str(S_CANCEL), sizeof dialog.no);
    dialog.sel = 1;
    dialog.cb = cb;
    dialog.data = data;
    dialog.open = true;
    dirty = true;
}

bool osm_dialog_open(void)
{
    return dialog.open;
}

static void draw_toast(void)
{
    if (!toast.until || TIME_AFTER(current_tick, toast.until))
    {
        toast.until = 0;
        return;
    }
    char vol[16];
    const char *text = toast.text;
    enum osm_icon icon = toast.icon;
    if (toast.volume)
    {
        int v = player_volume_percent();
        snprintf(vol, sizeof vol, "%d%%", v);
        text = vol;
        icon = v == 0 ? ICON_VOLUME_X : ICON_VOLUME_2;
    }
    int tw = MIN(gfx_text_width(OSM_FONT_R12, text), SCR_W - 70);
    int w = 12 + 16 + 8 + tw + 14;
    if (toast.volume)
        w += 70;
    int h = 34, x = (SCR_W - w) / 2, y = 8;
    gfx_shadow(x, y, w, h, 12, 8, 14, 110);
    gfx_rrect(x, y, w, h, 12, 0xFF2A2A2Au);
    gfx_rrect_border(x, y, w, h, 12, 1, 0xFF3A3A3Au);
    gfx_icon(icon, 16, x + 12, y + 9, C_GOLD);
    int tx = x + 12 + 16 + 8;
    if (toast.volume)
    {
        /* a level bar, the slider of Osmium's player */
        int bw = 62, bx = tx, by = y + 15;
        gfx_rrect(bx, by, bw, 4, 2, WA(0.10));
        gfx_rrect(bx, by, bw * player_volume_percent() / 100, 4, 2, C_GOLD);
        tx += bw + 8;
    }
    gfx_text_fit(OSM_FONT_R12, tx, y + (h - gfx_font_height(OSM_FONT_R12)) / 2,
                 tw + 2, text, C_WHITE, ALIGN_LEFT);
}

static void paint_menu_row(int i, int x, int y, int w, int h, bool sel, void *d)
{
    (void)d;
    const struct osm_menu_item *it = &menu.items[i];
    if (sel)
        gfx_rrect(x + 4, y + 1, w - 8, h - 2, 8, C_LIGHT);
    gfx_icon(it->icon, 16, x + 14, y + (h - 16) / 2, C_GOLD);
    gfx_text_fit(OSM_FONT_R13, x + 40, y + (h - gfx_font_height(OSM_FONT_R13)) / 2,
                 w - 40 - (it->checked ? 30 : 12), it->label, C_WHITE, ALIGN_LEFT);
    if (it->checked)
        gfx_icon(ICON_CHECK, 16, x + w - 28, y + (h - 16) / 2, C_GOLD);
}

static void draw_menu(void)
{
    gfx_fill(0, 0, SCR_W, SCR_H, BLACK_A(0.6));
    struct osm_list *l = &menu.list;
    int title_h = menu.title[0] ? 26 : 0;
    int bx = 6, bw = SCR_W - 12;
    int by = l->y - 8 - title_h, bh = l->h + 16 + title_h;
    gfx_shadow(bx, by, bw, bh, 16, 12, 24, 160);
    gfx_rrect(bx, by, bw, bh, 16, C_PANEL);
    gfx_rrect_border(bx, by, bw, bh, 16, 1, C_BORDER);
    if (title_h)
    {
        gfx_text_fit(OSM_FONT_B10, bx + 16, by + 10, bw - 32, menu.title, SILVER_A(0.7),
                     ALIGN_LEFT);
    }
    gfx_clip(l->x, l->y, l->w, l->h);
    list_draw(l, paint_menu_row, NULL);
    gfx_noclip();
    list_draw_scrollbar(l);
}

static void draw_dialog(void)
{
    gfx_fill(0, 0, SCR_W, SCR_H, BLACK_A(0.7));
    int w = SCR_W - 24, x = 12;
    int lines_h = 0;
    /* measure the message */
    int lh = 18;
    int nlines = 0;
    {
        /* at most 5 lines */
        nlines = 1 + gfx_text_width(OSM_FONT_R13, dialog.message) / (w - 32);
        nlines = MIN(5, nlines);
        if (!dialog.message[0])
            nlines = 0;
        lines_h = nlines * lh;
    }
    int h = 16 + 24 + (nlines ? 6 + lines_h : 0) + 16 + 38 + 16;
    int y = (SCR_H - h) / 2;
    gfx_shadow(x, y, w, h, 16, 14, 28, 170);
    gfx_rrect(x, y, w, h, 16, C_LIGHT);
    gfx_rrect_border(x, y, w, h, 16, 1, C_ACCENT);
    int cy = y + 16;
    gfx_text_fit(OSM_FONT_B15, x + 16, cy + 2, w - 32, dialog.title, C_WHITE, ALIGN_LEFT);
    cy += 24;
    if (nlines)
    {
        cy += 6;
        gfx_text_wrap(OSM_FONT_R13, x + 16, cy, w - 32, nlines, lh, dialog.message, C_SILVER);
        cy += lines_h;
    }
    cy += 16;
    int bw = (w - 32 - 10) / 2;
    /* Cancel on the left, the gold one on the right (Dialogs.qml) */
    int bx0 = x + 16, bx1 = x + 16 + bw + 10;
    gfx_rrect(bx0, cy, bw, 38, 8, dialog.sel == 0 ? C_ACCENT : C_DARK);
    gfx_text_fit(OSM_FONT_R13, bx0, cy + 11, bw, dialog.no, C_WHITE, ALIGN_CENTER);
    gfx_rrect(bx1, cy, bw, 38, 8, dialog.sel == 1 ? C_GOLD : 0xFF8A7424u);
    gfx_text_fit(OSM_FONT_B13, bx1, cy + 11, bw, dialog.yes, C_BLACK, ALIGN_CENTER);
    if (dialog.sel == 0)
        gfx_rrect_border(bx0 - 2, cy - 2, bw + 4, 42, 10, 2, C_GOLD);
    else
        gfx_rrect_border(bx1 - 2, cy - 2, bw + 4, 42, 10, 2, GOLD_A(0.6));
}

static void draw_progress(void)
{
    if (!progress.on || TIME_BEFORE(current_tick, progress.delay_until))
        return;
    int w = SCR_W - 40, h = 64, x = 20, y = (SCR_H - h) / 2;
    gfx_fill(0, 0, SCR_W, SCR_H, BLACK_A(0.6));
    gfx_rrect(x, y, w, h, 14, C_LIGHT);
    gfx_rrect_border(x, y, w, h, 14, 1, C_ACCENT);
    gfx_text_fit(OSM_FONT_R12, x + 14, y + 14, w - 28, progress.text, C_WHITE, ALIGN_LEFT);
    int bw = w - 28;
    gfx_rrect(x + 14, y + 40, bw, 5, 2, WA(0.1));
    if (progress.total > 0)
        gfx_rrect(x + 14, y + 40, bw * MIN(progress.cur, progress.total) / progress.total, 5, 2, C_GOLD);
}

/* ── drawing the frame ─────────────────────────────────────────────── */

static void render(void)
{
    gfx_noclip();
    if (osm_library_preparing())
    {
        osm_library_screen_draw();
        gfx_noclip();
        draw_toast();
        return;
    }
    if (np_shown)
        scr_nowplaying.draw();
    else if (depth)
        stack[depth - 1].s->draw();
    else
        gfx_clear(C_DARK);
    gfx_noclip();
    if (menu.open)
        draw_menu();
    if (dialog.open)
        draw_dialog();
    draw_progress();
    draw_toast();
}

/* ease-out over the frames of a transition */
static void run_transition(int kind)
{
    if (kind == TR_NONE)
        return;
    gfx_snapshot(snap_new);
    /* few frames, each a handful of row copies: the page moves at once */
    const int frames = 5;
    for (int f = 1; f <= frames; f++)
    {
        int t = f * 256 / frames;
        int e = 256 - ((256 - t) * (256 - t) >> 8);       /* ease out */
        if (kind == TR_PUSH || kind == TR_POP)
        {
            bool push = kind == TR_PUSH;
            /* the new page slides in from the right (or the old one out),
             * the one underneath drifts a quarter and dims a little */
            int off = push ? SCR_W - (SCR_W * e >> 8) : (SCR_W * e >> 8);
            int back = push ? -(SCR_W / 4) * e >> 8 : -(SCR_W / 4) * (256 - e) >> 8;
            const fb_data *under = push ? snap_old : snap_new;
            const fb_data *over = push ? snap_new : snap_old;
            int dim = push ? e : 256 - e;
            unsigned keep = 256 - dim * 90 / 256;
            /* the part of the row from the page underneath: x in [0, off),
             * read from x - back, which is >= 0 since back <= 0 */
            int un = MIN(off, SCR_W + back);
            for (int y = 0; y < SCR_H; y++)
            {
                fb_data *d = &osm_fb[y * SCR_W];
                if (un > 0)
                {
                    memcpy(d, &under[y * SCR_W - back], un * sizeof(fb_data));
                    gfx_darken_span(d, un, keep);
                }
                if (off > un)
                    memset(&d[MAX(un, 0)], 0, (off - MAX(un, 0)) * sizeof(fb_data));
                if (off < SCR_W)
                    memcpy(&d[off], &over[y * SCR_W], (SCR_W - off) * sizeof(fb_data));
            }
        }
        else
        {
            /* Now Playing rises from the bottom (and sinks back) */
            bool up = kind == TR_UP;
            int off = up ? SCR_H - (SCR_H * e >> 8) : (SCR_H * e >> 8);
            const fb_data *under = up ? snap_old : snap_new;
            const fb_data *over = up ? snap_new : snap_old;
            memcpy(osm_fb, under, off * SCR_W * sizeof(fb_data));
            memcpy(&osm_fb[off * SCR_W], over, (SCR_H - off) * SCR_W * sizeof(fb_data));
        }
        gfx_present();
        yield();
    }
    gfx_restore(snap_new);
}

static void redraw(void)
{
    int tr = pending_transition;
    pending_transition = TR_NONE;
    if (tr != TR_NONE)
        gfx_snapshot(snap_old);
    render();
    if (tr != TR_NONE)
        run_transition(tr);
    gfx_present();
    dirty = false;
}

/* ── keys ──────────────────────────────────────────────────────────── */

#define LONG_PRESS (HZ * 6 / 10)

static int map_button(int b)
{
    switch (b)
    {
        case BUTTON_UP:          return K_UP;
        case BUTTON_DOWN:        return K_DOWN;
        case BUTTON_LEFT:        return K_LEFT;
        case BUTTON_RIGHT:       return K_RIGHT;
        case BUTTON_SELECT:      return K_SELECT;
        case BUTTON_BACK:        return K_BACK;
        case BUTTON_MENU:        return K_MENU;
        case BUTTON_PLAY:        return K_PLAY;
        case BUTTON_VOL_UP:      return K_VOL_UP;
        case BUTTON_VOL_DOWN:    return K_VOL_DOWN;
        case BUTTON_POWER:       return K_POWER;
        case BUTTON_SCROLL_FWD:  return K_SCROLL_FWD;
        case BUTTON_SCROLL_BACK: return K_SCROLL_BACK;
    }
    return K_NONE;
}

static void menu_key(int key, int ev)
{
    if (list_key(&menu.list, key, ev))
    {
        dirty = true;
        return;
    }
    if ((key == K_SELECT || key == K_RIGHT) && (ev == EV_SHORT || ev == EV_LONG))
    {
        int sel = menu.list.sel;
        menu.open = false;
        dirty = true;
        if (menu.cb)
            menu.cb(sel, menu.data);
    }
    else if ((key == K_BACK || key == K_LEFT || key == K_MENU) && ev == EV_SHORT)
    {
        menu.open = false;
        dirty = true;
        if (menu.cb)
            menu.cb(-1, menu.data);
    }
}

static void dialog_key(int key, int ev)
{
    if (ev != EV_PRESS && ev != EV_REPEAT && ev != EV_SHORT)
        return;
    switch (key)
    {
        case K_LEFT: case K_UP: case K_SCROLL_BACK:
            if (ev != EV_SHORT) { dialog.sel = 0; dirty = true; }
            break;
        case K_RIGHT: case K_DOWN: case K_SCROLL_FWD:
            if (ev != EV_SHORT) { dialog.sel = 1; dirty = true; }
            break;
        case K_SELECT:
            if (ev == EV_SHORT)
            {
                dialog.open = false;
                dirty = true;
                if (dialog.cb)
                    dialog.cb(dialog.sel == 1, dialog.data);
            }
            break;
        case K_BACK:
            if (ev == EV_SHORT)
            {
                dialog.open = false;
                dirty = true;
                if (dialog.cb)
                    dialog.cb(false, dialog.data);
            }
            break;
    }
}

static void lock_keys(bool on)
{
    keys_locked = on;
    osm_toast(ICON_LOCK, osm_str(on ? S_LOCKED : S_UNLOCKED));
    if (on)
        backlight_off();
    else
        backlight_on();
}

static void dispatch(int key, int ev)
{
    /* the power key: a short press locks, holding it powers off */
    if (key == K_POWER)
    {
        if (ev == EV_SHORT)
            lock_keys(!keys_locked);
        else if (ev == EV_LONG)
            sys_poweroff();
        return;
    }
    if (keys_locked)
    {
        if (ev == EV_PRESS)
        {
            backlight_on();
            osm_toast(ICON_LOCK, osm_str(S_UNLOCK_HINT));
        }
        return;
    }

    /* volume: anywhere */
    if (key == K_VOL_UP || key == K_VOL_DOWN)
    {
        if (ev == EV_PRESS || ev == EV_REPEAT)
        {
            player_volume_step(key == K_VOL_UP ? 1 : -1);
            osm_toast_volume();
        }
        return;
    }

    /* nothing to browse until the library is built */
    if (osm_library_preparing())
    {
        if (key == K_SELECT && ev == EV_SHORT && osm_library_needs_restart())
            sys_reboot();
        return;
    }

    if (dialog.open)
    {
        dialog_key(key, ev);
        return;
    }
    if (menu.open)
    {
        menu_key(key, ev);
        return;
    }

    /* the screen first, then the keys that mean the same everywhere */
    bool handled = np_shown ? scr_nowplaying.key(key, ev)
                            : (depth && stack[depth - 1].s->key(key, ev));
    if (handled)
        return;

    switch (key)
    {
        case K_PLAY:
            if (ev == EV_SHORT)
                player_toggle_play();
            else if (ev == EV_LONG)
                audio_stop();
            break;
        case K_MENU:
            if (ev == EV_SHORT)
                osm_show_now_playing(!np_shown);
            break;
        case K_BACK:
            if (ev == EV_SHORT)
            {
                if (np_shown)
                    osm_show_now_playing(false);
                else
                    osm_pop();
            }
            else if (ev == EV_LONG)
            {
                /* home, as it is after power on */
                if (np_shown)
                    osm_show_now_playing(false);
                osm_pop_to_root();
                home_reset();
            }
            break;
    }
}

/* key state for long presses */
static long down_tick[16];
static bool long_fired[16];

static void handle_button(int btn)
{
    last_input = current_tick;
    int base = btn & ~(BUTTON_REL | BUTTON_REPEAT);
    /* the wheel and combined presses: take the bits one at a time */
    for (int bit = 0; bit < 13; bit++)
    {
        int b = base & (1 << bit);
        if (!b)
            continue;
        int key = map_button(b);
        if (key == K_NONE)
            continue;
        if (key == K_SCROLL_FWD || key == K_SCROLL_BACK)
        {
            if (!(btn & BUTTON_REL))
                dispatch(key, EV_PRESS);
            continue;
        }
        if (btn & BUTTON_REL)
        {
            if (!long_fired[key])
                dispatch(key, EV_SHORT);
            long_fired[key] = false;
            down_tick[key] = 0;
        }
        else if (btn & BUTTON_REPEAT)
        {
            if (!long_fired[key] && down_tick[key] &&
                TIME_AFTER(current_tick, down_tick[key] + LONG_PRESS))
            {
                long_fired[key] = true;
                dispatch(key, EV_LONG);
            }
            dispatch(key, EV_REPEAT);
        }
        else
        {
            down_tick[key] = current_tick;
            long_fired[key] = false;
            dispatch(key, EV_PRESS);
        }
    }
    dirty = true;
}

/* ── the core's notifications ──────────────────────────────────────── */

static bool on_ui_thread(void)
{
    return thread_self() == ui_thread_id;
}

void splash(int ticks, const char *str)
{
    (void)ticks;
    if (!str)
        return;
    str = P2STR((const unsigned char *)str);
    osm_toast(ICON_INFO, str);
    if (on_ui_thread() && ui_thread_id)
        redraw();
}

void splashf(int ticks, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, P2STR((const unsigned char *)fmt), ap);
    va_end(ap);
    splash(ticks, buf);
}

void splash_progress_set_delay(long delay_ticks)
{
    progress.delay = delay_ticks;
    progress.delay_until = current_tick + delay_ticks;
}

void splash_progress(int current, int total, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(progress.text, sizeof progress.text, P2STR((const unsigned char *)fmt), ap);
    va_end(ap);
    if (!progress.on)
    {
        progress.on = true;
        if (!progress.delay)
            progress.delay_until = current_tick;
    }
    progress.cur = current;
    progress.total = total;
    if (on_ui_thread() && ui_thread_id)
    {
        render();
        gfx_present();
    }
}

static void progress_done(void)
{
    progress.on = false;
    progress.delay = 0;
}

bool osm_user_abort(void)
{
    if (!on_ui_thread())
        return false;
    int b = button_get(false);
    return (b & ~BUTTON_REPEAT) == BUTTON_BACK;
}

/* a nested loop until the dialog is answered */
static bool wait_answer;
static bool wait_done;
static void wait_cb(bool yes, void *d)
{
    (void)d;
    wait_answer = yes;
    wait_done = true;
}

bool osm_dialog_wait(const char *title, const char *message,
                     const char *yes, const char *no)
{
    if (!on_ui_thread())
        return true;
    osm_dialog(title, message, yes, no, wait_cb, NULL);
    wait_done = false;
    while (!wait_done)
    {
        if (dirty)
            redraw();
        int btn = button_get_w_tmo(HZ / 4);
        if (btn == BUTTON_NONE)
            continue;
        if (btn & SYS_EVENT)
        {
            if (btn == SYS_USB_CONNECTED || btn == SYS_POWEROFF || btn == SYS_REBOOT)
            {
                dialog.open = false;
                default_event_handler(btn);
                return false;
            }
            continue;
        }
        handle_button(btn);
    }
    return wait_answer;
}

bool osm_confirm(const char *question)
{
    return osm_dialog_wait(question, NULL, osm_str(S_YES), osm_str(S_NO));
}

bool osm_confirm_reboot_new_firmware(void)
{
    return osm_dialog_wait(osm_str(S_NEW_FIRMWARE), osm_str(S_NEW_FIRMWARE_Q),
                           osm_str(S_RESTART), osm_str(S_CANCEL));
}

/* ── the main loop ─────────────────────────────────────────────────── */

static bool want_frames(void)
{
    if (toast.until)
        return true;
    if (np_shown)
        return scr_nowplaying.animating && scr_nowplaying.animating();
    const struct osm_screen *s = osm_top();
    return s && s->animating && s->animating();
}

/* The interface runs in its own thread: the main thread's stack is small
 * (a few KB in the first page of DRAM on the M3K, right above the exception
 * vectors) and the UI goes deep: file system and database calls under a
 * menu under a dialog, the core calling back into splash() and so on. */
static long ui_stack[(64 * 1024) / sizeof(long)];

static void ui_thread(void) NORETURN_ATTR;
static void ui_thread(void)
{
    ui_thread_id = thread_self();
    OSM_TRACE("ui thread");
    gfx_init();
    player_init();
    OSM_TRACE("player_init");
    osm_ui_init_fonts();
    OSM_TRACE("fonts");
    osm_push(&scr_home, NULL);
    pending_transition = TR_NONE;
    OSM_TRACE("home");

    /* back to what was playing at power off, paused */
    osm_resume_at_boot();
    OSM_TRACE("resume");
#ifdef OSM_BOOT_TRACE
    sleep(HZ * 3);      /* time to read the trace */
#endif

    long last_poll = 0;
    while (1)
    {
        if (progress.on && !TIME_BEFORE(current_tick, progress.delay_until + HZ))
            progress_done();
        if (toast.posted)
        {
            toast.posted = false;
            dirty = true;
        }
        if (TIME_AFTER(current_tick, last_poll + HZ / 5) || want_frames())
        {
            last_poll = current_tick;
            if (player_poll())
                dirty = true;
            bool changed = osm_ui_tick();
            if (osm_ui_idle() || changed)
                dirty = true;
        }
        if (dirty || want_frames())
            redraw();

        int tmo = want_frames() ? HZ / 30 : HZ / 5;
        if (lib_thumbs_busy())
            tmo = HZ / 20;              /* covers are coming in */
        int btn = button_get_w_tmo(MAX(1, tmo));
        if (btn == BUTTON_NONE)
        {
            /* idle: decode a cover for the album grid */
            if (osm_ui_idle())
                dirty = true;
            continue;
        }
        if (btn & SYS_EVENT)
        {
            default_event_handler(btn);
            dirty = true;
            continue;
        }
        handle_button(btn);
    }
}

void osm_ui_run(void)
{
    /* let the boot animation finish building the mark first */
    osm_boot_splash_end(true);
    ui_thread_id = create_thread(ui_thread, ui_stack, sizeof(ui_stack), 0,
                                 "osm ui" IF_PRIO(, PRIORITY_USER_INTERFACE)
                                 IF_COP(, CPU));
    if (!ui_thread_id)
        panicf("cannot start the interface");
    OSM_TRACE("ui thread created");
    /* the main thread has nothing left to do (and its code from main.c
     * lives in the init section, reused once the system is up) */
    while (1)
        sleep(HZ * 60);
}

/* ── shared drawing ────────────────────────────────────────────────── */

void ui_upper(char *dst, int len, const char *src)
{
    int i = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && i < len - 2)
    {
        unsigned char c = *p++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        else if (c == 0xC3 && *p >= 0xA0 && *p <= 0xBE && *p != 0xB7)
        {
            /* à..þ -> À..Þ */
            dst[i++] = c;
            dst[i++] = *p++ - 0x20;
            continue;
        }
        dst[i++] = c;
    }
    dst[i] = 0;
}

void ui_format_time(char *buf, int len, unsigned long ms)
{
    unsigned long s = ms / 1000;
    if (s >= 3600)
        snprintf(buf, len, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(buf, len, "%lu:%02lu", s / 60, s % 60);
}

int ui_draw_battery(int xr, int y)
{
    int lvl = battery_level();
    bool chg = charging_state();
    /* the body, 18 x 9, and its tip */
    int w = 18, h = 9, x = xr - w - 2;
    gfx_rrect_border(x, y, w, h, 2, 1, SILVER_A(0.7));
    gfx_fill(x + w, y + 3, 2, 3, SILVER_A(0.7));
    if (lvl >= 0)
    {
        int fw = (w - 4) * MIN(100, lvl) / 100;
        color_t c = lvl <= 15 && !chg ? C_RED400 : chg ? C_EMERALD : C_SILVER;
        gfx_fill(x + 2, y + 2, MAX(1, fw), h - 4, c);
    }
    return w + 2;
}

void ui_draw_statusbar(bool brand, const char *title)
{
    gfx_fill(0, 0, SCR_W, STATUSBAR_H, C_PANEL);
    gfx_fill(0, STATUSBAR_H - 1, SCR_W, 1, A(C_BORDER, 0.6));
    /* right: clock and battery */
    int xr = SCR_W - 10;
    xr -= ui_draw_battery(xr, 10) + 8;
    struct tm *tm = get_time();
    char clock[8];
    if (global_settings.timeformat)
        snprintf(clock, sizeof clock, "%d:%02d", (tm->tm_hour + 11) % 12 + 1, tm->tm_min);
    else
        snprintf(clock, sizeof clock, "%02d:%02d", tm->tm_hour, tm->tm_min);
    int cw = gfx_text_width(OSM_FONT_R12, clock);
    gfx_text(OSM_FONT_R12, xr - cw, 7, clock, C_WHITE);
    xr -= cw + 8;
    if (player_sleep_minutes() > 0)
    {
        gfx_icon(ICON_MOON, 14, xr - 14, 7, C_GOLD);
        xr -= 20;
    }
    if (brand)
        gfx_brand(12, 10, 8, true);
    else if (title)
        gfx_text_fit(OSM_FONT_B13, 12, 7, xr - 12 - 6, title, C_WHITE, ALIGN_LEFT);
}

void ui_draw_mini_player(void)
{
    int y = SCR_H - MINIPLAYER_H;
    gfx_fill(0, y, SCR_W, MINIPLAYER_H, C_PANEL);
    gfx_fill(0, y, SCR_W, 1, A(C_BORDER, 0.8));
    const struct osm_track *t = &player.cur;
    /* the progress, a gold line on the top edge */
    if (t->valid && t->length)
    {
        int pw = (int)((long long)SCR_W * MIN(t->elapsed, t->length) / t->length);
        gfx_hgrad(0, y, pw, 2, C_GOLD, C_YELLOW400);
    }
    int cx = 8, cy = y + (MINIPLAYER_H - MINI_COVER_SIZE) / 2 + 1;
    if (t->valid && player.have_cover)
        gfx_bitmap(player.mini, MINI_COVER_SIZE, MINI_COVER_SIZE, cx, cy,
                   MINI_COVER_SIZE, MINI_COVER_SIZE, 6, 256);
    else
    {
        gfx_rrect(cx, cy, MINI_COVER_SIZE, MINI_COVER_SIZE, 6, C_GRAY);
        gfx_icon(ICON_MUSIC, 16, cx + 9, cy + 9, SILVER_A(0.4));
    }
    int tx = cx + MINI_COVER_SIZE + 10, tw = SCR_W - tx - 44;
    if (t->valid)
    {
        gfx_text_fit(OSM_FONT_B12, tx, y + 8, tw, t->title, C_WHITE, ALIGN_LEFT);
        gfx_text_fit(OSM_FONT_R11, tx, y + 25, tw,
                     t->artist[0] ? t->artist : osm_str(S_UNKNOWN_ARTIST), C_GOLD, ALIGN_LEFT);
    }
    else
        gfx_text_fit(OSM_FONT_R12, tx, y + 15, tw, osm_str(S_NOTHING_PLAYING),
                     SILVER_A(0.5), ALIGN_LEFT);
    /* play / pause in a gold disc */
    int bx = SCR_W - 24, by = y + MINIPLAYER_H / 2 + 1;
    gfx_circle(bx, by, 15, t->valid ? C_GOLD : WA(0.10));
    gfx_icon(player.playing ? ICON_PAUSE_FILL : ICON_PLAY_FILL, 14,
             bx - 7 + (player.playing ? 0 : 1), by - 7, t->valid ? C_BLACK : SILVER_A(0.5));
}

void ui_draw_focus(int x, int y, int w, int h, int r)
{
    gfx_rrect(x, y, w, h, r, GOLD_A(0.10));
    gfx_rrect_border(x, y, w, h, r, 2, C_GOLD);
}

void ui_draw_row(int x, int y, int w, int h, bool sel, enum osm_icon icon,
                 const char *title, const char *subtitle, const char *right,
                 bool chevron, bool playing)
{
    int pad = 3;
    int cx = x + 6, cw = w - 12;
    if (sel)
        ui_draw_focus(cx, y + pad / 2, cw, h - pad, 10);
    else if (playing)
    {
        gfx_rrect(cx, y + pad / 2, cw, h - pad, 10, GOLD_A(0.10));
        gfx_rrect_border(cx, y + pad / 2, cw, h - pad, 10, 1, GOLD_A(0.5));
    }
    else
        gfx_rrect(cx, y + pad / 2, cw, h - pad, 10, C_SURFACE);
    int tx = cx + 12;
    if (icon != ICON_COUNT)
    {
        gfx_icon(icon, 16, tx, y + (h - 16) / 2, playing || sel ? C_GOLD : SILVER_A(0.6));
        tx += 26;
    }
    int rw = 0;
    if (chevron)
    {
        gfx_icon(ICON_CHEVRON_RIGHT, 14, cx + cw - 22, y + (h - 14) / 2, SILVER_A(0.5));
        rw = 22;
    }
    if (right && right[0])
    {
        int w2 = gfx_text_width(OSM_FONT_M10, right);
        gfx_text(OSM_FONT_M10, cx + cw - 10 - rw - w2, y + (h - gfx_font_height(OSM_FONT_M10)) / 2,
                 right, SILVER_A(0.6));
        rw += w2 + 8;
    }
    int tw = cx + cw - 10 - rw - tx;
    color_t tc = playing ? C_GOLD : C_WHITE;
    if (subtitle && subtitle[0])
    {
        int fh = gfx_font_height(OSM_FONT_R13);
        int top = y + (h - (fh + 15)) / 2;
        gfx_text_fit(playing ? OSM_FONT_B13 : OSM_FONT_R13, tx, top, tw, title, tc, ALIGN_LEFT);
        gfx_text_fit(OSM_FONT_R11, tx, top + fh + 1, tw, subtitle, SILVER_A(0.6), ALIGN_LEFT);
    }
    else
        gfx_text_fit(playing ? OSM_FONT_B13 : OSM_FONT_R13, tx,
                     y + (h - gfx_font_height(OSM_FONT_R13)) / 2, tw, title, tc, ALIGN_LEFT);
}

void ui_draw_crumbs(const char *const *parts, int count)
{
    int y = STATUSBAR_H;
    gfx_fill(0, y, SCR_W, CRUMBS_H, C_DARK);
    gfx_fill(0, y + CRUMBS_H - 1, SCR_W, 1, A(C_BORDER, 0.5));
    int x = 10;
    gfx_icon(ICON_HOME, 14, x, y + 5, SILVER_A(0.7));
    x += 18;
    /* the last parts win when they do not fit */
    int first = 0;
    int avail = SCR_W - x - 10;
    while (first < count - 1)
    {
        int need = 0;
        for (int i = first; i < count; i++)
            need += 14 + gfx_text_width(OSM_FONT_R11, parts[i]);
        if (need <= avail)
            break;
        first++;
    }
    for (int i = first; i < count; i++)
    {
        gfx_icon(ICON_CHEVRON_RIGHT, 12, x, y + 6, SILVER_A(0.4));
        x += 14;
        bool last = i == count - 1;
        x += gfx_text_fit(OSM_FONT_R11, x, y + 5, SCR_W - x - 8, parts[i],
                          last ? C_WHITE : SILVER_A(0.6), ALIGN_LEFT);
    }
}

void ui_draw_jump_letter(const char *letter)
{
    int s = 56, x = (SCR_W - s) / 2, y = (SCR_H - s) / 2 - 10;
    gfx_rrect(x, y, s, s, 14, A(C_LIGHT, 0.92));
    gfx_rrect_border(x, y, s, s, 14, 1, GOLD_A(0.4));
    int w = gfx_text_width(OSM_FONT_B22, letter);
    gfx_text(OSM_FONT_B22, x + (s - w) / 2, y + (s - gfx_font_height(OSM_FONT_B22)) / 2,
             letter, C_GOLD);
}

/* ── lists ─────────────────────────────────────────────────────────── */

void list_init(struct osm_list *l, int count, int x, int y, int w, int h, int row_h)
{
    memset(l, 0, sizeof(*l));
    l->count = count;
    l->x = x; l->y = y; l->w = w; l->h = h;
    l->row_h = row_h;
    l->cols = 1;
}

static int list_rows_visible(const struct osm_list *l)
{
    return MAX(1, l->h / l->row_h);
}

static void list_follow(struct osm_list *l)
{
    int cols = MAX(1, l->cols);
    int rows = list_rows_visible(l);
    int row = l->sel / cols;
    int top_row = l->top / cols;
    /* keep one row of context when there is room */
    if (row < top_row)
        top_row = row;
    else if (row >= top_row + rows)
        top_row = row - rows + 1;
    l->top = top_row * cols;
}

void list_set_sel(struct osm_list *l, int sel)
{
    if (l->count <= 0)
    {
        l->sel = l->top = 0;
        return;
    }
    l->sel = MAX(0, MIN(sel, l->count - 1));
    /* centre it when jumping */
    int cols = MAX(1, l->cols);
    int rows = list_rows_visible(l);
    int top_row = l->sel / cols - rows / 2;
    int max_top = (l->count + cols - 1) / cols - rows;
    top_row = MAX(0, MIN(top_row, max_top));
    l->top = top_row * cols;
}

bool list_key(struct osm_list *l, int key, int ev)
{
    if (l->count <= 0)
        return false;
    int cols = MAX(1, l->cols);
    int step = 0;
    if (ev == EV_PRESS || ev == EV_REPEAT)
    {
        switch (key)
        {
            case K_UP:          step = -cols; break;
            case K_DOWN:        step = cols; break;
            case K_SCROLL_BACK: step = -1; break;
            case K_SCROLL_FWD:  step = 1; break;
            case K_LEFT:        if (cols > 1 && l->sel % cols) step = -1; break;
            case K_RIGHT:       if (cols > 1 && l->sel % cols < cols - 1 && l->sel + 1 < l->count) step = 1; break;
        }
    }
    if (!step)
        return false;
    /* acceleration while the wheel or a key keeps going */
    if (TIME_BEFORE(current_tick, l->last_move + HZ / 6))
        l->speed++;
    else
        l->speed = 0;
    l->last_move = current_tick;
    if (l->speed > 12 && (key == K_SCROLL_FWD || key == K_SCROLL_BACK || ev == EV_REPEAT))
        step *= l->speed > 30 ? 8 : 3;
    int n = l->sel + step;
    if (n < 0)
        n = (l->sel == 0 && ev == EV_PRESS && cols == 1) ? l->count - 1 : 0;   /* wrap */
    else if (n >= l->count)
        n = (l->sel == l->count - 1 && ev == EV_PRESS && cols == 1) ? 0 : l->count - 1;
    l->sel = n;
    list_follow(l);
    return true;
}

void list_draw(struct osm_list *l, void (*paint)(int, int, int, int, int, bool, void *), void *d)
{
    int cols = MAX(1, l->cols);
    int cw = l->w / cols;
    int rows = list_rows_visible(l) + 1;
    for (int r = 0; r < rows; r++)
    {
        for (int c = 0; c < cols; c++)
        {
            int i = l->top + r * cols + c;
            if (i >= l->count)
                return;
            paint(i, l->x + c * cw, l->y + r * l->row_h, cw, l->row_h, i == l->sel, d);
        }
    }
}

void list_draw_scrollbar(struct osm_list *l)
{
    int cols = MAX(1, l->cols);
    int total_rows = (l->count + cols - 1) / cols;
    int rows = list_rows_visible(l);
    if (total_rows <= rows)
        return;
    int th = MAX(16, l->h * rows / total_rows);
    int ty = l->y + (l->h - th) * (l->top / cols) / MAX(1, total_rows - rows);
    gfx_rrect(l->x + l->w - 3, ty, 3, th, 1, GOLD_A(0.5));
}
