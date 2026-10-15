/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
 *
 * Osmium Sound: core helpers only (volume, replaygain, text and time
 * formatting, file reading, system events). See misc.c.
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
#ifndef MISC_H
#define MISC_H

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include "config.h"
#include "settings.h"

extern const unsigned char * const byte_units[];
extern const unsigned char * const * const kibyte_units;
extern const unsigned char * const unit_strings_core[];

/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. */
char *output_dyn_value(char *buf,
                       int buf_size,
                       int64_t value,
                       const unsigned char * const *units,
                       unsigned int unit_count,
                       bool binary_scale);

/* format_time_auto */
enum e_fmt_time_auto_idx
{
    UNIT_IDX_HR = 0,
    UNIT_IDX_MIN,
    UNIT_IDX_SEC,
    UNIT_IDX_MS,
    UNIT_IDX_TIME_COUNT,
};
#define UNIT_IDX_MASK       0x01FFU /*Return only Unit_IDX*/
#define UNIT_TRIM_ZERO      0x0200U /*Don't show leading zero on max_idx*/
#define UNIT_LOCK_HR        0x0400U /*Don't Auto Range below this field*/
#define UNIT_LOCK_MIN       0x0800U /*Don't Auto Range below this field*/
#define UNIT_LOCK_SEC       0x1000U /*Don't Auto Range below this field*/

unsigned int time_split_units(int unit_idx, unsigned long abs_val,
                        unsigned long (*units_in)[UNIT_IDX_TIME_COUNT]);
const char *format_time_auto(char *buffer, int buf_len, long value,
                                  int unit_idx, bool supress_unit);
void format_time(char* buf, int buf_size, long t);

int read_line(int fd, char* buffer, int buffer_size);
int fast_readline(int fd, char *buf, int buf_size, void *parameters,
                  int (*callback)(int n, char *buf, void *parameters));
bool settings_parseline(char* line, char** name, char** value);

long default_event_handler_ex(long event, void (*callback)(void *), void *parameter);
long default_event_handler(long event);
void car_adapter_mode_init(void) INIT_ATTR;

/* pause / resume playback the way the user expects (pause rewind) */
void pause_action(bool updatewps);
void unpause_action(bool updatewps);

#define BOM_UTF_8 "\xef\xbb\xbf"
#define BOM_UTF_8_SIZE 3
#define BOM_UTF_16_LE "\xff\xfe"
#define BOM_UTF_16_BE "\xfe\xff"
#define BOM_UTF_16_SIZE 2

int split_string(char *str, const char needle, char *vector[], int vector_length);
#ifndef O_PATH
#define O_PATH 0x2000
#endif
void fix_path_part(char* path, int offset, int count);
int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...);
int open_utf8(const char* pathname, int flags);
int string_option(const char *option, const char *const oplist[], bool ignore_case);

#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF) \
        || defined(HAVE_HOTSWAP_STORAGE_AS_MAIN)
void check_bootfile(bool do_rolo);
#endif
#endif

/* volume */
enum volume_adjust_mode
{
    VOLUME_ADJUST_DIRECT,       /* adjust in units of the volume step size */
    VOLUME_ADJUST_PERCEPTUAL,   /* adjust using perceptual steps */
};
#define MIN_NORM_VOLUME_STEPS 10
#define MAX_NORM_VOLUME_STEPS 100
void setvol(void);
void set_normalized_volume(int vol);
int get_normalized_volume(void);
void adjust_volume(int steps);
void adjust_volume_ex(int steps, enum volume_adjust_mode mode);
long to_normalized_volume(long vol, long min_vol, long max_vol, long max_norm);
long from_normalized_volume(long norm, long min_vol, long max_vol, long max_norm);

bool check_rockboxdir(void);

char* strrsplt(char* str, int c);
char* skip_whitespace(char* const str);
char *strip_extension(char* buffer, int buffer_size, const char *filename);

/* sounds */
void beep_play(unsigned int frequency, unsigned int duration,
               unsigned int amplitude);
enum system_sound
{
    SOUND_KEYCLICK = 0,
    SOUND_TRACK_SKIP,
    SOUND_TRACK_NO_MORE,
    SOUND_LIST_EDGE_BEEP_WRAP,
    SOUND_LIST_EDGE_BEEP_NOWRAP,
};
void system_sound_play(enum system_sound sound);

/* replaygain */
struct mp3entry;
int id3_get_replaygain_mode(const struct mp3entry *id3);
void replaygain_update(void);

void format_sound_value(char *buf, size_t buf_sz, int snd, int val);
void format_sound_value_ex(char *buf, size_t buf_sz, int snd, int val, bool skin_token);

#endif /* MISC_H */
