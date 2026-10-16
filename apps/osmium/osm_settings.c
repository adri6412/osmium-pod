/***************************************************************************
 *   Osmium Sound for FiiO M3K — settings
 *
 *   The Rockbox settings engine (a 2000-line table plus menus) is gone:
 *   global_settings is filled with sensible defaults here, the few values
 *   the user can change are kept in a small text file, and
 *   settings_apply() pushes everything to the drivers and the DSP.
 *
 *   File format (ROCKBOX_DIR "/osmium.cfg"):   key: value   one per line.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "settings.h"
#include "misc.h"
#include "sound.h"
#include "audio.h"
#include "backlight.h"
#include "powermgmt.h"
#include "rbunicode.h"
#include "rbpaths.h"
#include "dsp_core.h"
#include "playback.h"
#include "ata_idle_notify.h"
#include "language.h"
#include "talk.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "usb.h"
#include "string-extra.h"
#ifdef HAVE_ALBUMART
#include "albumart.h"
#endif
#include "osm_settings.h"

struct user_settings global_settings;
struct system_status global_status;

#define OSM_CFG_FILE      ROCKBOX_DIR "/osmium.cfg"
#define OSM_CFG_TMP_FILE  ROCKBOX_DIR "/osmium.cfg.tmp"

/* ── what is saved ──────────────────────────────────────────────────── */

enum cfg_type { CFG_INT, CFG_UINT, CFG_BOOL, CFG_STR };

struct cfg_item
{
    const char *name;
    void *var;
    enum cfg_type type;
    int size;           /* CFG_STR: buffer size */
};

#define CI(n, v)  { n, &global_settings.v, CFG_INT,  0 }
#define CU(n, v)  { n, &global_settings.v, CFG_UINT, 0 }
#define CB(n, v)  { n, &global_settings.v, CFG_BOOL, 0 }
#define CS(n, v)  { n, global_settings.v, CFG_STR, sizeof(global_settings.v) }
#define SI(n, v)  { n, &global_status.v, CFG_INT,  0 }
#define SU(n, v)  { n, &global_status.v, CFG_UINT, 0 }

static const struct cfg_item cfg_items[] =
{
    /* status: what was playing, the volume */
    SI("volume", volume),
    SI("resume index", resume_index),
    SU("resume crc32", resume_crc32),
    SU("resume elapsed", resume_elapsed),
    SU("resume offset", resume_offset),
#ifdef HAVE_PITCHCONTROL
    SI("resume pitch", resume_pitch),
    SI("resume speed", resume_speed),
#endif
    SI("runtime", runtime),
    SI("top runtime", topruntime),
#ifdef HAVE_DIRCACHE
    SI("dircache size", dircache_size),
#endif

    /* audio */
    CI("volume limit", volume_limit),
    CI("balance", balance),
    CI("bass", bass),
    CI("treble", treble),
    CI("channels", channel_config),
    CI("stereo width", stereo_width),
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    CI("roll off", roll_off),
#endif
#ifdef AUDIOHW_HAVE_POWER_MODE
    CI("power mode", power_mode),
#endif
#ifdef HAVE_PLAY_FREQ
    CI("playback frequency", play_frequency),
#endif
    CI("replaygain type", replaygain_settings.type),
    CB("replaygain noclip", replaygain_settings.noclip),
    CI("replaygain preamp", replaygain_settings.preamp),
#ifdef HAVE_CROSSFADE
    CI("crossfade", crossfade),
    CI("crossfade fade in duration", crossfade_fade_in_duration),
    CI("crossfade fade out duration", crossfade_fade_out_duration),
#endif
    CI("crossfeed", crossfeed),
    CB("eq enabled", eq_enabled),
    CU("eq precut", eq_precut),
    CI("eq band 0 gain", eq_band_settings[0].gain),
    CI("eq band 1 gain", eq_band_settings[1].gain),
    CI("eq band 2 gain", eq_band_settings[2].gain),
    CI("eq band 3 gain", eq_band_settings[3].gain),
    CI("eq band 4 gain", eq_band_settings[4].gain),
    CI("eq band 5 gain", eq_band_settings[5].gain),
    CI("eq band 6 gain", eq_band_settings[6].gain),
    CI("eq band 7 gain", eq_band_settings[7].gain),
    CI("eq band 8 gain", eq_band_settings[8].gain),
    CI("eq band 9 gain", eq_band_settings[9].gain),
    CB("dithering", dithering_enabled),
    CI("afr", afr_enabled),
    CI("pbe", pbe),

    /* playback */
    CI("repeat", repeat_mode),
    CB("shuffle", playlist_shuffle),
    CB("fade on stop", fade_on_stop),
    CB("cuesheet", cuesheet),
    CI("resume rewind", resume_rewind),
    CI("pause rewind", pause_rewind),
#if defined(HAVE_HEADPHONE_DETECTION)
    CI("unplug mode", unplug_mode),
#endif
#ifdef HAVE_ALBUMART
    CI("album art", album_art),
#endif
    CB("playback log", playback_log),
    CI("beep", beep),

    /* database */
#ifdef HAVE_TAGCACHE
#ifdef HAVE_TC_RAMCACHE
    CI("tagcache ram", tagcache_ram),
#endif
    CB("tagcache autoupdate", tagcache_autoupdate),
    CB("runtimedb", runtimedb),
    CS("database scan paths", tagcache_scan_paths),
#endif
#ifdef HAVE_DIRCACHE
    CB("dircache", dircache),
#endif

    /* power and display */
    CI("idle poweroff", poweroff),
    CI("time format", timeformat),
    CI("backlight timeout", backlight_timeout),
#if CONFIG_CHARGING
    CI("backlight timeout plugged", backlight_timeout_plugged),
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    CI("brightness", brightness),
#endif
#ifdef HAVE_BUTTON_LIGHT
    CI("buttonlight timeout", buttonlight_timeout),
#endif
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    CI("buttonlight brightness", buttonlight_brightness),
#endif
#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    CI("touchpad sensitivity", touchpad_sensitivity),
#endif

    /* the interface */
    CI("ui language", osm_language),
    CI("now playing view", osm_np_view),
    CI("vu meter look", osm_vu_skin),
    CI("album view", osm_album_view),
    CB("auto open now playing", osm_autoexpand),
    CB("wheel volume", osm_wheel_volume),
};

/* ── defaults ───────────────────────────────────────────────────────── */

/* a low shelf, eight peaks and a high shelf, as Rockbox always had */
#define EQ_PK EQ_FILTER_PEAK
static const struct eq_band_setting eq_defaults[EQ_NUM_BANDS] =
{
    { EQ_FILTER_LOW_SHELF, 32, 7, 0 },
    { EQ_PK, 64, 10, 0 },   { EQ_PK, 125, 10, 0 },  { EQ_PK, 250, 10, 0 },
    { EQ_PK, 500, 10, 0 },  { EQ_PK, 1000, 10, 0 }, { EQ_PK, 2000, 10, 0 },
    { EQ_PK, 4000, 10, 0 }, { EQ_PK, 8000, 10, 0 },
    { EQ_FILTER_HIGH_SHELF, 16000, 7, 0 },
};
#undef EQ_PK

void settings_reset(void)
{
    struct user_settings *s = &global_settings;
    memset(s, 0, sizeof(*s));

    /* audio */
    global_status.volume = sound_default(SOUND_VOLUME);
    s->volume_limit = sound_max(SOUND_VOLUME);
    s->balance = sound_default(SOUND_BALANCE);
#ifdef AUDIOHW_HAVE_BASS
    s->bass = sound_default(SOUND_BASS);
#endif
#ifdef AUDIOHW_HAVE_TREBLE
    s->treble = sound_default(SOUND_TREBLE);
#endif
    s->channel_config = sound_default(SOUND_CHANNELS);
    s->stereo_width = sound_default(SOUND_STEREO_WIDTH);
#ifdef AUDIOHW_HAVE_BASS_CUTOFF
    s->bass_cutoff = sound_default(SOUND_BASS_CUTOFF);
#endif
#ifdef AUDIOHW_HAVE_TREBLE_CUTOFF
    s->treble_cutoff = sound_default(SOUND_TREBLE_CUTOFF);
#endif
#ifdef AUDIOHW_HAVE_DEPTH_3D
    s->depth_3d = sound_default(SOUND_DEPTH_3D);
#endif
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    s->roll_off = sound_default(SOUND_FILTER_ROLL_OFF);
#endif
#ifdef AUDIOHW_HAVE_POWER_MODE
    s->power_mode = sound_default(SOUND_POWER_MODE);
#endif
#ifdef HAVE_CROSSFADE
    s->crossfade = CROSSFADE_ENABLE_OFF;
    s->crossfade_fade_in_duration = 2;
    s->crossfade_fade_out_duration = 2;
#endif
    s->replaygain_settings.type = REPLAYGAIN_SHUFFLE;
    s->replaygain_settings.noclip = false;
    s->replaygain_settings.preamp = 0;
    s->crossfeed = 0;
    s->crossfeed_direct_gain = (unsigned)-15;
    s->crossfeed_cross_gain = (unsigned)-60;
    s->crossfeed_hf_attenuation = (unsigned)-160;
    s->crossfeed_hf_cutoff = 700;
    s->eq_enabled = false;
    s->eq_precut = 0;
    memcpy(s->eq_band_settings, eq_defaults, sizeof(eq_defaults));
    s->compressor_settings.threshold = 0;
    s->compressor_settings.makeup_gain = 1;
    s->compressor_settings.ratio = 1;
    s->compressor_settings.knee = 1;
    s->compressor_settings.attack_time = 5;
    s->compressor_settings.release_time = 500;
    s->surround_balance = 35;
    s->surround_fx1 = 3400;
    s->surround_fx2 = 320;
    s->surround_mix = 50;
    s->pbe_precut = -25;
#ifdef HAVE_PERCEPTUAL_VOLUME
    s->volume_adjust_mode = VOLUME_ADJUST_DIRECT;
    s->volume_adjust_norm_steps = 50;
#endif

    /* playback */
    s->repeat_mode = REPEAT_OFF;
    s->next_folder = FOLDER_ADVANCE_OFF;
    s->fade_on_stop = true;
    s->play_selected = true;
    s->cuesheet = true;
    s->car_adapter_mode_delay = 5;
    s->ff_rewind_min_step = 1;
    s->ff_rewind_accel = 2;
    s->max_files_in_playlist = 10000;
#ifdef HAVE_ALBUMART
    s->album_art = AA_PREFER_EMBEDDED;
#endif
#if defined(HAVE_HEADPHONE_DETECTION) || defined(HAVE_LINEOUT_DETECTION)
    s->unplug_mode = 1;         /* pause when the phones come out */
#endif
    strcpy(s->start_directory, "/");
    s->talk_mixer_amp = 100;

    /* storage / database */
#ifdef HAVE_DIRCACHE
    s->dircache = true;
#endif
#ifdef HAVE_TAGCACHE
#ifdef HAVE_TC_RAMCACHE
    s->tagcache_ram = TAGCACHE_RAM_ON;
#endif
    s->tagcache_autoupdate = true;
    s->runtimedb = true;
    strcpy(s->tagcache_scan_paths, "/");
    strcpy(s->tagcache_db_path, ROCKBOX_DIR);
#endif
    s->default_codepage = 14;   /* UTF-8 */

    /* power / display */
    s->poweroff = 10;
    s->backlight_timeout = 15;
#if CONFIG_CHARGING
    s->backlight_timeout_plugged = 15;
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    s->brightness = DEFAULT_BRIGHTNESS_SETTING;
#endif
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
    s->backlight_fade_in = 300;
    s->backlight_fade_out = 2000;
#elif defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
    s->backlight_fade_in = true;
    s->backlight_fade_out = true;
#endif
#ifdef HAVE_BUTTON_LIGHT
    s->buttonlight_timeout = 5;
#endif
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    s->buttonlight_brightness = DEFAULT_BRIGHTNESS_SETTING;
#endif
#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    s->touchpad_sensitivity = DEFAULT_TOUCHPAD_SENSITIVITY_SETTING;
#endif
#if defined(HAVE_USB_POWER) && !defined(USB_NONE) && !defined(SIMULATOR)
    s->usb_mode = USBMODE_DEFAULT;
#endif

    /* interface */
    s->osm_language = OSM_LANG_ENGLISH;
    s->osm_np_view = OSM_NP_COVER;
    s->osm_vu_skin = 0;
    s->osm_album_view = 0;
    s->osm_autoexpand = true;
    s->osm_wheel_volume = false;

    global_status.resume_index = -1;
    global_status.font_id[SCREEN_MAIN] = FONT_SYSFIXED;
}

/* ── load / save ────────────────────────────────────────────────────── */

static const struct cfg_item *find_item(const char *name)
{
    for (size_t i = 0; i < ARRAYLEN(cfg_items); i++)
        if (!strcasecmp(cfg_items[i].name, name))
            return &cfg_items[i];
    return NULL;
}

static bool load_file(const char *path)
{
    char line[160];
    char *name, *value;
    int fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof line) > 0)
    {
        if (!settings_parseline(line, &name, &value))
            continue;
        const struct cfg_item *it = find_item(name);
        if (!it)
            continue;
        switch (it->type)
        {
            case CFG_INT:
                *(int *)it->var = atoi(value);
                break;
            case CFG_UINT:
                *(unsigned int *)it->var = strtoul(value, NULL, 10);
                break;
            case CFG_BOOL:
                *(bool *)it->var = !strcasecmp(value, "on") ||
                                   !strcmp(value, "1") ||
                                   !strcasecmp(value, "true");
                break;
            case CFG_STR:
                strmemccpy(it->var, value, it->size);
                break;
        }
    }
    close(fd);
    return true;
}

void settings_load(void)
{
    load_file(OSM_CFG_FILE);
    /* sanity */
    if (global_settings.max_files_in_playlist < 1000)
        global_settings.max_files_in_playlist = 1000;
}

static bool write_file(void)
{
    int fd = open(OSM_CFG_TMP_FILE, O_CREAT|O_WRONLY|O_TRUNC, 0666);
    if (fd < 0)
        return false;

    fdprintf(fd, "# Osmium Sound settings — written by the player\n");
    for (size_t i = 0; i < ARRAYLEN(cfg_items); i++)
    {
        const struct cfg_item *it = &cfg_items[i];
        switch (it->type)
        {
            case CFG_INT:
                fdprintf(fd, "%s: %d\n", it->name, *(int *)it->var);
                break;
            case CFG_UINT:
                fdprintf(fd, "%s: %u\n", it->name, *(unsigned int *)it->var);
                break;
            case CFG_BOOL:
                fdprintf(fd, "%s: %s\n", it->name, *(bool *)it->var ? "on" : "off");
                break;
            case CFG_STR:
                fdprintf(fd, "%s: %s\n", it->name, (char *)it->var);
                break;
        }
    }
    close(fd);
    remove(OSM_CFG_FILE);
    return rename(OSM_CFG_TMP_FILE, OSM_CFG_FILE) == 0;
}

/* written when the storage is spinning anyway, or when forced */
static void flush_callback(void)
{
    update_runtime();
    write_file();
}

void status_save(bool force)
{
    if (force)
    {
        unregister_storage_idle_func(flush_callback, false);
        flush_callback();
    }
    else
        register_storage_idle_func(flush_callback);
}

int settings_save(void)
{
    register_storage_idle_func(flush_callback);
    return 0;
}

/* ── runtime counters (battery screen) ──────────────────────────────── */

static long lasttime = 0;

void update_runtime(void)
{
    int elapsed_secs = (current_tick - lasttime) / HZ;
    global_status.runtime += elapsed_secs;
    lasttime += elapsed_secs * HZ;
    if (global_status.runtime > global_status.topruntime)
        global_status.topruntime = global_status.runtime;
}

void reset_runtime(void)
{
    update_runtime();
    lasttime = current_tick;
    global_status.runtime = 0;
}

/* ── applying ───────────────────────────────────────────────────────── */

void sound_settings_apply(void)
{
#ifdef AUDIOHW_HAVE_BASS
    sound_set(SOUND_BASS, global_settings.bass);
#endif
#ifdef AUDIOHW_HAVE_TREBLE
    sound_set(SOUND_TREBLE, global_settings.treble);
#endif
    sound_set(SOUND_BALANCE, global_settings.balance);
    sound_set(SOUND_VOLUME, global_status.volume);
    sound_set(SOUND_CHANNELS, global_settings.channel_config);
    sound_set(SOUND_STEREO_WIDTH, global_settings.stereo_width);
#ifdef AUDIOHW_HAVE_BASS_CUTOFF
    sound_set(SOUND_BASS_CUTOFF, global_settings.bass_cutoff);
#endif
#ifdef AUDIOHW_HAVE_TREBLE_CUTOFF
    sound_set(SOUND_TREBLE_CUTOFF, global_settings.treble_cutoff);
#endif
#ifdef AUDIOHW_HAVE_DEPTH_3D
    sound_set(SOUND_DEPTH_3D, global_settings.depth_3d);
#endif
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    sound_set(SOUND_FILTER_ROLL_OFF, global_settings.roll_off);
#endif
#ifdef AUDIOHW_HAVE_POWER_MODE
    sound_set(SOUND_POWER_MODE, global_settings.power_mode);
#endif
}

/* the DSP chain: replaygain, crossfeed, EQ and friends */
void osm_dsp_settings_apply(void)
{
    replaygain_update();
    dsp_set_crossfeed_type(global_settings.crossfeed);
    dsp_set_crossfeed_direct_gain(global_settings.crossfeed_direct_gain);
    dsp_set_crossfeed_cross_params(global_settings.crossfeed_cross_gain,
                                   global_settings.crossfeed_hf_attenuation,
                                   global_settings.crossfeed_hf_cutoff);
    dsp_eq_enable(global_settings.eq_enabled);
    dsp_set_eq_precut(global_settings.eq_precut);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        dsp_set_eq_coefs(i, &global_settings.eq_band_settings[i]);
    dsp_dither_enable(global_settings.dithering_enabled);
    dsp_surround_set_balance(global_settings.surround_balance);
    dsp_surround_set_cutoff(global_settings.surround_fx1, global_settings.surround_fx2);
    dsp_surround_mix(global_settings.surround_mix);
    dsp_surround_enable(global_settings.surround_enabled);
    dsp_afr_enable(global_settings.afr_enabled);
    dsp_pbe_precut(global_settings.pbe_precut);
    dsp_pbe_enable(global_settings.pbe);
#ifdef HAVE_PITCHCONTROL
    dsp_timestretch_enable(global_settings.timestretch_enabled);
#endif
    dsp_set_compressor(&global_settings.compressor_settings);
}

void osm_display_settings_apply(void)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    backlight_set_brightness(global_settings.brightness);
#endif
#ifdef HAVE_BACKLIGHT
    backlight_set_timeout(global_settings.backlight_timeout);
#if CONFIG_CHARGING
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
#endif
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING) \
    || defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
    backlight_set_fade_in(global_settings.backlight_fade_in);
    backlight_set_fade_out(global_settings.backlight_fade_out);
#endif
#endif
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    buttonlight_set_brightness(global_settings.buttonlight_brightness);
#endif
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_set_timeout(global_settings.buttonlight_timeout);
#endif
}

void settings_apply(bool read_disk)
{
    (void)read_disk;
    set_codepage(global_settings.default_codepage);

    sound_settings_apply();
#ifdef HAVE_DISK_STORAGE
    audio_set_buffer_margin(global_settings.buffer_margin);
#endif
    osm_display_settings_apply();
    set_poweroff_timeout(global_settings.poweroff);

#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    touchpad_set_sensitivity(global_settings.touchpad_sensitivity);
#endif
#if defined(HAVE_USB_POWER) && !defined(USB_NONE) && !defined(SIMULATOR)
    usb_set_mode(global_settings.usb_mode);
#endif

    /* Rockbox's own strings (used by the core for a few messages) */
    if (global_settings.osm_language == OSM_LANG_ITALIAN)
        lang_core_load(LANG_DIR "/italiano.lng");
    talk_init();

#ifdef HAVE_ALBUMART
    set_albumart_mode(global_settings.album_art);
#endif
#ifdef HAVE_PLAY_FREQ
    audio_set_playback_frequency(global_settings.play_frequency);
#endif
#ifdef HAVE_CROSSFADE
    audio_set_crossfade(global_settings.crossfade);
#endif
    osm_dsp_settings_apply();
}
