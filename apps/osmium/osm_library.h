/***************************************************************************
 *   Osmium Sound for FiiO M3K — the music library
 *
 *   Lists for the library screens, read from Rockbox's tag database
 *   (tagcache): artists, albums (one per album and album artist, like
 *   Lyrion), genres, years, composers, tracks, plus folders, playlists and
 *   the favourites. One list is held at a time; going back reloads it.
 ****************************************************************************/
#ifndef OSM_LIBRARY_H
#define OSM_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

enum lib_view {
    LIB_ARTISTS,        /* -> LIB_ARTIST_ALBUMS */
    LIB_ALBUMS,         /* -> LIB_ALBUM (page) */
    LIB_GENRES,         /* -> LIB_GENRE_ALBUMS */
    LIB_YEARS,          /* -> LIB_YEAR_ALBUMS */
    LIB_COMPOSERS,      /* -> LIB_COMPOSER_ALBUMS */
    LIB_NEW_ALBUMS,     /* albums, newest first */
    LIB_ARTIST_ALBUMS,
    LIB_GENRE_ALBUMS,
    LIB_YEAR_ALBUMS,
    LIB_COMPOSER_ALBUMS,
    LIB_ALBUM,          /* the tracks of one album */
    LIB_ARTIST_TRACKS,  /* every track of an artist */
    LIB_GENRE_TRACKS,
    LIB_YEAR_TRACKS,
    LIB_COMPOSER_TRACKS,
    LIB_ALL_TRACKS,
    LIB_MOST_PLAYED,
    LIB_RECENTLY_PLAYED,
    LIB_RECENTLY_ADDED,
    LIB_NEVER_PLAYED,
    LIB_FOLDER,         /* a folder of the card */
    LIB_PLAYLISTS,      /* playlist files */
    LIB_PLAYLIST,       /* the tracks of a playlist file */
    LIB_FAVORITES,
};

enum lib_item_kind {
    IT_ARTIST, IT_ALBUM, IT_GENRE, IT_YEAR, IT_COMPOSER, IT_TRACK,
    IT_DIR, IT_FILE, IT_PLAYLIST, IT_ALL_TRACKS,
};

struct lib_item {
    const char *name;
    const char *sub;        /* artist of an album, artist of a track... */
    const char *path;       /* files, folders, playlists (else NULL) */
    int32_t idx;            /* a track in the database (-1: none) */
    int32_t num;            /* year, number of tracks, track length (ms) */
    int32_t num2;           /* track number, play count... */
    uint8_t kind;
};

struct lib_query {
    int view;
    char key[160];          /* the artist, album, genre, composer, path */
    char key2[160];         /* the album artist of an album */
    int  num;               /* the year */
};

extern struct lib_item *lib_items;
extern int lib_count;

bool lib_ready(void);       /* the database can be read */
/* fills lib_items; returns the number of items (or -1) */
int  lib_load(const struct lib_query *q);
/* the file of a track (database or path) */
bool lib_item_path(const struct lib_item *it, char *buf, int len);
/* add the tracks of the list to a playlist being built (player_play_fill);
 * data: NULL for all, or a struct lib_range */
struct lib_range { int from, to; };
struct playlist_insert_context;
int  lib_fill(struct playlist_insert_context *ctx, void *data);
/* the position of item i among the tracks of the list (for "play from") */
int  lib_track_position(int i);

/* the database entry of a file (-1 if not in it) */
int32_t lib_index_of_path(const char *path);

/* a few totals for Settings → Library */
int  lib_track_total(void);

/* the album of an item (artist + album), for "Go to album" */
bool lib_album_of_track(const char *path, char *album, int alen,
                        char *artist, int arlen);

/* ── covers for the album grid ─────────────────────────────────────── */
/* the cell of the album grid (scr_browse.c), so the grid draws them 1:1 */
#define THUMB_SIZE 107
/* the decoded cover of the album whose track is `idx`, or NULL: then it
 * is decoded in the background */
const uint16_t *lib_thumb(int32_t idx, int *w, int *h);
/* true once after new covers arrived (redraw) */
bool lib_thumbs_work(void);
/* covers are being decoded or waiting to be */
bool lib_thumbs_busy(void);
void lib_thumbs_forget(void);

/* the dialogue of letters: the initial of an item for the jump overlay */
void lib_initial(const char *name, char *out);

#endif
