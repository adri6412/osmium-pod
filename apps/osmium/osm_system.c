/***************************************************************************
 *   Osmium Sound for FiiO M3K — system screens
 *
 *   What the user sees around the music: the boot splash with the Osmium
 *   Sound mark, the database preparation, the USB screen, the last frame
 *   before power off; plus the periodic chores of the interface.
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
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "button.h"
#include "backlight.h"
#include "usb.h"
#include "audio.h"
#include "playlist.h"
#include "powermgmt.h"
#include "power.h"
#include "settings.h"
#include "misc.h"
#include "version.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "timefuncs.h"
#include "osmium.h"
#include "osm_ui.h"
#include "osm_player.h"
#include "osm_screens.h"
#include "osm_library.h"

static int library_serial;

/* ── fonts ─────────────────────────────────────────────────────────── */

/* Characters DejaVu lacks (Japanese, Chinese, Korean...) come from a
 * Rockbox font on the card, if there is one: the first of these found. */
static const char *const fallback_fonts[] = {
    FONT_DIR "/osmium-fallback.fnt",
    FONT_DIR "/16-GNU-Unifont.fnt",
    FONT_DIR "/12-WenQangYi-Song.fnt",
    FONT_DIR "/16-WenQangYi-Unibit.fnt",
};

void osm_ui_init_fonts(void)
{
    for (unsigned i = 0; i < ARRAYLEN(fallback_fonts); i++)
    {
        if (!file_exists(fallback_fonts[i]))
            continue;
        int id = font_load(fallback_fonts[i]);
        if (id >= 0)
        {
            gfx_set_fallback_font(id);
            return;
        }
    }
    gfx_set_fallback_font(FONT_SYSFIXED);
}

/* ── boot ──────────────────────────────────────────────────────────── */

#ifdef OSM_BOOT_TRACE
#if !(CONFIG_PLATFORM & PLATFORM_HOSTED)
#include "backlight-target.h"
#endif
void osm_boot_trace(const char *step)
{
    static int line;
    static bool lit;
#if !(CONFIG_PLATFORM & PLATFORM_HOSTED)
    if (!lit)
    {
        backlight_hw_init();
        backlight_hw_on();
    }
#endif
    lit = true;
    lcd_set_viewport(NULL);
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_RGBPACK(255, 255, 0));
    lcd_set_background(LCD_RGBPACK(0, 0, 96));
    int h = font_get(FONT_SYSFIXED)->height;
    if (line == 0 || (line + 1) * h > LCD_HEIGHT)
    {
        lcd_clear_display();
        line = 0;
    }
    lcd_putsf(0, line++, "%ld %s", current_tick, step);
    lcd_update();
}
#endif

static void draw_logo_screen(const char *line, int step, int steps)
{
    gfx_noclip();
    gfx_clear(C_DARK);
    /* a soft gold halo behind the mark */
    for (int r = 110; r > 0; r -= 10)
        gfx_circle(SCR_W / 2, SCR_H / 2 - 18, r, GOLD_A(0.012));
    /* the stacked mark: bars and OSMIUM, SOUND spread underneath */
    int cap = 16;
    int w = gfx_brand(0, -100, cap, false);        /* measure */
    int x = (SCR_W - w) / 2, y = SCR_H / 2 - 34;
    gfx_brand(x, y, cap, false);
    const char *sound = "S O U N D";
    int sw = gfx_text_width_ls(OSM_FONT_B12, sound, 16 * 3);
    gfx_text_ls(OSM_FONT_B12, x + w - sw, y + cap + 10, sound, C_INK, 16 * 3);
    if (line)
    {
        gfx_text_fit(OSM_FONT_R12, 16, SCR_H - 62, SCR_W - 32, line, SILVER_A(0.8),
                     ALIGN_CENTER);
        if (steps > 0)
        {
            int bw = SCR_W - 80;
            gfx_rrect(40, SCR_H - 40, bw, 4, 2, WA(0.10));
            gfx_rrect(40, SCR_H - 40, bw * MIN(step, steps) / steps, 4, 2, C_GOLD);
        }
    }
    char ver[48];
    snprintf(ver, sizeof ver, "%s", rbversion);
    gfx_text_fit(OSM_FONT_R10, 16, SCR_H - 18, SCR_W - 32, ver, SILVER_A(0.35), ALIGN_CENTER);
    gfx_present();
}

void osm_boot_splash(void)
{
    gfx_init();
    draw_logo_screen(NULL, 0, 0);
#ifdef SIMULATOR
    sleep(HZ / 2);
#endif
}

void osm_boot_message(const char *msg, int step, int steps)
{
    draw_logo_screen(msg, step, steps);
}

void osm_fatal(const char *msg, int code)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%s (%d)", msg, code);
    draw_logo_screen(buf, 0, 0);
    sleep(HZ * 5);
    system_reboot();
}

/* ── USB ───────────────────────────────────────────────────────────── */

static void draw_usb(void)
{
    gfx_noclip();
    gfx_clear(C_DARK);
    gfx_brand(14, 14, 8, true);
    int cy = 120;
    gfx_circle(SCR_W / 2, cy, 46, GOLD_A(0.12));
    gfx_circle(SCR_W / 2, cy, 46 - 1, GOLD_A(0.06));
    gfx_icon(ICON_USB, 48, SCR_W / 2 - 24, cy - 24, C_GOLD);
    gfx_text_fit(OSM_FONT_B15, 12, cy + 64, SCR_W - 24, osm_str(S_USB_TITLE), C_WHITE,
                 ALIGN_CENTER);
    gfx_text_wrap(OSM_FONT_R12, 24, cy + 90, SCR_W - 48, 3, 17, osm_str(S_USB_SUB),
                  SILVER_A(0.7));
    /* the battery, charging */
    int lvl = battery_level();
    char b[24];
    snprintf(b, sizeof b, "%d%%", lvl);
    int y = SCR_H - 34;
    gfx_rrect(SCR_W / 2 - 50, y - 6, 100, 26, 13, WA(0.06));
    ui_draw_battery(SCR_W / 2 - 4, y + 2);
    gfx_text(OSM_FONT_R12, SCR_W / 2 + 4, y, b, C_WHITE);
    gfx_present();
}

void osm_usb_screen(void)
{
#ifdef SIMULATOR
    static bool inside = false;
    if (inside)
        return;
    inside = true;
#endif
    /* the font files stay open: close them while the computer has the card */
    font_disable_all();
    usb_acknowledge(SYS_USB_CONNECTED_ACK);
    while (1)
    {
        draw_usb();
        int b = button_get_w_tmo(HZ);
#ifdef SIMULATOR
        if (b && !(b & SYS_EVENT))
            break;
#endif
        if (b == SYS_USB_DISCONNECTED)
            break;
        if (b == SYS_CHARGER_DISCONNECTED)
            reset_runtime();
    }
    font_enable_all();
    settings_apply(true);
    playlist_resume();
    backlight_on();
    osm_dirty();
#ifdef SIMULATOR
    inside = false;
#endif
}

/* ── shutdown ──────────────────────────────────────────────────────── */

void osm_shutdown_screen(bool reboot, bool battery_low)
{
    draw_logo_screen(battery_low ? osm_str(S_BATTERY_LOW) :
                     osm_str(reboot ? S_RESTARTING : S_SHUTTING_DOWN), 0, 0);
}

/* ── the LCD driver's viewports ─────────────────────────────────────── */

/* The LCD driver calls this to swap a viewport's frame buffer (it lived in
 * the removed apps/gui/viewport.c). The Osmium UI draws in its own frame,
 * so there is only the default viewport. */
void viewport_set_buffer(struct viewport *vp, struct frame_buffer_t *buffer,
                         const enum screen_type screen)
{
    (void)screen;
    if (!vp)
        vp = lcd_current_viewport;
    vp->buffer = (buffer && buffer->elems == 0) ? NULL : buffer;
    lcd_init_viewport(vp);
}

/* ── the library ───────────────────────────────────────────────────── */

static void library_refresh(void)
{
    library_serial++;
    lib_thumbs_forget();
    osm_dirty();
}

void osm_library_changed(void)
{
    /* new files over USB: let the database find them */
    if (global_settings.tagcache_autoupdate && lib_ready())
        tagcache_update();
    library_refresh();
}

int osm_library_serial(void)
{
    return library_serial;
}

/* ── at boot: where we left off ────────────────────────────────────── */

void osm_resume_at_boot(void)
{
    /* The first start (no library yet): leave the memory to the database,
     * which needs it to finish building. */
    if (!file_exists(ROCKBOX_DIR "/database_idx.tcd"))
        return;
    /* the playlist is reloaded, the track queued and paused: Play resumes */
    if (playlist_resume() == -1 || global_status.resume_index < 0)
        return;
    unsigned long elapsed = global_status.resume_elapsed;
    unsigned long rewind = global_settings.resume_rewind * 1000;
    elapsed = elapsed > rewind ? elapsed - rewind : 0;
    playlist_start(global_status.resume_index, elapsed, global_status.resume_offset);
    audio_pause();
}

/* ── chores ────────────────────────────────────────────────────────── */

bool osm_ui_tick(void)
{
    static unsigned last_serial;
    static bool was_playing;
    static bool armed;
    static long since;
    bool redraw = false;

    /* Now Playing opens by itself once a track has been playing for a few
     * seconds with the keys left alone (App.qml, nowplaying-autoexpand) */
    if (player.serial != last_serial || player.playing != was_playing)
    {
        if (player.playing && (player.serial != last_serial || !was_playing))
        {
            armed = true;
            since = current_tick;
        }
        last_serial = player.serial;
        was_playing = player.playing;
        redraw = true;
    }
    if (!player.playing || osm_now_playing_shown())
        armed = false;
    if (armed && global_settings.osm_autoexpand && !osm_menu_open() && !osm_dialog_open())
    {
        long quiet_since = MAX(since, osm_last_input());
        if (TIME_AFTER(current_tick, quiet_since + HZ * 8))
        {
            armed = false;
            osm_show_now_playing(true);
            redraw = true;
        }
    }

    /* the clock in the status bar */
    static int last_minute = -1;
    struct tm *tm = get_time();
    if (tm->tm_min != last_minute)
    {
        last_minute = tm->tm_min;
        redraw = true;
    }

    /* The first start, a deleted database, or one left damaged (the card
     * pulled out while it was written): build it again. The engine checks
     * the files in the background first, give it a few seconds. */
    static bool db_kicked;
    static long db_since;
    if (!db_kicked && tagcache_is_initialized() && !lib_ready())
    {
        struct tagcache_stat *ts = tagcache_get_stat();
        if (!db_since)
            db_since = current_tick;
        bool missing = !file_exists(ROCKBOX_DIR "/database_idx.tcd");
        bool idle = ts && ts->commit_step == 0 && !ts->commit_delayed &&
                    ts->processed_entries == 0;
        if (idle && (missing || TIME_AFTER(current_tick, db_since + HZ * 4)))
        {
            db_kicked = true;
            tagcache_rebuild();
            osm_toast(ICON_REFRESH_CW, osm_str(S_LIB_UPDATING));
        }
    }

    /* the database was scanned but could not be written now (no memory
     * left while music plays): it is finished at the next start */
    static bool told_restart;
    struct tagcache_stat *st = tagcache_get_stat();
    if (!told_restart && st && st->commit_delayed)
    {
        told_restart = true;
        osm_toast(ICON_REFRESH_CW, osm_str(S_LIB_RESTART));
        redraw = true;
    }

    /* the database finished building in the background */
    static bool was_ready;
    bool ready = lib_ready();
    if (ready != was_ready)
    {
        was_ready = ready;
        library_refresh();
        redraw = true;
    }
    return redraw;
}

bool osm_ui_idle(void)
{
    return lib_thumbs_work();
}
