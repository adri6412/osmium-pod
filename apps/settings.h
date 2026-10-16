/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Stuart Martin
 *
 * Osmium Sound: trimmed down to what the Rockbox core (playback engine,
 * firmware drivers, tag database) and the Osmium UI actually use. The
 * settings table, the .cfg parser and every menu-related declaration are
 * gone; persistence lives in osmium/osm_settings.c.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stddef.h>
#include "inttypes.h"
#include "config.h"
#include "audiohw.h" /* for the AUDIOHW_* defines */
#include "button.h"
#include "audio.h"
#include "dsp_proc_settings.h"

#ifndef NB_SCREENS
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for(int i = 0; i < NB_SCREENS; i++)
#endif

/** Setting values defines **/
#define MAX_FILENAME 32
#define MAX_PATHNAME 80

#ifdef HAVE_CROSSFADE
enum {
    CROSSFADE_ENABLE_OFF = 0,
    CROSSFADE_ENABLE_AUTOSKIP,
    CROSSFADE_ENABLE_MANSKIP,
    CROSSFADE_ENABLE_SHUFFLE,
    CROSSFADE_ENABLE_SHUFFLE_OR_MANSKIP,
    CROSSFADE_ENABLE_ALWAYS,
};
#endif

enum {
    FOLDER_ADVANCE_OFF = 0,
    FOLDER_ADVANCE_NEXT,
    FOLDER_ADVANCE_RANDOM,
};

/* repeat mode options */
enum
{
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE,
    REPEAT_SHUFFLE,
#ifdef AB_REPEAT_ENABLE
    REPEAT_AB,
#endif
    NUM_REPEAT_MODES
};

/* single mode options */
enum {
    SINGLE_MODE_OFF = 0,
    SINGLE_MODE_TRACK,
    SINGLE_MODE_ALBUM,
    SINGLE_MODE_ALBUM_ARTIST,
    SINGLE_MODE_ARTIST,
    SINGLE_MODE_COMPOSER,
    SINGLE_MODE_GROUPING,
    SINGLE_MODE_GENRE
};

#ifdef HAVE_ALBUMART
enum
{
    AA_OFF = 0,
    AA_PREFER_EMBEDDED,
    AA_PREFER_IMAGE_FILE
};
#endif

enum
{
    TAGCACHE_RAM_OFF = 0,
    TAGCACHE_RAM_ON = 1,
    TAGCACHE_RAM_QUICK = 2
};

/* file and dir sort options */
enum { SORT_ALPHA, SORT_DATE, SORT_DATE_REVERSED, SORT_TYPE,
       SORT_ALPHA_REVERSED, SORT_TYPE_REVERSED, SORT_AS_FILE };

/* autoresume settings */
enum { AUTORESUME_NEXTTRACK_NEVER = 0, AUTORESUME_NEXTTRACK_ALWAYS,
       AUTORESUME_NEXTTRACK_CUSTOM};

/* duration of a key click, in ms (beep.c) */
#define KEYCLICK_DURATION 2

/** virtual pointers: a literal string, or a language string ID **/
#define VIRT_SIZE 0xFFFF /* more than enough for our string ID range */
/* offset from 0x0 slightly */
#define VIRT_PTR ((unsigned char*)sizeof(char*))

/* form a "virtual pointer" out of a language ID */
#define ID2P(id) (VIRT_PTR + id)

/* resolve a pointer which could be a virtualized ID or a literal */
#define P2STR(p) (char *)((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? str(p-VIRT_PTR) : p)

/* get the string ID from a virtual pointer, -1 if not virtual */
#define P2ID(p) ((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? p-VIRT_PTR : -1)

/** function prototypes **/
void reset_runtime(void);
void update_runtime(void);
void settings_load(void) INIT_ATTR;
void status_save(bool force);
int settings_save(void);
void settings_reset(void);
void settings_apply(bool read_disk);
void sound_settings_apply(void);

/** global_settings and global_status struct definitions **/

struct system_status
{
    int volume;     /* audio output volume in decibels range depends on the dac */
    int resume_index;  /* index in playlist (-1 for no active resume) */
    uint32_t resume_crc32; /* crc32 of the name of the file */
    uint32_t resume_elapsed; /* elapsed time in last file */
    uint32_t resume_offset; /* byte offset in mp3 file */
#ifdef HAVE_PITCHCONTROL
    int32_t resume_pitch;
    int32_t resume_speed;
#endif
    int runtime;       /* current runtime since last charge */
    int topruntime;    /* top known runtime */
#ifdef HAVE_DIRCACHE
    int dircache_size;      /* directory cache structure last size, 22 bits */
#endif
    int last_volume_change; /* tick the last volume change happened */
    int font_id[NB_SCREENS]; /* font id of the UI font for each screen */
    bool resume_modified; /* playlist is modified (=> warn before erase) */
};

struct user_settings
{
    /* ── audio ─────────────────────────────────────────────────────── */
    int balance;    /* stereo balance: -100 - +100 -100=left  0=bal +100=right  */
    int bass;       /* bass boost/cut in decibels                               */
    int treble;     /* treble boost/cut in decibels                             */
    int channel_config; /* Stereo, Mono, Custom, Mono left, Mono right, Karaoke */
    int stereo_width; /* 0-255% */
#ifdef AUDIOHW_HAVE_BASS_CUTOFF
    int bass_cutoff;
#endif
#ifdef AUDIOHW_HAVE_TREBLE_CUTOFF
    int treble_cutoff;
#endif
#ifdef HAVE_CROSSFADE
    int crossfade;                    /* CROSSFADE_ENABLE_*               */
    int crossfade_fade_in_delay;      /* Fade in delay (0-15s)             */
    int crossfade_fade_out_delay;     /* Fade out delay (0-15s)            */
    int crossfade_fade_in_duration;   /* Fade in duration (0-15s)          */
    int crossfade_fade_out_duration;  /* Fade out duration (0-15s)         */
    int crossfade_fade_out_mixmode;   /* Fade out mode (0=crossfade,1=mix) */
#endif
    struct replaygain_settings replaygain_settings;
    int crossfeed;                              /* crossfeed type */
    unsigned int crossfeed_direct_gain;         /* dB x 10 */
    unsigned int crossfeed_cross_gain;          /* dB x 10 */
    unsigned int crossfeed_hf_attenuation;      /* dB x 10 */
    unsigned int crossfeed_hf_cutoff;           /* Frequency in Hz */
    bool eq_enabled;
    unsigned int eq_precut;     /* dB */
    struct eq_band_setting eq_band_settings[EQ_NUM_BANDS];
    int  beep;              /* system beep volume when changing tracks etc. */
    bool dithering_enabled;
#ifdef HAVE_PITCHCONTROL
    bool timestretch_enabled;
#endif
    struct compressor_settings compressor_settings;
    int surround_enabled;
    int surround_balance;
    int surround_fx1;
    int surround_fx2;
    bool surround_method2;
    int surround_mix;
    int pbe;
    int pbe_precut;
    int afr_enabled;
#ifdef AUDIOHW_HAVE_DEPTH_3D
    int depth_3d;
#endif
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    int roll_off;
#endif
#ifdef AUDIOHW_HAVE_POWER_MODE
    int power_mode;
#endif
#ifdef AUDIOHW_HAVE_EQ
    struct hw_eq_band
    {
        int gain;
#ifdef AUDIOHW_HAVE_EQ_FREQUENCY
        int frequency;
#endif
#ifdef AUDIOHW_HAVE_EQ_WIDTH
        int width;
#endif
    } hw_eq_bands[AUDIOHW_EQ_BAND_NUM];
#endif
#ifdef HAVE_PLAY_FREQ
    int play_frequency; /* core audio output frequency selection */
#endif
    int volume_limit; /* maximum volume limit */
#ifdef HAVE_PERCEPTUAL_VOLUME
    int volume_adjust_mode;
    int volume_adjust_norm_steps;
#endif
#if defined(HAVE_EROS_QN_CODEC)
    int hp_lo_select;
#endif

    /* ── playback / playlist ───────────────────────────────────────── */
    int  repeat_mode;           /* REPEAT_* */
    bool playlist_shuffle;
    int  next_folder;           /* FOLDER_ADVANCE_* */
    bool constrain_next_folder;
    bool fade_on_stop;
    bool play_selected;
    int  single_mode;           /* SINGLE_MODE_* */
    bool party_mode;
    bool cuesheet;
    bool car_adapter_mode;
    int  car_adapter_mode_delay;
    int  resume_rewind;         /* seconds rewound when resuming */
    int  pause_rewind;
    int  ff_rewind_min_step;
    int  ff_rewind_accel;
    int  max_files_in_playlist;
    int  skip_length;
#ifdef HAVE_ALBUMART
    int  album_art;             /* AA_* */
#endif
#if defined(HAVE_HEADPHONE_DETECTION) || defined(HAVE_LINEOUT_DETECTION)
    int  unplug_mode;           /* pause on headphone unplug */
    bool unplug_autoresume;
#endif
    bool playback_log;
    char start_directory[MAX_PATHNAME+1];
    int  sort_dir;
    unsigned char lang_file[MAX_FILENAME+1];

    /* ── voice (kept for the core, unused by the Osmium UI) ────────── */
    bool talk_menu;
    int  talk_dir;
    bool talk_dir_clip;
    int  talk_file;
    bool talk_file_clip;
    bool talk_filetype;
    bool talk_battery_level;
    int  talk_mixer_amp;

    /* ── storage / database ────────────────────────────────────────── */
#ifdef HAVE_DISK_STORAGE
    int disk_spindown;
    int buffer_margin;
#endif
#ifdef HAVE_DIRCACHE
    bool dircache;
#endif
#ifdef HAVE_TAGCACHE
#ifdef HAVE_TC_RAMCACHE
    int tagcache_ram;
#endif
    bool tagcache_autoupdate;
    bool autoresume_enable;
    int autoresume_automatic;
    unsigned char autoresume_paths[MAX_PATHNAME+1];
    bool runtimedb;
    unsigned char tagcache_scan_paths[MAX_PATHNAME+1];
    unsigned char tagcache_db_path[MAX_PATHNAME+1];
#endif
    int default_codepage;

    /* ── power / display ───────────────────────────────────────────── */
    int  poweroff;              /* idle power off, minutes */
    int  timeformat;            /* 0=24h 1=12h */
    int  backlight_timeout;
#if CONFIG_CHARGING
    int  backlight_timeout_plugged;
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    int  brightness;
#endif
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
    int backlight_fade_in;
    int backlight_fade_out;
#elif defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
    bool backlight_fade_in;
    bool backlight_fade_out;
#endif
#ifdef HAVE_BUTTON_LIGHT
    int buttonlight_timeout;
#endif
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    int buttonlight_brightness;
#endif
#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
    int touchpad_sensitivity;
#endif
#ifdef HAVE_SPDIF_POWER
    bool spdif_enable;
#endif
#ifdef HAVE_USB_CHARGING_ENABLE
    int usb_charging;
#endif
#ifdef USB_ENABLE_AUDIO
    int usb_audio;
#endif
#if defined(HAVE_USB_POWER) && !defined(USB_NONE) && !defined(SIMULATOR)
    int usb_mode;
#endif
#if CONFIG_TUNER
    int fm_region;
#endif
#if defined(HAVE_RDS_CAP) && defined(CONFIG_RTC)
    bool sync_rds_time;
#endif
#if defined(HAVE_GENERAL_PURPOSE_LED)
    bool use_led_indicators;
#endif
    int sleeptimer_duration;    /* minutes; 0=off */
    bool show_shutdown_message;
    bool disable_mainmenu_scrolling;

    /* ── Osmium UI ─────────────────────────────────────────────────── */
    int  osm_language;          /* 0=English 1=Italiano */
    int  osm_np_view;           /* Now Playing: 0=cover 1=VU meters */
    int  osm_vu_skin;           /* VU meter look */
    int  osm_album_view;        /* albums as 0=grid 1=list */
    bool osm_autoexpand;        /* open Now Playing when a track starts */
    bool osm_wheel_volume;      /* scroll wheel on Now Playing changes volume */
};

/* global settings */
extern struct user_settings global_settings;
/* global status */
extern struct system_status global_status;

#endif /* __SETTINGS_H__ */
