/***************************************************************************
 *   Osmium Sound for FiiO M3K — the interface framework
 *
 *   Screens sit on a stack (library pages, settings pages); Now Playing
 *   slides up over them like Osmium's expanded player. Overlays (toast,
 *   dialog, context menu) are drawn last and take the keys first.
 ****************************************************************************/
#ifndef OSM_UI_H
#define OSM_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "osm_gfx.h"
#include "osm_strings.h"

/* ── keys ──────────────────────────────────────────────────────────── */
enum osm_key {
    K_NONE = 0,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SELECT,
    K_BACK, K_MENU, K_PLAY, K_VOL_UP, K_VOL_DOWN, K_POWER,
    K_SCROLL_FWD, K_SCROLL_BACK,
};
enum osm_key_event {
    EV_PRESS,       /* went down */
    EV_REPEAT,      /* still down (auto repeat) */
    EV_SHORT,       /* released before it became long */
    EV_LONG,        /* held: once */
};

/* ── screens ───────────────────────────────────────────────────────── */
struct osm_screen {
    const char *name;
    void (*enter)(void *arg);       /* pushed (arg from osm_push) */
    void (*resume)(void);           /* back on top after a pop (optional) */
    void (*leave)(void);            /* popped (optional) */
    void (*draw)(void);             /* the whole frame */
    bool (*key)(int key, int ev);   /* true if handled */
    bool (*animating)(void);        /* wants ~30 frames a second (optional) */
    bool shows_mini_player;         /* the bar at the bottom of the library */
};

/* push a screen (slides in from the right), pop back (slides out) */
void osm_push(const struct osm_screen *s, void *arg);
void osm_pop(void);
void osm_pop_to_root(void);
void osm_replace(const struct osm_screen *s, void *arg);
const struct osm_screen *osm_top(void);
int  osm_depth(void);

/* Now Playing over everything (slides up) and back down */
void osm_show_now_playing(bool show);
bool osm_now_playing_shown(void);

/* ask for a new frame */
void osm_dirty(void);

/* ── overlays ──────────────────────────────────────────────────────── */
/* a pill at the top: an icon and a message, for ~1.5 s */
void osm_toast(enum osm_icon icon, const char *msg);
/* the volume toast (same pill, with a level) */
void osm_toast_volume(void);

/* a menu sheet from the bottom; `cb(index)` on select, -1 on back */
#define OSM_MENU_MAX 12
struct osm_menu_item {
    enum osm_icon icon;
    const char *label;
    bool checked;           /* a gold check at the right */
};
void osm_menu(const char *title, const struct osm_menu_item *items, int count,
              int selected, void (*cb)(int index, void *data), void *data);
bool osm_menu_open(void);

/* a modal question; `cb(true)` on the gold button */
void osm_dialog(const char *title, const char *message, const char *yes,
                const char *no, void (*cb)(bool yes, void *data), void *data);
bool osm_dialog_open(void);

/* blocking variants for the Rockbox core hooks */
bool osm_dialog_wait(const char *title, const char *message,
                     const char *yes, const char *no);

/* ── common drawing pieces ─────────────────────────────────────────── */
/* the header of library screens: the mark or a title, clock, battery */
void ui_draw_statusbar(bool brand, const char *title);
#define STATUSBAR_H 28
/* the mini player at the bottom of the library (MiniPlayer.qml) */
void ui_draw_mini_player(void);
#define MINIPLAYER_H 46
/* the area left for content between the two */
#define CONTENT_TOP     STATUSBAR_H
#define CONTENT_BOTTOM  (SCR_H - MINIPLAYER_H)

/* a battery glyph and the clock, right aligned at x */
int  ui_draw_battery(int x_right, int y);
void ui_format_time(char *buf, int len, unsigned long ms);

/* ── a scrolling list (rows drawn by the owner) ───────────────────── */
struct osm_list {
    int count;
    int sel;
    int top;            /* first visible row */
    int row_h;
    int x, y, w, h;     /* the viewport */
    int cols;           /* > 1: a grid of cells row_h tall */
    long last_move;     /* tick of the last move (scroll indicator) */
    int  speed;         /* moves in a row, for acceleration */
};
void list_init(struct osm_list *l, int count, int x, int y, int w, int h, int row_h);
/* up / down / wheel keys; true if handled (selection moved or edge) */
bool list_key(struct osm_list *l, int key, int ev);
void list_set_sel(struct osm_list *l, int sel);
/* call the row painter for each visible row: paint(index, x, y, w, h, selected) */
void list_draw(struct osm_list *l, void (*paint)(int i, int x, int y, int w, int h, bool sel, void *d), void *d);
/* the thin scrollbar on the right edge */
void list_draw_scrollbar(struct osm_list *l);

/* the standard row of the library lists: a card, a title, a subtitle and
 * something at the right (text, or an icon) */
void ui_draw_row(int x, int y, int w, int h, bool sel, enum osm_icon icon,
                 const char *title, const char *subtitle, const char *right,
                 bool chevron, bool playing);
/* the Osmium focus ring: gold border on a lighter card */
void ui_draw_focus(int x, int y, int w, int h, int r);

/* "⌂ › Albums › Abbey Road" under the status bar */
void ui_draw_crumbs(const char *const *parts, int count);
#define CRUMBS_H 24

/* the big letter while scrolling fast through a long list */
void ui_draw_jump_letter(const char *name);

/* "Now playing" -> "NOW PLAYING" (ASCII and Latin-1 letters) */
void ui_upper(char *dst, int len, const char *src);

/* the tick of the last key press */
long osm_last_input(void);

/* the key lock (power button) */
bool osm_keys_locked(void);

#endif
