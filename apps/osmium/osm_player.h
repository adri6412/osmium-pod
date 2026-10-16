/***************************************************************************
 *   Osmium Sound for FiiO M3K — the player
 *
 *   What Osmium's Player object is to its QML screens: the state of the
 *   playback engine copied in one place (title, times, format, cover), and
 *   the commands the keys and menus send to it.
 ****************************************************************************/
#ifndef OSM_PLAYER_H
#define OSM_PLAYER_H

#include <stdbool.h>
#include "config.h"
#include "lcd.h"
#include "file.h"

#define NP_COVER_SIZE   156     /* the cover on Now Playing */
#define NP_BACKDROP_SIZE 24     /* tiny cover, stretched and dim, behind it */
#define MINI_COVER_SIZE 34

struct osm_track {
    bool valid;
    char title[160];
    char artist[120];
    char album[120];
    char albumartist[120];
    char path[MAX_PATH];
    char chip[48];              /* "FLAC · 44.1 kHz · 905 kbps" */
    unsigned long length;       /* ms */
    unsigned long elapsed;      /* ms */
    unsigned long frequency;
    int  codec;
    int  index;                 /* in the playlist */
    int  count;                 /* tracks in the playlist */
    long track_gain, album_gain;
};

struct osm_player {
    bool playing;               /* playing, not paused */
    bool paused;
    bool stopped;               /* nothing loaded */
    struct osm_track cur;
    unsigned serial;            /* bumps on every track change */
    /* the cover of the current track, decoded by the engine */
    bool have_cover;
    int  cover_w, cover_h;
    fb_data cover[NP_COVER_SIZE * NP_COVER_SIZE];
    bool have_backdrop;
    int  backdrop_w, backdrop_h;
    fb_data backdrop[NP_BACKDROP_SIZE * NP_BACKDROP_SIZE];
    fb_data mini[MINI_COVER_SIZE * MINI_COVER_SIZE];
};

extern struct osm_player player;

void player_init(void);
/* poll the engine; true if something visible changed */
bool player_poll(void);

/* commands */
void player_toggle_play(void);
void player_next(void);
void player_prev(void);
void player_seek_by(int ms);
void player_seek_fraction(int permille);
void player_cycle_repeat(void);
void player_toggle_shuffle(void);
void player_volume_step(int steps);
int  player_volume_percent(void);
bool player_volume_is_max(void);

/* the status plate under the cover */
bool player_led_hires(void);
bool player_led_bitperfect(void);
bool player_led_replaygain(void);

/* the needles: 0..1000 on the VU scale, with ballistics */
void player_vu_levels(int *left, int *right);

/* favourites: one playlist file, ROCKBOX_DIR/favorites.m3u8 */
bool player_is_favorite(const char *path);
bool player_toggle_favorite(const char *path);   /* returns the new state */
int  player_favorites_count(void);
const char *player_favorites_file(void);

/* fill a new playlist through a callback (it adds with
 * playlist_insert_context_add and returns how many it added), then play
 * from `start` (-1: random when shuffling) */
struct playlist_insert_context;
typedef int (*player_fill_fn)(struct playlist_insert_context *ctx, void *data);
bool player_play_fill(player_fill_fn fill, void *data, int start, bool shuffle);
/* the same, into the current queue: after this track, or at the end */
bool player_queue_fill(player_fill_fn fill, void *data, bool next);

/* start playing a list of files at `start`; shuffle starts at random */
bool player_play_files(const char *const *paths, int count, int start, bool shuffle);
/* queue: after the current track, or at the end */
bool player_queue_files(const char *const *paths, int count, bool next);
/* the whole folder of `path`, from that file */
bool player_play_folder_from(const char *path);
/* a playlist file */
bool player_play_playlist(const char *file, bool shuffle);

/* the time left on the sleep timer, minutes (0: off) */
int  player_sleep_minutes(void);
void player_set_sleep(int minutes);

#endif
