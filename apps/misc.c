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
 * Osmium Sound: the GUI-free part of Rockbox's apps/misc.c. Everything that
 * used to draw something (splashes, yes/no screens, the USB screen) is now
 * delegated to the Osmium UI through osmium.h.
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
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include "string-extra.h"
#include "config.h"
#include "misc.h"
#include "system.h"
#include "language.h"
#include "iap-usb.h"
#ifdef HAVE_DIRCACHE
#include "dircache.h"
#endif
#include "file.h"
#include "pathfuncs.h"
#include "lang.h"
#include "dir.h"
#include "timefuncs.h"
#include "talk.h"
#include "audio.h"
#include "settings.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#include "tagcache.h"
#include "sound.h"
#include "playlist.h"
#include "fixedpoint.h"
#include "debug.h"
#include "playback.h"
#include "voice_thread.h"
#include "dsp_core.h"
#include "rolo.h"
#include "splash.h"
#include "osmium.h"

/* units used with output_dyn_value */
const unsigned char * const byte_units[] =
{
    ID2P(LANG_BYTE),
    ID2P(LANG_KIBIBYTE),
    ID2P(LANG_MEBIBYTE),
    ID2P(LANG_GIBIBYTE)
};

const unsigned char * const * const kibyte_units = &byte_units[1];

/* units used with format_time_auto, option_select.c->option_get_valuestring() */
const unsigned char * const unit_strings_core[] =
{
    [UNIT_INT] = "",    [UNIT_MS]  = "ms",
    [UNIT_SEC] = "s",   [UNIT_MIN] = "min",
    [UNIT_HOUR]= "hr",  [UNIT_KHZ] = "kHz",
    [UNIT_DB]  = "dB",  [UNIT_PERCENT] = "%",
    [UNIT_MAH] = "mAh", [UNIT_PIXEL] = "px",
    [UNIT_PER_SEC] = "per sec",
    [UNIT_HERTZ] = "Hz",
    [UNIT_MB]  = "MB",  [UNIT_KBIT]  = "kb/s",
    [UNIT_PM_TICK] = "units/10ms",
};

/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. If a buffer is
 * given, the result is snprintf()'d into that buffer, otherwise the result is
 * voiced.*/

/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. If a buffer is
 * given, the result is snprintf()'d into that buffer, otherwise the result is
 * voiced.*/
char *output_dyn_value(char *buf,
                       int buf_size,
                       int64_t value,
                       const unsigned char * const *units,
                       unsigned int unit_count,
                       bool binary_scale)
{
    unsigned int scale = binary_scale ? 1024 : 1000;
    unsigned int fraction = 0;
    unsigned int unit_no = 0;
    uint64_t value_abs = (value < 0) ? -value : value;
    char tbuf[5];
    int value2;

    while (value_abs >= scale && unit_no < (unit_count - 1))
    {
        fraction = value_abs % scale;
        value_abs /= scale;
        unit_no++;
    }

    value2 = (value < 0) ? -value_abs : value_abs; /* preserve sign */
    fraction = (fraction * 1000 / scale) / 10;

    if (value_abs >= 100 || fraction >= 100 || !unit_no)
        tbuf[0] = '\0';
    else if (value_abs >= 10)
        snprintf(tbuf, sizeof(tbuf), "%01u", fraction / 10);
    else
        snprintf(tbuf, sizeof(tbuf), "%02u", fraction);

    if (buf)
    {
        if (*tbuf)
            snprintf(buf, buf_size, "%d%s%s%s", value2, str(LANG_POINT),
                     tbuf, P2STR(units[unit_no]));
        else
            snprintf(buf, buf_size, "%d%s", value2, P2STR(units[unit_no]));
    }
    else
    {
        talk_fractional(tbuf, value2, P2ID(units[unit_no]));
    }
    return buf;
}

/* Performance optimized version of the read_line() (see below) function. */
int fast_readline(int fd, char *buf, int buf_size, void *parameters,
                  int (*callback)(int n, char *buf, void *parameters))
{
    char *p, *next;
    int rc, pos = 0;
    int count = 0;

    while ( 1 )
    {
        next = NULL;

        rc = read(fd, &buf[pos], buf_size - pos - 1);
        if (rc >= 0)
            buf[pos+rc] = '\0';

        if ( (p = strchr(buf, '\n')) != NULL)
        {
            *p = '\0';
            next = ++p;
        }

        if ( (p = strchr(buf, '\r')) != NULL)
        {
            *p = '\0';
            if (!next)
                next = ++p;
        }

        rc = callback(count, buf, parameters);
        if (rc < 0)
            return rc;

        count++;
        if (next)
        {
            pos = buf_size - ((intptr_t)next - (intptr_t)buf) - 1;
            memmove(buf, next, pos);
        }
        else
            break ;
    }

    return 0;
}

bool settings_parseline(char* line, char** name, char** value)
{
    char* ptr;

    line = skip_whitespace(line);

    if ( *line == '#' )
        return false;

    ptr = strchr(line, ':');
    if ( !ptr )
        return false;

    *name = line;
    *ptr = '\0'; /* terminate previous */
    ptr++;
    ptr = skip_whitespace(ptr);
    *value = ptr;

    /* strip whitespace from the right side of value */
    ptr += strlen(ptr);
    ptr--;
    while ((ptr > (*value) - 1) && isspace(*ptr))
    {
        *ptr = '\0';
        ptr--;
    }

    return true;
}

#if CONFIG_CHARGING
static bool waiting_to_resume_play = false;
static bool paused_on_unplugged = false;
static long play_resume_tick;

static void car_adapter_mode_processing(bool inserted)
{
    if (global_settings.car_adapter_mode)
    {
        if(inserted)
        {
            /*
             * Just got plugged in, delay & resume if we were playing
             */
            if ((audio_status() & AUDIO_STATUS_PAUSE) && paused_on_unplugged)
            {
                /* delay resume a bit while the engine is cranking */
                play_resume_tick = current_tick + HZ*global_settings.car_adapter_mode_delay;
                waiting_to_resume_play = true;
            }
        }
        else
        {
            /*
             * Just got unplugged, pause if playing
             */
            if ((audio_status() & AUDIO_STATUS_PLAY) &&
                !(audio_status() & AUDIO_STATUS_PAUSE))
            {
                pause_action(true);
                paused_on_unplugged = true;
            }
            else if (!waiting_to_resume_play)
                paused_on_unplugged = false;
            waiting_to_resume_play = false;
        }
    }
}

static void car_adapter_tick(void)
{
    if (waiting_to_resume_play)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) &&
                !(audio_status() & AUDIO_STATUS_PAUSE))
                waiting_to_resume_play = false;
        if (TIME_AFTER(current_tick, play_resume_tick))
        {
            if (audio_status() & AUDIO_STATUS_PAUSE)
            {
                queue_broadcast(SYS_CAR_ADAPTER_RESUME, 0);
            }
            waiting_to_resume_play = false;
        }
    }
}

void car_adapter_mode_init(void)
{
    tick_add_task(car_adapter_tick);
}
#endif /* CONFIG_CHARGING */

#ifdef HAVE_HEADPHONE_DETECTION
static void hp_unplug_change(bool inserted)
{
    static bool headphone_caused_pause = true;

    if (global_settings.unplug_mode)
    {
        int audio_stat = audio_status();
        if (inserted)
        {
            backlight_on();
            if ((audio_stat & AUDIO_STATUS_PLAY) &&
                    headphone_caused_pause &&
                    global_settings.unplug_mode > 1 )
            {
                unpause_action(true);
            }
            headphone_caused_pause = false;
        } else {
            if ((audio_stat & AUDIO_STATUS_PLAY) &&
                    !(audio_stat & AUDIO_STATUS_PAUSE))
            {
                headphone_caused_pause = true;
                pause_action(false);
            }
        }
    }

#ifdef HAVE_SPEAKER
    /* update speaker status */
    audio_enable_speaker(global_settings.speaker_mode);
#endif
}
#endif /* HAVE_HEADPHONE_DETECTION */


/* set volume and save settings */
void setvol(void)
{
    sound_set_volume(global_status.volume);
    iap_on_volume(global_status.volume);
    global_status.last_volume_change = current_tick;
    status_save(false);
}

#ifdef HAVE_PERCEPTUAL_VOLUME
static short norm_tab[MAX_NORM_VOLUME_STEPS+2];
static int norm_tab_num_steps;
static int norm_tab_size;

static void update_norm_tab(void)
{
    const int lim = global_settings.volume_adjust_norm_steps;
    if (lim == norm_tab_num_steps)
        return;
    norm_tab_num_steps = lim;

    const int min = sound_min(SOUND_VOLUME);
    const int max = sound_max(SOUND_VOLUME);
    const int step = sound_steps(SOUND_VOLUME);

    /* Ensure the table contains the minimum volume */
    norm_tab[0] = min;
    norm_tab_size = 1;

    for (int i = 1; i < lim - 1; ++i)
    {
        int vol = from_normalized_volume(i, min, max, lim);
        int rem = vol % step;

        vol -= rem;
        if (abs(rem) > step/2)
            vol += rem < 0 ? -step : step;

        /* Add volume step, ignoring any duplicate entries that may
         * occur due to rounding */
        if (vol != norm_tab[norm_tab_size-1])
            norm_tab[norm_tab_size++] = vol;
    }

    /* Ensure the table contains the maximum volume */
    if (norm_tab[norm_tab_size-1] != max)
        norm_tab[norm_tab_size++] = max;
}

void set_normalized_volume(int vol)
{
    update_norm_tab();

    if (vol < 0)
        vol = 0;
    if (vol >= norm_tab_size)
        vol = norm_tab_size - 1;

    global_status.volume = norm_tab[vol];
}

int get_normalized_volume(void)
{
    update_norm_tab();

    int a = 0, b = norm_tab_size - 1;
    while (a != b)
    {
        int i = (a + b + 1) / 2;
        if (global_status.volume < norm_tab[i])
            b = i - 1;
        else
            a = i;
    }

    return a;
}
#else
void set_normalized_volume(int vol)
{
    global_status.volume = vol * sound_steps(SOUND_VOLUME);
}

int get_normalized_volume(void)
{
    return global_status.volume / sound_steps(SOUND_VOLUME);
}
#endif

void adjust_volume(int steps)
{
#ifdef HAVE_PERCEPTUAL_VOLUME
    adjust_volume_ex(steps, global_settings.volume_adjust_mode);
#else
    adjust_volume_ex(steps, VOLUME_ADJUST_DIRECT);
#endif
}

void adjust_volume_ex(int steps, enum volume_adjust_mode mode)
{
    switch (mode)
    {
    case VOLUME_ADJUST_PERCEPTUAL:
#ifdef HAVE_PERCEPTUAL_VOLUME
        set_normalized_volume(get_normalized_volume() + steps);
        break;
#endif
    case VOLUME_ADJUST_DIRECT:
    default:
        global_status.volume += steps * sound_steps(SOUND_VOLUME);
        break;
    }

    setvol();
}


char* strrsplt(char* str, int c)
{
    char* s = strrchr(str, c);

    if (s != NULL)
    {
        *s++ = '\0';
    }
    else
    {
        s = str;
    }

    return s;
}

/*
 * removes the extension of filename (if it doesn't start with a .)
 * puts the result in buffer
 */
char *strip_extension(char* buffer, int buffer_size, const char *filename)
{
    if (!buffer || !filename || buffer_size <= 0)
    {
        return NULL;
    }

    off_t dotpos = (strrchr(filename, '.') - filename) + 1;

    /* no match on filename beginning with '.' or beyond buffer_size */
    if(dotpos > 1 && dotpos < buffer_size)
        buffer_size = dotpos;
    strmemccpy(buffer, filename, buffer_size);

    return buffer;
}

/* Return the ReplayGain mode adjusted by other relevant settings */
static int replaygain_setting_mode(int type)
{
    switch (type)
    {
    case REPLAYGAIN_SHUFFLE:
        type = global_settings.playlist_shuffle ?
            REPLAYGAIN_TRACK : REPLAYGAIN_ALBUM;
    case REPLAYGAIN_ALBUM:
    case REPLAYGAIN_TRACK:
    case REPLAYGAIN_OFF:
    default:
        break;
    }

    return type;
}

/* Return the ReplayGain mode adjusted for display purposes */
int id3_get_replaygain_mode(const struct mp3entry *id3)
{
    if (!id3)
        return -1;

    int type = global_settings.replaygain_settings.type;
    type = replaygain_setting_mode(type);

    return (type != REPLAYGAIN_TRACK && id3->album_gain != 0) ?
        REPLAYGAIN_ALBUM : (id3->track_gain != 0 ? REPLAYGAIN_TRACK : -1);
}

/* Update DSP's replaygain from global settings */
void replaygain_update(void)
{
    struct replaygain_settings settings = global_settings.replaygain_settings;
    settings.type = replaygain_setting_mode(settings.type);
    dsp_replaygain_set_settings(&settings);
}

void format_sound_value_ex(char *buf, size_t buf_sz, int snd, int val, bool skin_token)
{
    int numdec = sound_numdecimals(snd);
    const char *unit = sound_unit(snd);
    int physval = sound_val2phys(snd, val);

    unsigned int factor = ipow(10, numdec);
    if (factor == 0)
    {
        DEBUGF("DIVISION BY ZERO: format_sound_value s:%d v:%d", snd, val);
        factor = 1;
    }
    unsigned int av = abs(physval);
    unsigned int i = av / factor;
    unsigned int d = av - i*factor;

    snprintf(buf, buf_sz, "%s%u%.*s%.*u%s%s", physval < 0 ? "-" : &" "[skin_token],
             i, numdec, ".", numdec, d, &" "[skin_token], skin_token ? "" : unit);
}

/* format a sound value as "-1.05 dB", or " 1.05 dB" */
void format_sound_value(char *buf, size_t buf_sz, int snd, int val)
{
    format_sound_value_ex(buf, buf_sz, snd, val, false);
}

/* Read (up to) a line of text from fd into buffer and return number of bytes
 * read (which may be larger than the number of bytes stored in buffer). If
 * an error occurs, -1 is returned (and buffer contains whatever could be
 * read). A line is terminated by a LF char. Neither LF nor CR chars are
 * stored in buffer.
 */
int read_line(int fd, char* buffer, int buffer_size)
{
    if (!buffer || buffer_size-- <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    unsigned char rdbuf[32];
    off_t rdbufend = 0;
    int rdbufidx = 0;
    int count = 0;
    int num_read = 0;

    while (count < buffer_size)
    {
        if (rdbufidx >= rdbufend)
        {
            rdbufidx = 0;
            rdbufend = read(fd, rdbuf, sizeof (rdbuf));

            if (rdbufend <= 0)
                break;

            num_read += rdbufend;
        }

        int c = rdbuf[rdbufidx++];

        if (c == '\n')
            break;

        if (c == '\r')
            continue;

        buffer[count++] = c;
    }

    rdbufidx -= rdbufend;

    if (rdbufidx < 0)
    {
        /* "put back" what wasn't read from the buffer */
        num_read += rdbufidx;
        rdbufend = lseek(fd, rdbufidx, SEEK_CUR);
    }

    buffer[count] = '\0';

    return rdbufend >= 0 ? num_read : -1;
}

char* skip_whitespace(char* const str)
{
    char *s = str;

    while (isspace(*s))
        s++;

    return s;
}

unsigned int time_split_units(int unit_idx, unsigned long abs_val,
                              unsigned long (*units_in)[UNIT_IDX_TIME_COUNT])
{
    unsigned int base_idx = UNIT_IDX_HR;
    unsigned long hours;
    unsigned long minutes  = 0;
    unsigned long seconds  = 0;
    unsigned long millisec = 0;

    switch (unit_idx & UNIT_IDX_MASK) /*Mask off upper bits*/
    {
            case UNIT_MS:
                base_idx = UNIT_IDX_MS;
                millisec = abs_val;
                abs_val  = abs_val  /  1000U;
                millisec = millisec - (1000U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_SEC:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_SEC;
                seconds  = abs_val;
                abs_val  = abs_val  / 60U;
                seconds  = seconds - (60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_MIN:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_MIN;
                minutes  = abs_val;
                abs_val  = abs_val / 60U;
                minutes  = minutes -(60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_HOUR:
            default:
                hours    = abs_val;
                break;
    }

    (*units_in)[UNIT_IDX_HR]  = hours;
    (*units_in)[UNIT_IDX_MIN] = minutes;
    (*units_in)[UNIT_IDX_SEC] = seconds;
    (*units_in)[UNIT_IDX_MS]  = millisec;

    return base_idx;
}

const char *format_time_auto(char *buffer, int buf_len, long value,
                                  int unit_idx, bool supress_unit)
{
    const char * const sign        = &"-"[value < 0 ? 0 : 1];
    bool               is_rtl      = lang_is_rtl();
    char               timebuf[25]; /* -2147483648:00:00.00\0 */
    int                len;
#if 0 /* unused */
    int left_offset;
#endif
    unsigned char      base_idx, max_idx;

    unsigned long  units_in[UNIT_IDX_TIME_COUNT];
    unsigned char  fwidth[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 0, /* hr is variable length */
                        [UNIT_IDX_MIN] = 2,
                        [UNIT_IDX_SEC] = 2,
                        [UNIT_IDX_MS]  = 3,
                   }; /* {0,2,2,3}; Field Widths */
    unsigned char  offsets[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 10,/* ?:59:59.999 Std offsets    */
                        [UNIT_IDX_MIN] = 7, /*0?:+1:+4.+7 need calculated */
                        [UNIT_IDX_SEC] = 4,/* 999.59:59:0  RTL offsets    */
                        [UNIT_IDX_MS]  = 0,/* 0  .4 :7 :10 won't change   */
                   }; /* {10,7,4,0}; Offsets */
    static const uint16_t unitlock[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_LOCK_HR,
                        [UNIT_IDX_MIN] = UNIT_LOCK_MIN,
                        [UNIT_IDX_SEC] = UNIT_LOCK_SEC,
                        [UNIT_IDX_MS]  = 0,
                   }; /* unitlock */
    static const uint16_t units[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_HOUR,
                        [UNIT_IDX_MIN] = UNIT_MIN,
                        [UNIT_IDX_SEC] = UNIT_SEC,
                        [UNIT_IDX_MS]  = UNIT_MS,
                   }; /* units */

#if 0 /* unused */
    if (idx_pos != NULL)
    {
        (*idx_pos)[0] = MIN((*idx_pos)[0], UNIT_IDX_TIME_COUNT - 1);
        unit_idx |= unitlock[(*idx_pos)[0]];
    }
#endif

    base_idx = time_split_units(unit_idx, labs(value), &units_in);

    if (units_in[UNIT_IDX_HR] || (unit_idx & unitlock[UNIT_IDX_HR]))
        max_idx = UNIT_IDX_HR;
    else if (units_in[UNIT_IDX_MIN] || (unit_idx & unitlock[UNIT_IDX_MIN]))
        max_idx = UNIT_IDX_MIN;
    else if (units_in[UNIT_IDX_SEC] || (unit_idx & unitlock[UNIT_IDX_SEC]))
        max_idx = UNIT_IDX_SEC;
    else if (units_in[UNIT_IDX_MS])
        max_idx = UNIT_IDX_MS;
    else /* value is 0 */
        max_idx = base_idx;

    if (!is_rtl)
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%02lu:%02lu:%02lu.%03lu",
                       units_in[UNIT_IDX_HR],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MS]);

        fwidth[UNIT_IDX_HR]   = len - offsets[UNIT_IDX_HR];

        /* calculate offsets of the other fields based on length of previous */
        offsets[UNIT_IDX_MS]  = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_MIN];
        offsets[UNIT_IDX_SEC] = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_SEC];
        offsets[UNIT_IDX_MIN] = fwidth[UNIT_IDX_HR] + 1;
        offsets[UNIT_IDX_HR]  = 0;

        timebuf[offsets[base_idx] + fwidth[base_idx]] = '\0';

#if 0 /* unused */
        left_offset  = -(offsets[max_idx]);
        left_offset +=
#endif
        strlcpy(buffer, sign, buf_len);

        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            offsets[max_idx]++;
        }

        strlcat(buffer, &timebuf[offsets[max_idx]], buf_len);

        if (!supress_unit)
        {
            strlcat(buffer, " ", buf_len);
            strlcat(buffer, unit_strings_core[units[max_idx]], buf_len);
        }
    }
    else /*RTL Languages*/
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%03lu.%02lu:%02lu:%02lu",
                       units_in[UNIT_IDX_MS],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_HR]);

        fwidth[UNIT_IDX_HR] = len - offsets[UNIT_IDX_HR];

#if 0 /* unused */
        left_offset = -(offsets[base_idx]);
#endif

        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            timebuf[offsets[max_idx]] = timebuf[offsets[max_idx]+1];
            fwidth[max_idx]--;
        }

        timebuf[offsets[max_idx] + fwidth[max_idx]] = '\0';

        if (!supress_unit)
        {
            strmemccpy(buffer, unit_strings_core[units[max_idx]], buf_len);
#if 0 /* unused */
            left_offset +=
#endif
            strlcat(buffer, " ", buf_len);
            strlcat(buffer, &timebuf[offsets[base_idx]], buf_len);
        }
        else
            strmemccpy(buffer, &timebuf[offsets[base_idx]], buf_len);

        strlcat(buffer, sign, buf_len);
    }
#if 0 /* unused */
    if (idx_pos != NULL)
    {
        (*idx_pos)[1]= fwidth[*(idx_pos)[0]];
        (*idx_pos)[0]= left_offset + offsets[(*idx_pos)[0]];
    }
#endif

    return buffer;
}

/* Format time into buf.
 *
 * buf      - buffer to format to.
 * buf_size - size of buffer.
 * t        - time to format, in milliseconds.
 */
void format_time(char* buf, int buf_size, long t)
{
    unsigned long units_in[UNIT_IDX_TIME_COUNT] = {0};
    time_split_units(UNIT_MS, labs(t), &units_in);
    int hashours = units_in[UNIT_IDX_HR] > 0;
    snprintf(buf, buf_size, "%.*s%.0lu%.*s%.*lu:%.2lu",
             t < 0, "-", units_in[UNIT_IDX_HR], hashours, ":",
             hashours+1, units_in[UNIT_IDX_MIN], units_in[UNIT_IDX_SEC]);
}

/**
 * Splits str at each occurence of split_char and puts the substrings into vector,
 * but at most vector_lenght items. Empty substrings are ignored.
 *
 * Modifies str by replacing each split_char following a substring with nul
 *
 * Returns the number of substrings found, i.e. the number of valid strings
 * in vector
 */
int split_string(char *str, const char split_char, char *vector[], const int vector_length)
{
    int i;
    char sep[2] = {split_char, '\0'};
    char *e, *p = strtok_r(str, sep, &e);

    /* strtok takes care of leading & trailing splitters */
    for(i = 0; i < vector_length; i++)
    {
        vector[i] = p;
        if (!p)
            break;
        p = strtok_r(NULL, sep, &e);
    }

    return i;
}

/* returns match index from option list
 * returns -1 if option was not found
 * option list is array of char pointers with the final item set to null
 * ex - const char * const option[] = { "op_a", "op_b", "op_c", NULL}
 */
int string_option(const char *option, const char *const oplist[], bool ignore_case)
{
    const char *op;
    int (*cmp_fn)(const char*, const char*) = &strcasecmp;
    if (!ignore_case)
        cmp_fn = strcmp;
    for (int i=0; (op=oplist[i]) != NULL; i++)
    {
        if (cmp_fn(op, option) == 0)
            return i;
    }
    return -1;
}

/* Make sure part of path only contain chars valid for a FAT32 long name.
 * Double quotes are replaced with single quotes, other unsupported chars
 * are replaced with an underscore.
 *
 * path   - path to modify.
 * offset - where in path to start checking.
 * count  - number of chars to check.
 */
void fix_path_part(char* path, int offset, int count)
{
    static const char invalid_chars[] = "*/:<>?\\|";
    int i;

    path += offset;

    for (i = 0; i <= count; i++, path++)
    {
        if (*path == 0)
            return;
        if (*path == '"')
            *path = '\'';
        else if (strchr(invalid_chars, *path))
            *path = '_';
    }
}

/* open but with a builtin printf for assembling the path */
int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...)
{
    va_list ap;
    va_start(ap, pathfmt);
    vsnprintf(buf, size, pathfmt, ap);
    va_end(ap);
    if ((oflag & O_PATH) == O_PATH)
        return -1;
    return open(buf, oflag, 0666);
}

int open_utf8(const char* pathname, int flags)
{
    ssize_t ret;
    int fd;
    unsigned char bom[BOM_UTF_8_SIZE];

    fd = open(pathname, flags, 0666);
    if(fd < 0)
        return fd;

    if(flags & (O_TRUNC | O_WRONLY))
    {
        ret = write(fd, BOM_UTF_8, BOM_UTF_8_SIZE);
    }
    else
    {
        ret = read(fd, bom, BOM_UTF_8_SIZE);
        /* check for BOM */
        if (ret == BOM_UTF_8_SIZE)
        {
            if(memcmp(bom, BOM_UTF_8, BOM_UTF_8_SIZE))
                lseek(fd, 0, SEEK_SET);
        }
    }
    /* read or write failure, do not continue */
    if (ret < 0)
        close(fd);

    return ret >= 0 ? fd : -1;
}

/*
 * "The mapping is designed so that the position in the interval is proportional
 * to the volume as a human ear would perceive it (i.e., the position is the
 * cubic root of the linear sample multiplication factor).  For controls with
 * a small range (24 dB or less), the mapping is linear in the dB values so
 * that each step has the same size visually.  Only for controls without dB
 * information, a linear mapping of the hardware volume register values is used
 * (this is the same algorithm as used in the old alsamixer)."
 */

#define NVOL_FRACBITS 16
#define NVOL_UNITY    (1L << NVOL_FRACBITS)

#define nvol_div(x,y) fp_div((x), (y), NVOL_FRACBITS)
#define nvol_mul(x,y) fp_mul((x), (y), NVOL_FRACBITS)
#define nvol_exp10(x) fp_exp10((x), NVOL_FRACBITS)
#define nvol_log10(x) fp_log10((x), NVOL_FRACBITS)

static long get_nvol_factor(void)
{
    long factor = 600L << NVOL_FRACBITS;
    long numdecimals = sound_numdecimals(SOUND_VOLUME);

    if (numdecimals == 0)
        factor /= 10;

    /* nothing *actually* needs this, but: */
    while (numdecimals-- > 1)
        factor *= 10;

    return factor;
}

long to_normalized_volume(long vol, long min_vol, long max_vol, long max_norm)
{
    long norm, min_norm;
    long factor = get_nvol_factor();

    vol <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    norm = nvol_exp10(nvol_div(vol - max_vol, factor));
    norm = nvol_div(norm - min_norm, NVOL_UNITY - min_norm);

    return nvol_mul(norm, max_norm) >> NVOL_FRACBITS;
}

long from_normalized_volume(long norm, long min_vol, long max_vol, long max_norm)
{
    long vol, min_norm;
    long factor = get_nvol_factor();

    norm <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    vol = nvol_div(norm, max_norm);

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    vol = nvol_mul(vol, NVOL_UNITY - min_norm) + min_norm;
    vol = nvol_mul(nvol_log10(vol), factor) + max_vol;

    return vol >> NVOL_FRACBITS;
}

/* ── pausing (was in gui/wps.c) ─────────────────────────────────────── */

void pause_action(bool updatewps)
{
    (void)updatewps;
    /* rewind a bit when pausing, if asked to */
    int rewind = global_settings.pause_rewind;
    audio_pause();
    if (rewind > 0)
    {
        struct mp3entry *id3 = audio_current_track();
        if (id3)
        {
            long pos = (long)id3->elapsed - rewind * 1000L;
            audio_ff_rewind(pos > 0 ? pos : 0);
        }
    }
}

void unpause_action(bool updatewps)
{
    (void)updatewps;
    audio_resume();
}

/* ── system sounds ──────────────────────────────────────────────────── */

void system_sound_play(enum system_sound sound)
{
    static const struct { unsigned short freq, dur, amp; } beeps[] =
    {
        [SOUND_KEYCLICK]              = { 4000, KEYCLICK_DURATION, 2500 },
        [SOUND_TRACK_SKIP]            = { 2000, 100, 2500 },
        [SOUND_TRACK_NO_MORE]         = { 1000, 100, 1500 },
        [SOUND_LIST_EDGE_BEEP_WRAP]   = { 2000,  20, 1500 },
        [SOUND_LIST_EDGE_BEEP_NOWRAP] = { 1000,  40, 1500 },
    };
    if (sound == SOUND_TRACK_SKIP || sound == SOUND_TRACK_NO_MORE)
    {
        if (global_settings.beep)
            beep_play(beeps[sound].freq, beeps[sound].dur,
                      beeps[sound].amp * global_settings.beep);
    }
}

/* ── shutdown, USB, events ──────────────────────────────────────────── */

static void system_flush(void)
{
    playlist_shutdown();
    call_storage_idle_notifys(true);
}

static bool clean_shutdown(enum shutdown_type sd_type,
                           void (*callback)(void *), void *parameter)
{
    status_save(true);

#if CONFIG_CHARGING && !defined(HAVE_POWEROFF_WHILE_CHARGING)
    if (!charger_inserted())
#endif
    {
        bool batt_safe = battery_level_safe();

        if (batt_safe)
        {
#ifdef HAVE_TAGCACHE
            if (!tagcache_prepare_shutdown())
            {
                cancel_shutdown();
                splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
                return false;
            }
#endif
            osm_shutdown_screen(sd_type == SHUTDOWN_REBOOT, battery_level() <= 10);
        }
        else
            osm_shutdown_screen(false, true);

#ifdef HAVE_DISK_STORAGE
        if (batt_safe) /* do not save on critical battery */
#endif
        {
            audio_stop();
            if (callback != NULL)
                callback(parameter);
            system_flush();
        }
#if defined(HAVE_DIRCACHE) && defined(HAVE_DISK_STORAGE)
        else
            dircache_disable();
#endif
        shutdown_hw(sd_type);
    }
    return false;
}

#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF) || defined(HAVE_HOTSWAP_STORAGE_AS_MAIN)
/* memorize/compare details about the BOOTFILE: when a new firmware was
 * copied over USB, offer to start it right away */
void check_bootfile(bool do_rolo)
{
    static time_t mtime = 0;
    DIR* dir = opendir(BOOTDIR);
    struct dirent* entry;

    if (!dir)
        return;

    while ((entry = readdir(dir)))
    {
        if (!strcasecmp(entry->d_name, BOOTFILE))
        {
            struct dirinfo info = dir_get_info(dir, entry);
            if (mtime && do_rolo && info.mtime != mtime)
            {
                button_clear_queue();
                if (osm_confirm_reboot_new_firmware())
                {
                    audio_hard_stop();
                    rolo_load(BOOTDIR "/" BOOTFILE);
                }
            }
            mtime = info.mtime;
            break;
        }
    }
    closedir(dir);
}
#endif
#endif

long default_event_handler_ex(long event, void (*callback)(void *), void *parameter)
{
    switch(event)
    {
        case SYS_USB_CONNECTED:
        {
            intptr_t seqnum = button_get_data();
            if (callback != NULL)
                callback(parameter);
            system_flush();
#if defined(BOOTFILE) && !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
            check_bootfile(false); /* gets initial size */
#endif
            osm_usb_screen(seqnum);
#if defined(BOOTFILE) && !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
            check_bootfile(true);
#endif
            osm_library_changed();
            return SYS_USB_CONNECTED;
        }

        case SYS_POWEROFF:
        case SYS_REBOOT:
            if (!clean_shutdown(event == SYS_POWEROFF ? SHUTDOWN_POWER_OFF
                                                      : SHUTDOWN_REBOOT,
                                callback, parameter))
                return event;
            break;
#if CONFIG_CHARGING
        case SYS_CHARGER_CONNECTED:
            car_adapter_mode_processing(true);
            return SYS_CHARGER_CONNECTED;

        case SYS_CHARGER_DISCONNECTED:
            car_adapter_mode_processing(false);
            reset_runtime();
            return SYS_CHARGER_DISCONNECTED;

        case SYS_CAR_ADAPTER_RESUME:
            unpause_action(true);
            return SYS_CAR_ADAPTER_RESUME;
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        case SYS_FS_CHANGED:
        {
            /* rockbox lives on the first hotswappable drive: give up if
             * that one is gone */
            for (int i = 0; i < NUM_DRIVES; i++)
            {
                if (storage_removable(i) && !storage_present(i))
                    return SYS_FS_CHANGED;
            }
            system_flush();
            check_bootfile(true); /* state gotten in main.c:init() */
            osm_library_changed();
        }
            return SYS_FS_CHANGED;
#endif
#ifdef HAVE_HEADPHONE_DETECTION
        case SYS_PHONE_PLUGGED:
            hp_unplug_change(true);
            return SYS_PHONE_PLUGGED;

        case SYS_PHONE_UNPLUGGED:
            hp_unplug_change(false);
            return SYS_PHONE_UNPLUGGED;
#endif
    }
    return 0;
}

/* .rockbox must exist for anything to work */
bool check_rockboxdir(void)
{
    if (!dir_exists(ROCKBOX_DIR))
    {
        splash(HZ*2, "No .rockbox directory");
        return false;
    }
    return true;
}

long default_event_handler(long event)
{
    return default_event_handler_ex(event, NULL, NULL);
}
