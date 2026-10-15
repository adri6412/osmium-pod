/***************************************************************************
 *   Osmium Sound for FiiO M3K — the player
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "buffering.h"
#include "metadata.h"
#include "settings.h"
#include "misc.h"
#include "sound.h"
#include "powermgmt.h"
#include "pcm_mixer.h"
#include "file.h"
#include "pathfuncs.h"
#include "rbpaths.h"
#include "crc32.h"
#include "string-extra.h"
#include "fixedpoint.h"
#include "dsp_core.h"
#include "osm_player.h"
#include "osm_settings.h"

struct osm_player player;

static int aa_slot_cover = -1, aa_slot_backdrop = -1;
static int cover_hid = -1, backdrop_hid = -1;
static long cover_since;              /* tick of the track change */

#define FAVORITES_FILE ROCKBOX_DIR "/favorites.m3u8"

/* ── album art ─────────────────────────────────────────────────────── */

void player_init(void)
{
    memset(&player, 0, sizeof(player));
    player.stopped = true;
#ifdef HAVE_ALBUMART
    struct dim d = { NP_COVER_SIZE, NP_COVER_SIZE };
    aa_slot_cover = playback_claim_aa_slot(&d);
    struct dim b = { NP_BACKDROP_SIZE, NP_BACKDROP_SIZE };
    aa_slot_backdrop = playback_claim_aa_slot(&b);
#endif
}

/* copy the engine's decoded picture into one of ours */
static bool take_bitmap(int hid, fb_data *dst, int max, int *w, int *h)
{
    struct bitmap *bmp = NULL;
    if (hid < 0 || bufgetdata(hid, 0, (void **)&bmp) <= 0 || !bmp || !bmp->data)
        return false;
    if (bmp->width <= 0 || bmp->height <= 0 || bmp->width > max || bmp->height > max)
        return false;
    memcpy(dst, bmp->data, bmp->width * bmp->height * sizeof(fb_data));
    *w = bmp->width;
    *h = bmp->height;
    return true;
}

/* box filter down to the mini player's thumbnail */
static void make_mini(void)
{
    const int n = MINI_COVER_SIZE;
    int sw = player.cover_w, sh = player.cover_h;
    for (int y = 0; y < n; y++)
    {
        int y0 = y * sh / n, y1 = MAX(y0 + 1, (y + 1) * sh / n);
        for (int x = 0; x < n; x++)
        {
            int x0 = x * sw / n, x1 = MAX(x0 + 1, (x + 1) * sw / n);
            unsigned r = 0, g = 0, b = 0, k = 0;
            for (int j = y0; j < y1; j++)
                for (int i = x0; i < x1; i++)
                {
                    fb_data p = player.cover[j * sw + i];
                    r += (p >> 11) & 31; g += (p >> 5) & 63; b += p & 31;
                    k++;
                }
            player.mini[y * n + x] = ((r / k) << 11) | ((g / k) << 5) | (b / k);
        }
    }
}

static bool poll_covers(void)
{
    bool changed = false;
#ifdef HAVE_ALBUMART
    int hid = aa_slot_cover >= 0 ? playback_current_aa_hid(aa_slot_cover) : -1;
    if (hid != cover_hid)
    {
        cover_hid = hid;
        player.have_cover = take_bitmap(hid, player.cover, NP_COVER_SIZE,
                                        &player.cover_w, &player.cover_h);
        if (player.have_cover)
            make_mini();
        changed = true;
    }
    hid = aa_slot_backdrop >= 0 ? playback_current_aa_hid(aa_slot_backdrop) : -1;
    if (hid != backdrop_hid)
    {
        backdrop_hid = hid;
        player.have_backdrop = take_bitmap(hid, player.backdrop, NP_BACKDROP_SIZE,
                                           &player.backdrop_w, &player.backdrop_h);
        changed = true;
    }
#endif
    return changed;
}

/* ── state ─────────────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t len, const char *src)
{
    strmemccpy(dst, src ? src : "", len);
}

static void make_chip(struct osm_track *t, const struct mp3entry *id3)
{
    const char *label = (id3->codectype < AFMT_NUM_CODECS) ?
                        audio_formats[id3->codectype].label : "";
    char rate[24] = "";
    unsigned long f = id3->frequency;
    if (f)
    {
        if (f % 1000)
            snprintf(rate, sizeof rate, "%lu.%lu kHz", f / 1000, (f % 1000) / 100);
        else
            snprintf(rate, sizeof rate, "%lu kHz", f / 1000);
    }
    if (id3->bitrate)
        snprintf(t->chip, sizeof t->chip, "%s \xC2\xB7 %s \xC2\xB7 %uk",
                 label, rate, id3->bitrate);
    else
        snprintf(t->chip, sizeof t->chip, "%s \xC2\xB7 %s", label, rate);
}

bool player_poll(void)
{
    bool changed = false;
    int st = audio_status();
    bool playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    bool paused = (st & AUDIO_STATUS_PAUSE) != 0;
    bool stopped = !(st & AUDIO_STATUS_PLAY);
    if (playing != player.playing || paused != player.paused || stopped != player.stopped)
    {
        player.playing = playing;
        player.paused = paused;
        player.stopped = stopped;
        changed = true;
    }

    struct mp3entry *id3 = stopped ? NULL : audio_current_track();
    struct osm_track *t = &player.cur;
    if (!id3 || !id3->path[0])
    {
        if (t->valid)
        {
            t->valid = false;
            player.serial++;
            changed = true;
        }
        return changed;
    }

    if (!t->valid || strcmp(t->path, id3->path) ||
        (id3->title && strcmp(t->title, id3->title)))
    {
        /* a new track (or a radio title) */
        t->valid = true;
        copy_str(t->path, sizeof t->path, id3->path);
        if (id3->title && id3->title[0])
            copy_str(t->title, sizeof t->title, id3->title);
        else
        {
            /* no tags: the file name without extension */
            const char *base = strrchr(id3->path, '/');
            strip_extension(t->title, sizeof t->title, base ? base + 1 : id3->path);
        }
        copy_str(t->artist, sizeof t->artist, id3->artist ? id3->artist : id3->albumartist);
        copy_str(t->album, sizeof t->album, id3->album);
        copy_str(t->albumartist, sizeof t->albumartist,
                 id3->albumartist ? id3->albumartist : id3->artist);
        t->frequency = id3->frequency;
        t->codec = id3->codectype;
        t->track_gain = id3->track_gain;
        t->album_gain = id3->album_gain;
        make_chip(t, id3);
        player.serial++;
        cover_since = current_tick;
        changed = true;
    }
    if (t->length != id3->length || t->elapsed / 1000 != id3->elapsed / 1000)
        changed = true;
    t->length = id3->length;
    t->elapsed = id3->elapsed;
    t->index = playlist_get_display_index();
    t->count = playlist_amount();

    if (poll_covers())
        changed = true;
    (void)cover_since;
    return changed;
}

/* ── commands ──────────────────────────────────────────────────────── */

void player_toggle_play(void)
{
    int st = audio_status();
    if (st & AUDIO_STATUS_PLAY)
    {
        if (st & AUDIO_STATUS_PAUSE)
            unpause_action(true);
        else
            pause_action(true);
        return;
    }
    /* stopped: pick up where we left off */
    if (playlist_resume() != -1)
    {
        unsigned long elapsed = global_status.resume_elapsed;
        unsigned long rewind = global_settings.resume_rewind * 1000;
        elapsed = elapsed > rewind ? elapsed - rewind : 0;
        playlist_start(global_status.resume_index, elapsed,
                       global_status.resume_offset);
    }
}

void player_next(void)
{
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_next();
}

void player_prev(void)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return;
    /* past the first seconds, back to the start of this one */
    if (player.cur.elapsed > 3000)
    {
        audio_pre_ff_rewind();
        audio_ff_rewind(0);
    }
    else
        audio_prev();
}

void player_seek_by(int ms)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY) || !player.cur.length)
        return;
    long pos = (long)player.cur.elapsed + ms;
    if (pos < 0) pos = 0;
    if (pos > (long)player.cur.length - 1000) pos = player.cur.length - 1000;
    audio_pre_ff_rewind();
    audio_ff_rewind(pos);
    player.cur.elapsed = pos;
}

void player_seek_fraction(int permille)
{
    if (!player.cur.length)
        return;
    long pos = (long)((long long)player.cur.length * permille / 1000);
    player_seek_by(pos - (long)player.cur.elapsed);
}

void player_cycle_repeat(void)
{
    int r = global_settings.repeat_mode;
    r = (r == REPEAT_OFF) ? REPEAT_ALL : (r == REPEAT_ALL) ? REPEAT_ONE : REPEAT_OFF;
    global_settings.repeat_mode = r;
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_flush_and_reload_tracks();
    settings_save();
}

void player_toggle_shuffle(void)
{
    global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        replaygain_update();
        if (global_settings.playlist_shuffle)
            playlist_randomise(NULL, current_tick, true);
        else
            playlist_sort(NULL, true);
    }
    settings_save();
}

void player_volume_step(int steps)
{
    adjust_volume(steps);
}

int player_volume_percent(void)
{
    int min = sound_min(SOUND_VOLUME), max = MIN(sound_max(SOUND_VOLUME),
                                                 global_settings.volume_limit);
    if (max <= min)
        return 100;
    return to_normalized_volume(global_status.volume, min, max, 100);
}

bool player_volume_is_max(void)
{
    return global_status.volume >= MIN(sound_max(SOUND_VOLUME),
                                       global_settings.volume_limit);
}

/* ── the status plate ──────────────────────────────────────────────── */

bool player_led_hires(void)
{
    return player.cur.valid && player.cur.frequency > 48000;
}

bool player_led_replaygain(void)
{
    if (!player.cur.valid || global_settings.replaygain_settings.type == REPLAYGAIN_OFF)
        return false;
    return player.cur.track_gain != 0 || player.cur.album_gain != 0;
}

bool player_led_bitperfect(void)
{
    /* nothing in the DSP chain touches the samples */
    if (!player.cur.valid || player_led_replaygain())
        return false;
    const struct user_settings *s = &global_settings;
    return !s->eq_enabled && !s->crossfeed && !s->dithering_enabled &&
           !s->surround_enabled && !s->pbe && !s->afr_enabled &&
           s->bass == 0 && s->treble == 0 && s->balance == 0 &&
           s->channel_config == SOUND_CHAN_STEREO &&
           s->compressor_settings.threshold == 0;
}

/* ── VU needles ────────────────────────────────────────────────────── */

void player_vu_levels(int *left, int *right)
{
    static int pos[2], vel[2];
    static long last;
    int target[2] = { 0, 0 };
    if (player.playing)
    {
        static struct pcm_peaks peaks;
        mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);
        /* full deflection (+3 VU) at -3 dBFS peaks; the needle follows the
         * amplitude, like a real meter */
        target[0] = MIN(1100, (int)(peaks.left * 1000 / 23197));
        target[1] = MIN(1100, (int)(peaks.right * 1000 / 23197));
    }
    long now = current_tick;
    int steps = MIN(10, MAX(1, (int)(now - last)));
    if (now - last > HZ)
        steps = 1;
    last = now;
    /* a damped spring per tick: the 300 ms rise of a VU meter */
    for (int s = 0; s < steps; s++)
        for (int c = 0; c < 2; c++)
        {
            int f = (target[c] - pos[c]) * 18 / 100 - vel[c] * 45 / 100;
            vel[c] += f;
            pos[c] += vel[c] / 2;
            if (pos[c] < 0) { pos[c] = 0; vel[c] = 0; }
            if (pos[c] > 1080) { pos[c] = 1080; vel[c] = -vel[c] / 3; }
        }
    *left = MIN(1000, pos[0]);
    *right = MIN(1000, pos[1]);
}

/* ── favourites ────────────────────────────────────────────────────── */

#define FAV_MAX 4000
static uint32_t fav_crc[FAV_MAX];
static int fav_count = -1;            /* -1: not loaded */

static uint32_t path_crc(const char *path)
{
    return crc_32(path, strlen(path), 0xffffffff);
}

static void fav_load(void)
{
    fav_count = 0;
    int fd = open_utf8(FAVORITES_FILE, O_RDONLY);
    if (fd < 0)
        return;
    char line[MAX_PATH];
    while (read_line(fd, line, sizeof line) > 0 && fav_count < FAV_MAX)
    {
        if (line[0] && line[0] != '#')
            fav_crc[fav_count++] = path_crc(line);
    }
    close(fd);
}

const char *player_favorites_file(void)
{
    return FAVORITES_FILE;
}

int player_favorites_count(void)
{
    if (fav_count < 0)
        fav_load();
    return fav_count;
}

bool player_is_favorite(const char *path)
{
    if (!path || !path[0])
        return false;
    if (fav_count < 0)
        fav_load();
    uint32_t c = path_crc(path);
    for (int i = 0; i < fav_count; i++)
        if (fav_crc[i] == c)
            return true;
    return false;
}

bool player_toggle_favorite(const char *path)
{
    if (!path || !path[0])
        return false;
    bool was = player_is_favorite(path);
    if (!was)
    {
        int fd = open(FAVORITES_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0)
        {
            fdprintf(fd, "%s\n", path);
            close(fd);
        }
    }
    else
    {
        /* copy every other line to a new file */
        int in = open_utf8(FAVORITES_FILE, O_RDONLY);
        int out = open(FAVORITES_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (in >= 0 && out >= 0)
        {
            char line[MAX_PATH];
            while (read_line(in, line, sizeof line) > 0)
                if (line[0] && strcmp(line, path))
                    fdprintf(out, "%s\n", line);
        }
        if (in >= 0) close(in);
        if (out >= 0)
        {
            close(out);
            remove(FAVORITES_FILE);
            rename(FAVORITES_FILE ".tmp", FAVORITES_FILE);
        }
    }
    fav_load();
    return !was;
}

/* ── starting music ────────────────────────────────────────────────── */

static bool insert_files(const char *const *paths, int count, int position, bool queue)
{
    struct playlist_insert_context ctx;
    if (playlist_insert_context_create(NULL, &ctx, position, queue, false) < 0)
        return false;
    for (int i = 0; i < count; i++)
        if (playlist_insert_context_add(&ctx, paths[i]) < 0)
            break;
    playlist_insert_context_release(&ctx);
    return true;
}

bool player_play_fill(player_fill_fn fill, void *data, int start, bool shuffle)
{
    struct playlist_insert_context ctx;
    if (playlist_create(NULL, NULL) < 0)
        return false;
    if (playlist_insert_context_create(NULL, &ctx, PLAYLIST_INSERT_LAST, false, false) < 0)
        return false;
    int count = fill(&ctx, data);
    playlist_insert_context_release(&ctx);
    if (count <= 0)
        return false;
    if (start >= count)
        start = count - 1;
    if (shuffle != global_settings.playlist_shuffle)
    {
        global_settings.playlist_shuffle = shuffle;
        settings_save();
    }
    replaygain_update();
    if (shuffle)
    {
        if (start < 0)
            start = rand() % count;
        start = playlist_shuffle(current_tick, start);
        if (start < 0)
            start = 0;
    }
    else if (start < 0)
        start = 0;
    playlist_start(start, 0, 0);
    return true;
}

bool player_queue_fill(player_fill_fn fill, void *data, bool next)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return player_play_fill(fill, data, 0, false);
    struct playlist_insert_context ctx;
    if (playlist_insert_context_create(NULL, &ctx, next ? PLAYLIST_INSERT_FIRST
                                                        : PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
        return false;
    int n = fill(&ctx, data);
    playlist_insert_context_release(&ctx);
    if (n > 0)
        audio_flush_and_reload_tracks();
    return n > 0;
}

bool player_play_files(const char *const *paths, int count, int start, bool shuffle)
{
    if (count <= 0)
        return false;
    if (playlist_create(NULL, NULL) < 0)
        return false;
    if (!insert_files(paths, count, PLAYLIST_INSERT_LAST, false))
        return false;
    if (shuffle != global_settings.playlist_shuffle)
    {
        global_settings.playlist_shuffle = shuffle;
        settings_save();
    }
    replaygain_update();
    if (shuffle)
    {
        if (start < 0)
            start = rand() % count;
        start = playlist_shuffle(current_tick, start);
        if (start < 0)
            start = 0;
    }
    else if (start < 0)
        start = 0;
    playlist_start(start, 0, 0);
    return true;
}

bool player_queue_files(const char *const *paths, int count, bool next)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return player_play_files(paths, count, 0, false);
    bool ok = insert_files(paths, count, next ? PLAYLIST_INSERT_FIRST : PLAYLIST_INSERT_LAST, false);
    if (ok)
        audio_flush_and_reload_tracks();
    return ok;
}

bool player_play_folder_from(const char *path)
{
    char dir[MAX_PATH];
    const char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    size_t n = MIN((size_t)(slash - path), sizeof dir - 1);
    memcpy(dir, path, n);
    dir[n] = 0;
    if (!dir[0])
        strcpy(dir, "/");
    if (playlist_create(dir, NULL) < 0)
        return false;
    int pos = 0;
    playlist_insert_directory(NULL, dir, PLAYLIST_INSERT_LAST, false, false);
    /* find the file in the new playlist */
    struct playlist_track_info info;
    int amount = playlist_amount();
    for (int i = 0; i < amount; i++)
    {
        if (playlist_get_track_info(NULL, i, &info) >= 0 && !strcmp(info.filename, path))
        {
            pos = i;
            break;
        }
    }
    if (global_settings.playlist_shuffle)
        pos = playlist_shuffle(current_tick, pos);
    playlist_start(pos, 0, 0);
    return true;
}

bool player_play_playlist(const char *file, bool shuffle)
{
    const char *slash = strrchr(file, '/');
    char dir[MAX_PATH];
    size_t n = slash ? (size_t)(slash - file) : 0;
    n = MIN(n, sizeof dir - 1);
    memcpy(dir, file, n);
    dir[n] = 0;
    if (playlist_create(dir[0] ? dir : "/", slash ? slash + 1 : file) < 0)
        return false;
    if (playlist_amount() <= 0)
        return false;
    global_settings.playlist_shuffle = shuffle;
    int start = 0;
    if (shuffle)
        start = playlist_shuffle(current_tick, rand() % playlist_amount());
    playlist_start(start, 0, 0);
    return true;
}

/* ── sleep timer ───────────────────────────────────────────────────── */

int player_sleep_minutes(void)
{
    if (!get_sleep_timer_active())
        return 0;
    return (get_sleep_timer() + 59) / 60;
}

void player_set_sleep(int minutes)
{
    set_sleeptimer_duration(minutes);
}
