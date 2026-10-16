/***************************************************************************
 *   Osmium Sound for FiiO M3K — Settings
 *
 *   SettingsTab.qml for a player: "Settings / Configure your Osmium Sound",
 *   then one card per section with its gold icon box, and inside each
 *   section rows with the value in gold at the right. Left and Right change
 *   a value, Select opens the choices (or switches a toggle).
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
#include "sound.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "backlight.h"
#include "powermgmt.h"
#include "power.h"
#include "misc.h"
#include "version.h"
#include "tagcache.h"
#include "dsp_core.h"
#include "disk.h"
#include "dir.h"
#include "string-extra.h"
#include "osm_ui.h"
#include "osm_screens.h"
#include "osm_player.h"
#include "osm_settings.h"
#include "osm_library.h"

enum { PG_ROOT, PG_PLAYBACK, PG_AUDIO, PG_EQ, PG_NP, PG_DISPLAY, PG_LIBRARY,
       PG_POWER, PG_ABOUT, PG_COUNT };

enum item_type { T_PAGE, T_CHOICE, T_INT, T_BOOL, T_ACTION, T_INFO };

struct item {
    int type;
    enum osm_str_id label;
    enum osm_icon icon;
    void *var;                          /* int * or bool * */
    int page;                           /* T_PAGE */
    /* T_CHOICE */
    const enum osm_str_id *names;
    const int *values;                  /* NULL: 0..n-1 */
    int n;
    /* T_INT */
    int min, max, step;
    void (*fmt)(int v, char *buf, int len);
    /* after a change / the action */
    void (*apply)(void);
    /* T_INFO: the text at the right */
    void (*info)(char *buf, int len);
    /* custom label (EQ bands) */
    void (*label_fn)(char *buf, int len);
    /* custom choice names (VU looks) */
    const char *(*name_fn)(int i);
};

/* ── formatting and applying ───────────────────────────────────────── */

static void fmt_db10(int v, char *b, int l)  { snprintf(b, l, "%s%d.%d dB", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10); }
static void fmt_pct(int v, char *b, int l)   { snprintf(b, l, "%d%%", v); }
static void fmt_sec(int v, char *b, int l)   { snprintf(b, l, osm_str(S_SECONDS), v); }
static void fmt_balance(int v, char *b, int l)
{
    if (v == 0) snprintf(b, l, "0");
    else snprintf(b, l, "%c%d%%", v < 0 ? 'L' : 'R', abs(v));
}
static void fmt_vol(int v, char *b, int l)   { format_sound_value(b, l, SOUND_VOLUME, v); }
static void fmt_bass(int v, char *b, int l)  { format_sound_value(b, l, SOUND_BASS, v); }
static void fmt_treble(int v, char *b, int l){ format_sound_value(b, l, SOUND_TREBLE, v); }
static void fmt_eq(int v, char *b, int l)    { fmt_db10(v, b, l); }
static void fmt_sens(int v, char *b, int l)  { snprintf(b, l, "%+d", v); }

static void ap_save(void)        { settings_save(); }
static void ap_sound(void)       { sound_settings_apply(); settings_save(); }
static void ap_dsp(void)         { osm_dsp_settings_apply(); settings_save(); }
static void ap_display(void)     { osm_display_settings_apply(); settings_save(); }
static void ap_volume(void)      { setvol(); settings_save(); }
static void ap_repeat(void)
{
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_flush_and_reload_tracks();
    settings_save();
}
static void ap_shuffle(void)
{
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        if (global_settings.playlist_shuffle)
            playlist_randomise(NULL, current_tick, true);
        else
            playlist_sort(NULL, true);
    }
    replaygain_update();
    settings_save();
}
static void ap_crossfade(void)
{
#ifdef HAVE_CROSSFADE
    audio_set_crossfade(global_settings.crossfade);
#endif
    settings_save();
}
static void ap_cuesheet(void)    { audio_set_cuesheet(global_settings.cuesheet); settings_save(); }
static void ap_poweroff(void)    { set_poweroff_timeout(global_settings.poweroff); settings_save(); }
static void ap_touchpad(void)
{
#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    touchpad_set_sensitivity(global_settings.touchpad_sensitivity);
#endif
    settings_save();
}
static void ap_language(void)
{
    settings_save();
    /* the core's own few strings follow, at the next start */
    osm_toast(ICON_GLOBE, osm_str(S_SETTINGS_SAVED));
}
static void ap_restart_note(void)
{
    settings_save();
    osm_toast(ICON_REFRESH_CW, osm_str(S_NEEDS_RESTART));
}

static void act_update(void)
{
#ifdef HAVE_TAGCACHE
    tagcache_update();
    osm_toast(ICON_REFRESH_CW, osm_str(S_LIB_UPDATING));
#endif
}
static void act_rebuild(void)
{
#ifdef HAVE_TAGCACHE
    if (osm_dialog_wait(osm_str(S_LIB_REBUILD), osm_str(S_REBUILD_Q),
                        osm_str(S_OK), osm_str(S_CANCEL)))
    {
        tagcache_rebuild();
        osm_toast(ICON_REFRESH_CW, osm_str(S_LIB_REBUILDING));
    }
#endif
}
static void act_eq_reset(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        global_settings.eq_band_settings[i].gain = 0;
    global_settings.eq_precut = 0;
    ap_dsp();
}
static void act_restart(void)
{
    if (osm_dialog_wait(osm_str(S_RESTART), osm_str(S_RESTART_Q), osm_str(S_RESTART),
                        osm_str(S_CANCEL)))
        sys_reboot();
}
static void act_poweroff(void)
{
    if (osm_dialog_wait(osm_str(S_POWER_OFF), osm_str(S_POWER_OFF_Q), osm_str(S_POWER_OFF),
                        osm_str(S_CANCEL)))
        sys_poweroff();
}

static void info_tracks(char *b, int l)
{
    snprintf(b, l, osm_str(S_TRACK_COUNT), lib_track_total());
}
static void info_version(char *b, int l) { strmemccpy(b, rbversion, l); }
static void info_model(char *b, int l)   { strmemccpy(b, MODEL_NAME, l); }
static void info_output(char *b, int l)  { strmemccpy(b, osm_str(S_HP_DAC), l); }
static void info_battery(char *b, int l)
{
    snprintf(b, l, "%d%%%s", battery_level(), charging_state() ? " \xE2\x9A\xA1" : "");
    if (charging_state())
        snprintf(b, l, "%d%% \xC2\xB7 %s", battery_level(), osm_str(S_CHARGING));
}
static void info_storage(char *b, int l)
{
    sector_t size = 0, free = 0;
    volume_size(IF_MV(0,) &size, &free);
    char fs[24], ts[24];
    output_dyn_value(fs, sizeof fs, (int64_t)free * 1024, byte_units, 4, true);
    output_dyn_value(ts, sizeof ts, (int64_t)size * 1024, byte_units, 4, true);
    snprintf(b, l, osm_str(S_FREE_OF), fs, ts);
}

/* ── choices ───────────────────────────────────────────────────────── */

static const enum osm_str_id n_lang[] = { S_LANG_ENGLISH, S_LANG_ITALIAN };
static const enum osm_str_id n_repeat[] = { S_REPEAT_OFF, S_REPEAT_ALL, S_REPEAT_ONE };
static const enum osm_str_id n_rg[] = { S_RG_TRACK, S_RG_ALBUM, S_RG_SHUFFLE, S_RG_OFF };
static const int v_rg[] = { REPLAYGAIN_TRACK, REPLAYGAIN_ALBUM, REPLAYGAIN_SHUFFLE, REPLAYGAIN_OFF };
static const enum osm_str_id n_xf[] = { S_XF_OFF, S_XF_AUTO, S_XF_MANUAL, S_XF_SHUFFLE, S_XF_ALWAYS };
static const int v_xf[] = { 0, 1, 2, 3, 5 };
static const enum osm_str_id n_unplug[] = { S_UNPLUG_NOTHING, S_UNPLUG_PAUSE, S_UNPLUG_PAUSE_RESUME };
static const enum osm_str_id n_channels[] = { S_CH_STEREO, S_CH_MONO, S_CH_STEREO, S_CH_LEFT, S_CH_RIGHT, S_CH_KARAOKE };
static const int v_channels[] = { SOUND_CHAN_STEREO, SOUND_CHAN_MONO, SOUND_CHAN_STEREO,
                                  SOUND_CHAN_MONO_LEFT, SOUND_CHAN_MONO_RIGHT, SOUND_CHAN_KARAOKE };
static const enum osm_str_id n_filter[] = { S_FILTER_SHARP, S_FILTER_SLOW, S_FILTER_SHORT_SHARP, S_FILTER_SHORT_SLOW };
static const enum osm_str_id n_power[] = { S_DAC_POWER_HIGH, S_DAC_POWER_LOW };
static const enum osm_str_id n_crossfeed[] = { S_OFF, S_CF_MEIER };
static const enum osm_str_id n_afr[] = { S_OFF, S_AFR_WEAK, S_AFR_MODERATE, S_AFR_STRONG };
static const enum osm_str_id n_view[] = { S_NP_VIEW_COVER, S_NP_VIEW_VU };
static const enum osm_str_id n_albumview[] = { S_AV_GRID, S_AV_LIST };
static const enum osm_str_id n_tf[] = { S_TF_24, S_TF_12 };
static const enum osm_str_id n_none[16];
static const int v_backlight[] = { 5, 10, 15, 20, 30, 45, 60, 120, 0 };
static const int v_buttonlight[] = { 0, 1, 2, 3, 5, 10, 15, 30, -1 };
static const int v_poweroff[] = { 0, 5, 10, 15, 30, 60 };
static const int v_tc_ram[] = { TAGCACHE_RAM_OFF, TAGCACHE_RAM_ON };

static const char *vu_name(int i)
{
    if ((unsigned)i >= OSM_VU_SKIN_COUNT)
        return "";
    return global_settings.osm_language == OSM_LANG_ITALIAN ? osm_vu_skins[i].name_it
                                                             : osm_vu_skins[i].name_en;
}

static char time_names[16][24];
static const char *backlight_name(int i)
{
    int v = v_backlight[i];
    if (v == 0)
        return osm_str(S_BACKLIGHT_ALWAYS);
    snprintf(time_names[i], sizeof time_names[i], osm_str(S_SECONDS), v);
    return time_names[i];
}
static char bl_names[16][24];
static const char *buttonlight_name(int i)
{
    int v = v_buttonlight[i];
    if (v == 0) return osm_str(S_OFF);
    if (v < 0) return osm_str(S_BACKLIGHT_ALWAYS);
    snprintf(bl_names[i], sizeof bl_names[i], osm_str(S_SECONDS), v);
    return bl_names[i];
}
static char po_names[8][24];
static const char *poweroff_name(int i)
{
    int v = v_poweroff[i];
    if (!v) return osm_str(S_OFF);
    snprintf(po_names[i], sizeof po_names[i], osm_str(S_MINUTES), v);
    return po_names[i];
}
static const char *tc_ram_name(int i) { return osm_str(i ? S_ON : S_OFF); }

/* the band labels of the equalizer */
#define EQ_LABEL(n) static void eq_label_##n(char *b, int l) { \
    int f = global_settings.eq_band_settings[n].cutoff; \
    if (f >= 1000) snprintf(b, l, "%d kHz", f / 1000); else snprintf(b, l, "%d Hz", f); }
EQ_LABEL(0) EQ_LABEL(1) EQ_LABEL(2) EQ_LABEL(3) EQ_LABEL(4)
EQ_LABEL(5) EQ_LABEL(6) EQ_LABEL(7) EQ_LABEL(8) EQ_LABEL(9)

/* ── the tree ──────────────────────────────────────────────────────── */

#define PAGE(lbl, ic, pg)          { .type = T_PAGE, .label = lbl, .icon = ic, .page = pg }
#define CHOICE(lbl, v, nm, vals, cnt, ap) { .type = T_CHOICE, .label = lbl, .var = &global_settings.v, \
                                     .names = nm, .values = vals, .n = cnt, .apply = ap }
#define CHOICEF(lbl, v, fn, vals, cnt, ap) { .type = T_CHOICE, .label = lbl, .var = &global_settings.v, \
                                     .names = n_none, .values = vals, .n = cnt, .apply = ap, .name_fn = fn }
#define INT(lbl, v, mn, mx, st, f, ap) { .type = T_INT, .label = lbl, .var = &global_settings.v, \
                                     .min = mn, .max = mx, .step = st, .fmt = f, .apply = ap }
#define BOOL(lbl, v, ap)           { .type = T_BOOL, .label = lbl, .var = &global_settings.v, .apply = ap }
#define ACTION(lbl, ic, fn)        { .type = T_ACTION, .label = lbl, .icon = ic, .apply = fn }
#define INFO(lbl, fn)              { .type = T_INFO, .label = lbl, .info = fn }
#define EQB(n) { .type = T_INT, .label = S_EQ_BAND, .var = &global_settings.eq_band_settings[n].gain, \
                 .min = -240, .max = 240, .step = 10, .fmt = fmt_eq, .apply = ap_dsp, .label_fn = eq_label_##n }

static struct item root_items[] = {
    CHOICE(S_SET_LANGUAGE, osm_language, n_lang, NULL, 2, ap_language),
    PAGE(S_SET_PLAYBACK, ICON_PLAY_FILL, PG_PLAYBACK),
    PAGE(S_SET_AUDIO, ICON_VOLUME_2, PG_AUDIO),
    PAGE(S_SET_EQUALIZER, ICON_SLIDERS, PG_EQ),
    PAGE(S_SET_NOWPLAYING, ICON_VU_METER, PG_NP),
    PAGE(S_SET_DISPLAY, ICON_MONITOR, PG_DISPLAY),
    PAGE(S_SET_LIBRARY, ICON_LIBRARY, PG_LIBRARY),
    PAGE(S_SET_POWER, ICON_POWER, PG_POWER),
    PAGE(S_SET_ABOUT, ICON_INFO, PG_ABOUT),
};

static struct item playback_items[] = {
    BOOL(S_SHUFFLE, playlist_shuffle, ap_shuffle),
    CHOICE(S_REPEAT, repeat_mode, n_repeat, NULL, 3, ap_repeat),
    CHOICE(S_REPLAYGAIN, replaygain_settings.type, n_rg, v_rg, 4, ap_dsp),
    BOOL(S_RG_NOCLIP, replaygain_settings.noclip, ap_dsp),
    INT(S_RG_PREAMP, replaygain_settings.preamp, -120, 120, 5, fmt_db10, ap_dsp),
#ifdef HAVE_CROSSFADE
    CHOICE(S_CROSSFADE, crossfade, n_xf, v_xf, 5, ap_crossfade),
#endif
    BOOL(S_FADE_ON_STOP, fade_on_stop, ap_save),
    INT(S_RESUME_REWIND, resume_rewind, 0, 60, 5, fmt_sec, ap_save),
    BOOL(S_CUESHEET, cuesheet, ap_cuesheet),
#ifdef HAVE_HEADPHONE_DETECTION
    CHOICE(S_UNPLUG, unplug_mode, n_unplug, NULL, 3, ap_save),
#endif
};

static struct item audio_items[] = {
    INT(S_VOLUME_LIMIT, volume_limit, 0, 0, 1, fmt_vol, ap_volume),        /* range at runtime */
    INT(S_BALANCE, balance, -100, 100, 5, fmt_balance, ap_sound),
    INT(S_BASS, bass, 0, 0, 1, fmt_bass, ap_sound),
    INT(S_TREBLE, treble, 0, 0, 1, fmt_treble, ap_sound),
    CHOICE(S_CHANNELS, channel_config, n_channels, v_channels, 6, ap_sound),
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    CHOICE(S_FILTER, roll_off, n_filter, NULL, 4, ap_sound),
#endif
#ifdef AUDIOHW_HAVE_POWER_MODE
    CHOICE(S_POWER_MODE, power_mode, n_power, NULL, 2, ap_sound),
#endif
    CHOICE(S_CROSSFEED, crossfeed, n_crossfeed, NULL, 2, ap_dsp),
    BOOL(S_DITHERING, dithering_enabled, ap_dsp),
    CHOICE(S_AFR, afr_enabled, n_afr, NULL, 4, ap_dsp),
    INT(S_PBE, pbe, 0, 100, 25, fmt_pct, ap_dsp),
};

static struct item eq_items[] = {
    BOOL(S_EQ_ENABLED, eq_enabled, ap_dsp),
    { .type = T_INT, .label = S_EQ_PRECUT, .var = &global_settings.eq_precut, .min = 0, .max = 240,
      .step = 5, .fmt = fmt_db10, .apply = ap_dsp },
    EQB(0), EQB(1), EQB(2), EQB(3), EQB(4), EQB(5), EQB(6), EQB(7), EQB(8), EQB(9),
    ACTION(S_EQ_RESET, ICON_REFRESH_CW, act_eq_reset),
};

static struct item np_items[] = {
    CHOICE(S_NP_VIEW, osm_np_view, n_view, NULL, 2, ap_save),
    CHOICEF(S_VU_LOOK, osm_vu_skin, vu_name, NULL, OSM_VU_SKIN_COUNT, ap_save),
    BOOL(S_AUTOEXPAND, osm_autoexpand, ap_save),
    BOOL(S_WHEEL_VOLUME, osm_wheel_volume, ap_save),
    CHOICE(S_ALBUM_VIEW, osm_album_view, n_albumview, NULL, 2, ap_save),
};

static struct item display_items[] = {
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    INT(S_BRIGHTNESS, brightness, MIN_BRIGHTNESS_SETTING, MAX_BRIGHTNESS_SETTING, BRIGHTNESS_STEP,
        fmt_pct, ap_display),
#endif
    CHOICEF(S_BACKLIGHT, backlight_timeout, backlight_name, v_backlight, 9, ap_display),
#ifdef HAVE_BUTTON_LIGHT
    CHOICEF(S_BUTTONLIGHT, buttonlight_timeout, buttonlight_name, v_buttonlight, 9, ap_display),
#endif
#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    INT(S_TOUCHPAD_SENS, touchpad_sensitivity, MIN_TOUCHPAD_SENSITIVITY_SETTING,
        MAX_TOUCHPAD_SENSITIVITY_SETTING, 5, fmt_sens, ap_touchpad),
#endif
    CHOICE(S_TIME_FORMAT, timeformat, n_tf, NULL, 2, ap_save),
};

static struct item library_items[] = {
    INFO(S_LIB_STATS, info_tracks),
    ACTION(S_LIB_UPDATE, ICON_REFRESH_CW, act_update),
    ACTION(S_LIB_REBUILD, ICON_LIBRARY, act_rebuild),
#ifdef HAVE_TAGCACHE
    BOOL(S_LIB_AUTOUPDATE, tagcache_autoupdate, ap_save),
#ifdef HAVE_TC_RAMCACHE
    CHOICEF(S_LIB_RAM, tagcache_ram, tc_ram_name, v_tc_ram, 2, ap_restart_note),
#endif
    BOOL(S_RUNTIMEDB, runtimedb, ap_save),
#endif
};

static struct item power_items[] = {
    CHOICEF(S_IDLE_POWEROFF, poweroff, poweroff_name, v_poweroff, 6, ap_poweroff),
    ACTION(S_SLEEP_TIMER, ICON_MOON, NULL),         /* opens the picker */
    ACTION(S_RESTART, ICON_REFRESH_CW, act_restart),
    ACTION(S_POWER_OFF, ICON_POWER, act_poweroff),
};

static struct item about_items[] = {
    INFO(S_MODEL, info_model),
    INFO(S_VERSION, info_version),
    INFO(S_BATTERY, info_battery),
    INFO(S_STORAGE, info_storage),
    INFO(S_OUTPUT, info_output),
};

static const struct { struct item *items; int count; enum osm_str_id title; } pages[PG_COUNT] = {
    [PG_ROOT]     = { root_items, ARRAYLEN(root_items), S_SETTINGS_TITLE },
    [PG_PLAYBACK] = { playback_items, ARRAYLEN(playback_items), S_SET_PLAYBACK },
    [PG_AUDIO]    = { audio_items, ARRAYLEN(audio_items), S_SET_AUDIO },
    [PG_EQ]       = { eq_items, ARRAYLEN(eq_items), S_SET_EQUALIZER },
    [PG_NP]       = { np_items, ARRAYLEN(np_items), S_SET_NOWPLAYING },
    [PG_DISPLAY]  = { display_items, ARRAYLEN(display_items), S_SET_DISPLAY },
    [PG_LIBRARY]  = { library_items, ARRAYLEN(library_items), S_SET_LIBRARY },
    [PG_POWER]    = { power_items, ARRAYLEN(power_items), S_SET_POWER },
    [PG_ABOUT]    = { about_items, ARRAYLEN(about_items), S_SET_ABOUT },
};

/* the ranges known only at run time */
static void fix_ranges(void)
{
    audio_items[0].min = sound_min(SOUND_VOLUME);
    audio_items[0].max = sound_max(SOUND_VOLUME);
    audio_items[0].step = sound_steps(SOUND_VOLUME) * 5;
    audio_items[2].min = sound_min(SOUND_BASS);
    audio_items[2].max = sound_max(SOUND_BASS);
    audio_items[2].step = sound_steps(SOUND_BASS);
    audio_items[3].min = sound_min(SOUND_TREBLE);
    audio_items[3].max = sound_max(SOUND_TREBLE);
    audio_items[3].step = sound_steps(SOUND_TREBLE);
}

/* ── values ────────────────────────────────────────────────────────── */

static int get_int(const struct item *it)
{
    return *(int *)it->var;
}

static int choice_index(const struct item *it)
{
    int v = get_int(it);
    if (!it->values)
        return MAX(0, MIN(v, it->n - 1));
    for (int i = 0; i < it->n; i++)
        if (it->values[i] == v)
            return i;
    return 0;
}

static const char *choice_name(const struct item *it, int i)
{
    if (it->name_fn)
        return it->name_fn(i);
    return osm_str(it->names[i]);
}

static void set_choice(const struct item *it, int i)
{
    i = (i + it->n) % it->n;
    *(int *)it->var = it->values ? it->values[i] : i;
    if (it->apply)
        it->apply();
}

static void value_text(const struct item *it, char *b, int l)
{
    b[0] = 0;
    switch (it->type)
    {
        case T_CHOICE: strmemccpy(b, choice_name(it, choice_index(it)), l); break;
        case T_INT:    it->fmt(get_int(it), b, l); break;
        case T_BOOL:   break;
        case T_INFO:   it->info(b, l); break;
    }
}

static void label_text(const struct item *it, char *b, int l)
{
    if (it->label_fn)
        it->label_fn(b, l);
    else
        strmemccpy(b, osm_str(it->label), l);
}

/* ── drawing ───────────────────────────────────────────────────────── */

/* the toggle of Osmium's settings rows */
static void draw_switch(int x, int y, bool on, bool sel)
{
    gfx_rrect(x, y, 34, 20, 10, on ? C_GOLD : WA(0.15));
    gfx_circle(on ? x + 24 : x + 10, y + 10, 7, on ? C_BLACK : (sel ? C_WHITE : C_SILVER));
}

static void paint_item(const struct item *it, int x, int y, int w, int h, bool sel)
{
    int cx = x + 6, cw = w - 12;
    if (sel)
        ui_draw_focus(cx, y + 2, cw, h - 4, 12);
    else
    {
        gfx_rrect(cx, y + 2, cw, h - 4, 12, C_SURFACE);
        gfx_rrect_border(cx, y + 2, cw, h - 4, 12, 1, C_BORDER);
    }
    char label[64], value[64];
    label_text(it, label, sizeof label);
    int tx = cx + 12;
    if (it->type == T_PAGE || it->type == T_ACTION)
    {
        gfx_rrect(cx + 8, y + (h - 28) / 2, 28, 28, 7, GOLD_A(0.15));
        gfx_icon(it->icon, 18, cx + 13, y + (h - 18) / 2, C_GOLD);
        tx = cx + 46;
    }
    if (it->type == T_CHOICE && it->label == S_SET_LANGUAGE)
    {
        gfx_rrect(cx + 8, y + (h - 28) / 2, 28, 28, 7, GOLD_A(0.15));
        gfx_icon(ICON_GLOBE, 18, cx + 13, y + (h - 18) / 2, C_GOLD);
        tx = cx + 46;
    }
    value_text(it, value, sizeof value);
    int right = cx + cw - 10;
    if (it->type == T_PAGE || it->type == T_CHOICE)
    {
        gfx_icon(ICON_CHEVRON_RIGHT, 14, right - 14, y + (h - 14) / 2, SILVER_A(0.5));
        right -= 20;
    }
    else if (it->type == T_BOOL)
    {
        draw_switch(right - 34, y + (h - 20) / 2, *(bool *)it->var, sel);
        right -= 42;
    }
    else if (it->type == T_INT && sel)
    {
        /* the arrows say Left / Right change it */
        gfx_icon(ICON_CHEVRON_RIGHT, 12, right - 12, y + (h - 12) / 2, C_GOLD);
        right -= 14;
    }
    int vw = 0;
    if (value[0])
    {
        vw = MIN(gfx_text_width(OSM_FONT_R12, value), (cw * 45) / 100);
        gfx_text_fit(OSM_FONT_R12, right - vw, y + (h - 15) / 2, vw, value,
                     it->type == T_INFO ? SILVER_A(0.7) : C_GOLD, ALIGN_RIGHT);
        if (it->type == T_INT && sel)
            gfx_icon(ICON_CHEVRON_LEFT, 14, right - vw - 16, y + (h - 14) / 2, C_GOLD);
        vw += (it->type == T_INT && sel) ? 18 : 6;
    }
    gfx_text_fit(OSM_FONT_R13, tx, y + (h - 16) / 2, right - vw - tx - 4, label,
                 C_WHITE, ALIGN_LEFT);
}

/* ── the settings home, in the third tab of the home screen ───────── */

static struct osm_list root_list;

void settings_home_enter(void)
{
    fix_ranges();
}

void settings_home_draw(int top)
{
    gfx_text(OSM_FONT_B22, 14, top + 6, osm_str(S_SETTINGS_TITLE), C_WHITE);
    gfx_text_fit(OSM_FONT_R11, 14, top + 34, SCR_W - 28, osm_str(S_SETTINGS_SUB),
                 SILVER_A(0.8), ALIGN_LEFT);
    root_list.count = ARRAYLEN(root_items);
    root_list.x = 0;
    root_list.y = top + 54;
    root_list.w = SCR_W - 2;
    root_list.h = CONTENT_BOTTOM - root_list.y - 2;
    root_list.row_h = 46;
    gfx_clip(0, root_list.y, SCR_W, root_list.h);
    for (int i = root_list.top; i < root_list.count; i++)
    {
        int y = root_list.y + (i - root_list.top) * root_list.row_h;
        if (y > root_list.y + root_list.h)
            break;
        paint_item(&root_items[i], root_list.x, y, root_list.w, root_list.row_h,
                   i == root_list.sel);
    }
    gfx_noclip();
    list_draw_scrollbar(&root_list);
}

static void choice_cb(int sel, void *data);
static const struct item *picking;

static void open_choices(const struct item *it)
{
    struct osm_menu_item m[OSM_MENU_MAX];
    int cur = choice_index(it);
    int n = MIN(it->n, OSM_MENU_MAX);
    for (int i = 0; i < n; i++)
    {
        m[i].icon = it->label == S_SET_LANGUAGE ? ICON_GLOBE :
                    it->label == S_VU_LOOK ? ICON_VU_METER : ICON_CHECK;
        if (m[i].icon == ICON_CHECK)
            m[i].icon = i == cur ? ICON_CHECK : ICON_SLIDERS;
        m[i].label = choice_name(it, i);
        m[i].checked = i == cur;
    }
    char title[64];
    label_text(it, title, sizeof title);
    picking = it;
    osm_menu(title, m, n, cur, choice_cb, NULL);
}

static void choice_cb(int sel, void *data)
{
    (void)data;
    if (sel >= 0 && picking)
        set_choice(picking, sel);
    picking = NULL;
    osm_dirty();
}

static void sleep_picker(void);

/* Select, Left, Right on an item; true if handled */
static bool item_key(const struct item *it, int key, int ev)
{
    bool sel = key == K_SELECT && ev == EV_SHORT;
    bool adj = (key == K_LEFT || key == K_RIGHT) && (ev == EV_PRESS || ev == EV_REPEAT);
    int dir = key == K_RIGHT ? 1 : -1;
    switch (it->type)
    {
        case T_PAGE:
            if (sel || (key == K_RIGHT && ev == EV_SHORT))
            {
                settings_open(it->page);
                return true;
            }
            return false;
        case T_CHOICE:
            if (sel)
                open_choices(it);
            else if (adj)
                set_choice(it, choice_index(it) + dir);
            else
                return false;
            osm_dirty();
            return true;
        case T_INT:
            if (adj)
            {
                int v = get_int(it) + dir * it->step;
                *(int *)it->var = MAX(it->min, MIN(it->max, v));
                if (it->apply)
                    it->apply();
                osm_dirty();
                return true;
            }
            return sel;
        case T_BOOL:
            if (sel || (adj && ev == EV_PRESS))
            {
                bool *b = it->var;
                *b = adj ? dir > 0 : !*b;
                if (it->apply)
                    it->apply();
                osm_dirty();
                return true;
            }
            return false;
        case T_ACTION:
            if (sel)
            {
                if (it->label == S_SLEEP_TIMER)
                    sleep_picker();
                else if (it->apply)
                    it->apply();
                osm_dirty();
                return true;
            }
            return false;
        case T_INFO:
            return sel;
    }
    return false;
}

bool settings_home_key(int key, int ev)
{
    if ((key == K_UP || key == K_SCROLL_BACK) && root_list.sel == 0 &&
        (ev == EV_PRESS || ev == EV_REPEAT))
        return false;               /* up to the tab bar */
    if (list_key(&root_list, key, ev))
    {
        osm_dirty();
        return true;
    }
    return item_key(&root_items[root_list.sel], key, ev);
}

/* ── a settings page ───────────────────────────────────────────────── */

static int page_stack[8];
static struct osm_list page_lists[8];
static int pdepth;

static void sleep_cb(int sel, void *d)
{
    static const int mins[] = { 0, 15, 30, 45, 60, 90, 120 };
    (void)d;
    if (sel >= 0)
        player_set_sleep(mins[sel]);
    osm_dirty();
}

static void sleep_picker(void)
{
    static const int mins[] = { 0, 15, 30, 45, 60, 90, 120 };
    static struct osm_menu_item m[7];
    static char labels[7][24];
    for (int i = 0; i < 7; i++)
    {
        if (mins[i])
            snprintf(labels[i], sizeof labels[i], osm_str(S_MINUTES), mins[i]);
        else
            strmemccpy(labels[i], osm_str(S_SLEEP_OFF), sizeof labels[i]);
        m[i] = (struct osm_menu_item){ ICON_MOON, labels[i], false };
    }
    m[0].checked = player_sleep_minutes() == 0;
    osm_menu(osm_str(S_SLEEP_TIMER), m, 7, 0, sleep_cb, NULL);
}

static void page_draw(void)
{
    int pg = page_stack[pdepth - 1];
    struct osm_list *l = &page_lists[pdepth - 1];
    gfx_clear(C_DARK);
    ui_draw_statusbar(true, NULL);
    const char *crumbs[3] = { osm_str(S_SETTINGS_TITLE), osm_str(pages[pg].title) };
    ui_draw_crumbs(crumbs, 2);
    gfx_clip(l->x, l->y, l->w, l->h);
    for (int i = l->top; i < l->count; i++)
    {
        int y = l->y + (i - l->top) * l->row_h;
        if (y > l->y + l->h)
            break;
        paint_item(&pages[pg].items[i], l->x, y, l->w, l->row_h, i == l->sel);
    }
    gfx_noclip();
    list_draw_scrollbar(l);
    if (pg == PG_ABOUT)
    {
        /* the mark under the facts */
        int y = l->y + l->count * l->row_h + 18;
        if (y < CONTENT_BOTTOM - 20)
            gfx_brand((SCR_W - 150) / 2, y, 9, true);
    }
    ui_draw_mini_player();
}

static bool page_key(int key, int ev)
{
    int pg = page_stack[pdepth - 1];
    struct osm_list *l = &page_lists[pdepth - 1];
    if (list_key(l, key, ev))
    {
        osm_dirty();
        return true;
    }
    if (item_key(&pages[pg].items[l->sel], key, ev))
        return true;
    if (key == K_LEFT && ev == EV_SHORT)
    {
        osm_pop();
        return true;
    }
    return false;
}

static void page_enter(void *arg)
{
    (void)arg;
}

static void page_leave(void)
{
    if (pdepth > 0)
        pdepth--;
}

void settings_open(int page)
{
    if (pdepth >= 8 || page <= PG_ROOT || page >= PG_COUNT)
        return;
    fix_ranges();
    page_stack[pdepth] = page;
    list_init(&page_lists[pdepth], pages[page].count, 0, STATUSBAR_H + CRUMBS_H + 4,
              SCR_W - 2, CONTENT_BOTTOM - STATUSBAR_H - CRUMBS_H - 6, 44);
    pdepth++;
    osm_push(&scr_settings, NULL);
}

const struct osm_screen scr_settings = {
    .name = "settings",
    .enter = page_enter,
    .leave = page_leave,
    .draw = page_draw,
    .key = page_key,
    .shows_mini_player = true,
};
