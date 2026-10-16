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
#include "string-extra.h"
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

/* The boot animation of the Osmium Sound appliance (its Plymouth theme),
 * at the same 25 frames a second:
 *
 *   frames  8..17  a gold dot grows into the first bar
 *          22..44  the other six bars slide out of it to their places
 *          43..64  O S M I U M come in from the left, one after the other
 *          61..77  S O U N D fades in underneath
 *          80..    a light runs over the bars every 2.2 s (55 frames)
 *
 * The timings are measured on the appliance's frames. */
#define BA_FPS      25
#define BA_INTRO    80          /* the mark is complete */
#define BA_LOOP     55
#define BA_PALE     0xFFF8EBB8u /* the bars at the top of the light */

/* the first bar's height, frames 8..17, 1/1000 of full */
static const short ba_grow[] = { 128, 397, 603, 731, 808, 885, 910, 936, 962, 987 };
#define BA_GROW_FIRST 8

/* bars 1..6 on their way out, frames 22..44, 1/1000 of the way */
static const short ba_open[][BRAND_BARS - 1] = {
    {    0,    0,    0,    0,    0,    0 }, /* 22 */
    {    0,    0,    0,    0,    0,   63 },
    {    0,    0,    0,   32,  112,  193 },
    {    0,    0,   79,  161,  241,  324 }, /* 25 */
    {   67,  115,  209,  291,  370,  451 },
    {  157,  257,  339,  419,  495,  570 },
    {  306,  386,  466,  541,  608,  673 },
    {  434,  511,  583,  649,  704,  757 },
    {  553,  622,  683,  738,  779,  820 }, /* 30 */
    {  658,  715,  765,  806,  835,  867 },
    {  744,  789,  825,  857,  877,  902 },
    {  809,  842,  871,  894,  908,  928 },
    {  858,  883,  904,  922,  931,  947 },
    {  894,  912,  928,  942,  948,  961 }, /* 35 */
    {  921,  935,  947,  958,  961,  972 },
    {  942,  952,  961,  969,  970,  980 },
    {  955,  963,  971,  978,  978,  986 },
    {  966,  973,  979,  986,  984,  991 },
    {  975,  980,  986,  990,  988,  995 }, /* 40 */
    {  982,  986,  989,  994,  991,  997 },
    {  987,  990,  993,  997,  993,  999 },
    {  991,  992,  996,  999,  995, 1000 },
    {  993,  995,  997, 1000,  996, 1000 }, /* 44 */
};
#define BA_OPEN_FIRST 22

/* each letter starts a little after the one on its left (1/10 frame) */
static const short ba_letter_delay[BRAND_LETTERS] = { 0, 19, 39, 58, 71, 88 };
#define BA_LETTERS_START 425    /* 1/10 frame */
#define BA_LETTERS_LEN   111    /* 1/10 frame */
#define BA_LETTERS_SLIDE 192    /* from the left, 1/1000 of the cap height */

/* S O U N D, frames 61..77 */
static const unsigned char ba_sound[] = {
    23, 46, 69, 87, 107, 128, 145, 161, 179, 194, 207, 219, 230, 240, 245, 250,
};
#define BA_SOUND_FIRST 61

/* the light: a Gaussian (sigma 3.8 frames) by 1/4 frame, and when it
 * peaks on each bar (1/4 frame after the loop starts) */
static const unsigned char ba_light[] = {
    255, 254, 253, 250, 246, 242, 236, 229, 222, 214, 205, 196, 187, 177, 167,
    157, 147, 136, 126, 117, 107,  98,  89,  81,  73,  66,  59,  53,  47,  41,
     36,  32,  28,  24,  21,  18,  15,  13,  11,   9,   8,   7,   6,   5,   4,
      3,   3,   2,   2,
};
static const unsigned char ba_light_peak[BRAND_BARS] = { 28, 40, 48, 55, 61, 65, 68 };

static int ease_out_cubic(int t1000)
{
    if (t1000 <= 0)
        return 0;
    if (t1000 >= 1000)
        return 1000;
    int u = 1000 - t1000;
    return 1000 - u * u / 1000 * u / 1000;
}

static color_t mix(color_t a, color_t b, int w255)
{
    int r = ((a >> 16) & 0xFF) + ((((int)(b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * w255) / 255;
    int g = ((a >> 8) & 0xFF) + ((((int)(b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * w255) / 255;
    int bl = (a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * w255) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* the mark at frame f (-1: complete, still); returns the opacity of SOUND */
static int boot_look(int f, struct gfx_brand_look *look)
{
    if (f < 0)
        f = BA_INTRO - 1;

    int grow = f - BA_GROW_FIRST;
    look->bars_h1000 = grow < 0 ? 0 :
                       grow < (int)ARRAYLEN(ba_grow) ? ba_grow[grow] : 1000;

    int open = f - BA_OPEN_FIRST;
    look->bars_open[0] = 1000;
    for (int i = 1; i < BRAND_BARS; i++)
        look->bars_open[i] = open < 0 ? 0 :
                             open < (int)ARRAYLEN(ba_open) ? ba_open[open][i - 1] : 1000;

    for (int i = 0; i < BRAND_BARS; i++)
    {
        int w = 0;
        if (f >= BA_INTRO)
        {
            int d = (f - BA_INTRO) % BA_LOOP * 4 - ba_light_peak[i];
            if (d < 0)
                d = -d;
            w = d < (int)ARRAYLEN(ba_light) ? ba_light[d] : 0;
        }
        look->bars_color[i] = mix(C_GOLD, BA_PALE, w);
    }

    for (int i = 0; i < BRAND_LETTERS; i++)
    {
        int t = (f * 10 - BA_LETTERS_START - ba_letter_delay[i]) * 1000 / BA_LETTERS_LEN;
        int e = ease_out_cubic(t);
        look->letters_alpha[i] = t <= 0 ? 0 : e * 255 / 1000;
        look->letters_dx[i] = -BA_LETTERS_SLIDE * (1000 - e) / 1000;
    }

    int s = f - BA_SOUND_FIRST;
    return s < 0 ? 0 : s < (int)ARRAYLEN(ba_sound) ? ba_sound[s] : 255;
}

/* the boot screen: the mark at frame f (-1: complete), and the message
 * and progress of what the player is doing */
static void draw_logo_screen(int f, const char *line, int step, int steps)
{
    struct gfx_brand_look look;
    int sound_a = boot_look(f, &look);

    gfx_noclip();
    gfx_clear(C_DARK);
    /* the stacked mark: bars and OSMIUM, SOUND spread underneath */
    int cap = 16;
    int w = gfx_brand(0, -100, cap, false);        /* measure */
    int x = (SCR_W - w) / 2, y = SCR_H / 2 - 34;
    gfx_brand_ex(x, y, cap, false, &look);
    const char *sound = "S O U N D";
    int sw = gfx_text_width_ls(OSM_FONT_B12, sound, 16 * 3);
    if (sound_a > 0)
        gfx_text_ls(OSM_FONT_B12, x + w - sw, y + cap + 10, sound,
                    (C_INK & 0x00FFFFFFu) | ((color_t)sound_a << 24), 16 * 3);
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

/* While the system starts, a thread of its own plays the animation: the
 * boot itself blocks for seconds at a time (scanning the card, loading the
 * database). It owns the screen until osm_boot_splash_end(); the boot
 * messages only leave it the text to show. */
static struct {
    unsigned int thread;
    long start;                 /* tick of frame 0 */
    volatile bool finish;       /* stop once the mark is complete */
    volatile bool stop;         /* stop now */
    struct mutex lock;          /* the message below */
    char line[96];
    bool has_line;
    int step, steps;
} boot;
static long boot_stack[(16 * 1024) / sizeof(long)];

static void boot_draw(int f)
{
    char line[sizeof boot.line];
    bool has_line;
    int step, steps;
    mutex_lock(&boot.lock);
    has_line = boot.has_line;
    strmemccpy(line, boot.line, sizeof line);
    step = boot.step;
    steps = boot.steps;
    mutex_unlock(&boot.lock);
    draw_logo_screen(f, has_line ? line : NULL, step, steps);
}

static void boot_thread(void)
{
    while (!boot.stop)
    {
        int f = (current_tick - boot.start) * BA_FPS / HZ;
        if (boot.finish && f >= BA_INTRO)
            break;
        boot_draw(f);
        long next = boot.start + (f + 1) * HZ / BA_FPS;
        sleep(MAX(1, next - current_tick));
    }
}

void osm_boot_splash(void)
{
    gfx_init();
    mutex_init(&boot.lock);
    boot.start = current_tick;
#ifdef OSM_BOOT_TRACE
    /* the trace writes on the screen: no animation over it */
    draw_logo_screen(-1, NULL, 0, 0);
#else
    draw_logo_screen(0, NULL, 0, 0);
    boot.thread = create_thread(boot_thread, boot_stack, sizeof boot_stack, 0,
                                "osm boot" IF_PRIO(, PRIORITY_USER_INTERFACE)
                                IF_COP(, CPU));
#endif
}

void osm_boot_splash_end(bool complete)
{
    if (!boot.thread)
        return;
    boot.finish = true;
    if (!complete)
        boot.stop = true;
    thread_wait(boot.thread);
    boot.thread = 0;
}

void osm_boot_message(const char *msg, int step, int steps)
{
    mutex_lock(&boot.lock);
    boot.has_line = msg != NULL;
    if (msg)
        strmemccpy(boot.line, msg, sizeof boot.line);
    boot.step = step;
    boot.steps = steps;
    mutex_unlock(&boot.lock);
    if (!boot.thread)
        draw_logo_screen(-1, msg, step, steps);
}

void osm_fatal(const char *msg, int code)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%s (%d)", msg, code);
    osm_boot_splash_end(false);
    draw_logo_screen(-1, buf, 0, 0);
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

void osm_usb_screen(intptr_t seqnum)
{
#ifdef SIMULATOR
    static bool inside = false;
    if (inside)
        return;
    inside = true;
#endif
    /* USB at boot (no usable partition): the screen is ours now */
    osm_boot_splash_end(false);
    /* the font files stay open: close them while the computer has the card */
    font_disable_all();
    usb_acknowledge(SYS_USB_CONNECTED_ACK, seqnum);
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
    draw_logo_screen(-1, battery_low ? osm_str(S_BATTERY_LOW) :
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

/* ── the library being built ───────────────────────────────────────── */

/* While the database is (re)built the player has no library to show:
 * a full screen with the progress takes over until it is ready. */
bool osm_library_preparing(void)
{
#ifdef HAVE_TAGCACHE
    /* not for the instant the engine takes to check a sound database at
     * start: only once it has not been ready for a moment */
    static long since;
    if (!tagcache_is_initialized() || lib_ready())
    {
        since = 0;
        return false;
    }
    if (!since)
        since = current_tick ? current_tick : 1;
    return TIME_AFTER(current_tick, since + HZ);
#else
    return false;
#endif
}

bool osm_library_needs_restart(void)
{
#ifdef HAVE_TAGCACHE
    struct tagcache_stat *st = tagcache_get_stat();
    return osm_library_preparing() && st && st->commit_delayed;
#else
    return false;
#endif
}

void osm_library_screen_draw(void)
{
#ifdef HAVE_TAGCACHE
    struct tagcache_stat *st = tagcache_get_stat();
    bool restart = st->commit_delayed;
    bool saving = st->commit_step > 0;
    char line[64];

    gfx_noclip();
    gfx_clear(C_DARK);
    gfx_brand(14, 14, 8, true);

    /* the mark, breathing while the work goes on */
    int cy = 104;
    int phase = (current_tick / 4) % 32;
    int breath = phase < 16 ? phase : 31 - phase;            /* 0..15 */
    gfx_glow(SCR_W / 2, cy, 40, 14, (C_GOLD & 0x00FFFFFFu) | (color_t)(26 + breath * 2) << 24);
    gfx_circle(SCR_W / 2, cy, 44, GOLD_A(0.12));
    gfx_circle(SCR_W / 2, cy, 43, C_PANEL);
    gfx_icon(restart ? ICON_REFRESH_CW : ICON_LIBRARY, 44, SCR_W / 2 - 22, cy - 22,
             C_GOLD);

    int y = cy + 60;
    gfx_text_fit(OSM_FONT_B13, 8, y, SCR_W - 16,
                 osm_str(restart ? S_LIB_RESTART : S_BOOT_DATABASE), C_WHITE,
                 ALIGN_CENTER);
    y += 24;

    if (restart)
    {
        gfx_text_wrap(OSM_FONT_R12, 24, y, SCR_W - 48, 3, 17,
                      osm_str(S_PRESS_RESTART_WHY), SILVER_A(0.7));
        int bw = 180, by = SCR_H - 64;
        gfx_rrect((SCR_W - bw) / 2, by, bw, 34, 17, C_GOLD);
        gfx_text_fit(OSM_FONT_B13, (SCR_W - bw) / 2, by + 9, bw,
                     osm_str(S_LIB_PRESS_RESTART), C_BLACK, ALIGN_CENTER);
        return;
    }

    gfx_text_fit(OSM_FONT_R12, 12, y, SCR_W - 24,
                 osm_str(saving ? S_LIB_PHASE_SAVE : S_LIB_PHASE_SCAN),
                 SILVER_A(0.7), ALIGN_CENTER);
    y += 22;

    /* how far: the steps of the commit, or the files read so far */
    int max = tagcache_get_max_commit_step();
    if (saving)
        snprintf(line, sizeof line, osm_str(S_LIB_SAVE_PROGRESS),
                 MIN(st->commit_step, max), max);
    else
        snprintf(line, sizeof line, osm_str(S_LIB_SCAN_PROGRESS),
                 st->processed_entries);
    gfx_text_fit(OSM_FONT_B13, 12, y, SCR_W - 24, line, C_GOLD, ALIGN_CENTER);
    y += 26;

    /* the bar: filled while saving, a running segment while reading */
    int bx = 32, bw = SCR_W - 64, bh = 6;
    gfx_rrect(bx, y, bw, bh, 3, WA(0.10));
    if (saving && max > 0)
        gfx_rrect(bx, y, MAX(bh, bw * MIN(st->commit_step, max) / max), bh, 3,
                  C_GOLD);
    else
    {
        int seg = bw / 4;
        int pos = (current_tick * 2) % (bw + seg) - seg;
        int x0 = MAX(0, pos), x1 = MIN(bw, pos + seg);
        if (x1 > x0)
        {
            gfx_clip(bx, y, bw, bh);
            gfx_rrect(bx + x0, y, x1 - x0, bh, 3, C_GOLD);
            gfx_noclip();
        }
    }
    y += 16;

    /* the file being read */
    const char *cur = (const char *)st->curentry;
    if (!saving && cur)
    {
        char name[MAX_PATH];
        strlcpy(name, cur, sizeof name);
        const char *base = strrchr(name, '/');
        gfx_text_fit(OSM_FONT_R10, 16, y, SCR_W - 32, base ? base + 1 : name,
                     SILVER_A(0.45), ALIGN_CENTER);
    }

    gfx_text_fit(OSM_FONT_R10, 12, SCR_H - 30, SCR_W - 24,
                 osm_str(S_LIB_FIRST_TIME), SILVER_A(0.45), ALIGN_CENTER);
#endif
}

/* ── at boot: where we left off ────────────────────────────────────── */

void osm_resume_at_boot(void)
{
    /* The first start (no library yet): leave the memory to the database,
     * which needs it to finish building. */
    if (!file_exists(ROCKBOX_DIR "/database_idx.tcd") || !lib_ready())
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
     * pulled out or the battery gone while it was written): build it
     * again. Wait until the engine is idle - nothing read, nothing saved
     * for a few seconds - and try again if a build ends without a usable
     * database. */
    static int db_kicks;
    static int db_seen = -1;
    static long db_still;
    if (tagcache_is_initialized() && !lib_ready() && db_kicks < 3)
    {
        struct tagcache_stat *ts = tagcache_get_stat();
        if (ts->processed_entries != db_seen || ts->commit_step > 0 || ts->curentry)
        {
            db_seen = ts->processed_entries;
            db_still = current_tick;
        }
        bool missing = !file_exists(ROCKBOX_DIR "/database_idx.tcd");
        long wait = (db_kicks == 0 && missing) ? HZ / 2 : HZ * 4;
        if (ts->commit_step == 0 && !ts->commit_delayed &&
            TIME_AFTER(current_tick, db_still + wait))
        {
            db_kicks++;
            db_still = current_tick + HZ * 2;      /* give it time to start */
            tagcache_rebuild();
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

    /* while the database is built, the empty pages show how far it got */
    if (osm_library_preparing())
        redraw = true;

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
