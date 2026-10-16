/***************************************************************************
 *   Osmium Sound for FiiO M3K — the music library
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "tagcache.h"
#include "metadata.h"
#include "misc.h"
#include "crc32.h"
#include "strnatcmp.h"
#include "string-extra.h"
#include "pathfuncs.h"
#include "rbunicode.h"
#include "bmp.h"
#include "jpeg_load.h"
#include "albumart.h"
#include "osm_library.h"
#include "osm_player.h"
#include "osm_strings.h"
#include "playlist.h"
#include "thread.h"

#define LIB_MAX    12000
#define POOL_SIZE  (768 * 1024)

static struct lib_item items[LIB_MAX];
struct lib_item *lib_items = items;
int lib_count;

static char pool[POOL_SIZE];
static int pool_used;
static int32_t sortkey[LIB_MAX];         /* mtime, play count... */

static struct tagcache_search tcs;
static struct tagcache_search_clause clauses[3];
static char tagbuf[MAX_PATH];

#define PLAYLIST_DIR "/Playlists"

/* ── helpers ───────────────────────────────────────────────────────── */

static const char *pool_str(const char *s)
{
    size_t n = strlen(s) + 1;
    if (pool_used + n > POOL_SIZE)
        return "";
    char *p = &pool[pool_used];
    memcpy(p, s, n);
    pool_used += n;
    return p;
}

static void reset(void)
{
    lib_count = 0;
    pool_used = 0;
}

static struct lib_item *add_item(int kind, const char *name, const char *sub)
{
    if (lib_count >= LIB_MAX)
        return NULL;
    struct lib_item *it = &items[lib_count];
    it->kind = kind;
    it->name = pool_str(name);
    it->sub = sub ? pool_str(sub) : NULL;
    it->path = NULL;
    it->idx = -1;
    it->num = it->num2 = 0;
    sortkey[lib_count] = 0;
    lib_count++;
    return it;
}

static bool untagged(const char *s)
{
    return !s || !*s || !strcmp(s, UNTAGGED);
}

bool lib_ready(void)
{
#ifdef HAVE_TAGCACHE
    return tagcache_is_usable();
#else
    return false;
#endif
}

/* a stable sort key that ignores a leading "The " and case */
static const char *sortable(const char *s)
{
    if (!strncasecmp(s, "the ", 4))
        return s + 4;
    return s;
}

static int cmp_name(const void *a, const void *b)
{
    const struct lib_item *x = a, *y = b;
    /* the untagged go last */
    bool ux = x->name[0] == '\x7f', uy = y->name[0] == '\x7f';
    if (ux != uy)
        return ux ? 1 : -1;
    return strnatcasecmp(sortable(x->name), sortable(y->name));
}

/* qsort with a key array alongside: sort indices, then permute */
static int sort_dir;
static int cmp_key_idx(const void *a, const void *b)
{
    int i = *(const short *)a, j = *(const short *)b;
    if (sortkey[i] != sortkey[j])
        return sort_dir * (sortkey[i] < sortkey[j] ? -1 : 1);
    return strnatcasecmp(items[i].name, items[j].name);
}

static short order[LIB_MAX];

static void sort_by_key(int dir, int limit)
{
    for (int i = 0; i < lib_count; i++)
        order[i] = i;
    sort_dir = dir;
    qsort(order, lib_count, sizeof(order[0]), cmp_key_idx);
    /* permute items (and keys) in place through a scratch copy */
    static struct lib_item scratch[LIB_MAX];
    static int32_t kscratch[LIB_MAX];
    for (int i = 0; i < lib_count; i++)
    {
        scratch[i] = items[order[i]];
        kscratch[i] = sortkey[order[i]];
    }
    if (limit > 0 && lib_count > limit)
        lib_count = limit;
    memcpy(items, scratch, lib_count * sizeof(items[0]));
    memcpy(sortkey, kscratch, lib_count * sizeof(sortkey[0]));
}

static int cmp_track(const void *a, const void *b)
{
    const struct lib_item *x = a, *y = b;
    if (x->num2 != y->num2)
        return x->num2 < y->num2 ? -1 : 1;
    return strnatcasecmp(x->name, y->name);
}

static int cmp_track_by_album(const void *a, const void *b)
{
    const struct lib_item *x = a, *y = b;
    int c = strnatcasecmp(x->sub ? x->sub : "", y->sub ? y->sub : "");
    if (c)
        return c;
    return cmp_track(a, b);
}

static int cmp_album_by_year(const void *a, const void *b)
{
    const struct lib_item *x = a, *y = b;
    if (x->num2 != y->num2)
        return x->num2 < y->num2 ? -1 : 1;
    return strnatcasecmp(x->name, y->name);
}

/* ── the database ──────────────────────────────────────────────────── */

static void set_clause(int n, int tag, const char *str, bool numeric, long num)
{
    struct tagcache_search_clause *c = &clauses[n];
    memset(c, 0, sizeof(*c));
    c->tag = tag;
    c->type = clause_is;
    c->numeric = numeric;
    c->source = source_constant;
    c->numeric_data = num;
    c->str = (char *)str;
    tagcache_search_add_clause(&tcs, c);
}

/* a list of the distinct values of a tag (artists, genres, composers) */
static int load_unique(int tag, int kind)
{
    if (!tagcache_search(&tcs, tag))
        return -1;
    while (tagcache_get_next(&tcs, tagbuf, sizeof tagbuf))
    {
        const char *s = tcs.result;
        if (untagged(s))
            continue;
        if (!add_item(kind, s, NULL))
            break;
    }
    tagcache_search_finish(&tcs);
    qsort(items, lib_count, sizeof(items[0]), cmp_name);
    return lib_count;
}

/* years: every distinct year of the tracks, newest first */
static int load_years(void)
{
    static uint16_t seen[3000];
    memset(seen, 0, sizeof seen);
    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    while (tagcache_get_next(&tcs, tagbuf, sizeof tagbuf))
    {
        long y = tagcache_get_numeric(&tcs, tag_year);
        if (y > 0 && y < 3000)
            seen[y]++;
    }
    tagcache_search_finish(&tcs);
    for (int y = 2999; y > 0; y--)
    {
        if (!seen[y])
            continue;
        char buf[8];
        snprintf(buf, sizeof buf, "%d", y);
        struct lib_item *it = add_item(IT_YEAR, buf, NULL);
        if (!it)
            break;
        it->num = y;
        it->num2 = seen[y];
    }
    return lib_count;
}

/* albums, one per (album, album artist), from the tracks that pass the
 * clauses already set on tcs */
#define AHASH 16384
static int16_t ahash[AHASH];

static int load_albums_from_search(void)
{
    char album[160], artist[160];
    memset(ahash, 0xff, sizeof ahash);
    while (tagcache_get_next(&tcs, tagbuf, sizeof tagbuf))
    {
        int idx = tcs.idx_id;
        tagcache_retrieve(&tcs, idx, tag_album, album, sizeof album);
        tagcache_retrieve(&tcs, idx, tag_virt_canonicalartist, artist, sizeof artist);
        long year = tagcache_get_numeric(&tcs, tag_year);
        long mtime = tagcache_get_numeric(&tcs, tag_commitid) * 0x10000L +
                     (tagcache_get_numeric(&tcs, tag_mtime) & 0xFFFF);
        if (untagged(album))
            strcpy(album, "\x7f");          /* sorted last, shown as "Unknown" */
        if (untagged(artist))
            artist[0] = 0;
        uint32_t h = crc_32(album, strlen(album), 0xffffffff);
        h = crc_32(artist, strlen(artist), h);
        unsigned slot = h & (AHASH - 1);
        int found = -1;
        while (ahash[slot] >= 0)
        {
            struct lib_item *it = &items[ahash[slot]];
            if (!strcmp(it->name, album) && !strcmp(it->sub, artist))
            {
                found = ahash[slot];
                break;
            }
            slot = (slot + 1) & (AHASH - 1);
        }
        if (found >= 0)
        {
            items[found].num++;
            if (year > items[found].num2)
                items[found].num2 = year;
            if (mtime > sortkey[found])
                sortkey[found] = mtime;
            continue;
        }
        if (lib_count >= LIB_MAX - 1 || lib_count >= AHASH * 3 / 4)
            continue;
        struct lib_item *it = add_item(IT_ALBUM, album, artist);
        it->idx = idx;
        it->num = 1;
        it->num2 = year;
        sortkey[lib_count - 1] = mtime;
        ahash[slot] = lib_count - 1;
    }
    tagcache_search_finish(&tcs);
    return lib_count;
}

/* tracks that pass the clauses already set on tcs */
static int load_tracks_from_search(bool album_in_sub)
{
    char title[160], other[160];
    while (tagcache_get_next(&tcs, tagbuf, sizeof tagbuf))
    {
        int idx = tcs.idx_id;
        tagcache_retrieve(&tcs, idx, tag_title, title, sizeof title);
        tagcache_retrieve(&tcs, idx, album_in_sub ? tag_album : tag_artist,
                          other, sizeof other);
        if (untagged(title))
        {
            /* no title: the file name */
            const char *base = strrchr(tagbuf, '/');
            strip_extension(title, sizeof title, base ? base + 1 : tagbuf);
        }
        struct lib_item *it = add_item(IT_TRACK, title, untagged(other) ? "" : other);
        if (!it)
            break;
        it->idx = idx;
        it->num = tagcache_get_numeric(&tcs, tag_length);
        long disc = tagcache_get_numeric(&tcs, tag_discnumber);
        long track = tagcache_get_numeric(&tcs, tag_tracknumber);
        it->num2 = MAX(0, disc) * 1000 + MAX(0, track);
        sortkey[lib_count - 1] = 0;
    }
    tagcache_search_finish(&tcs);
    return lib_count;
}

/* tracks ranked by a numeric tag (play count, last played...) */
static int load_ranked(int tag, bool zero_only, int limit)
{
    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    char title[160], artist[160];
    while (tagcache_get_next(&tcs, tagbuf, sizeof tagbuf))
    {
        long v = tagcache_get_numeric(&tcs, tag);
        if (zero_only ? v != 0 : v <= 0)
            continue;
        int idx = tcs.idx_id;
        tagcache_retrieve(&tcs, idx, tag_title, title, sizeof title);
        tagcache_retrieve(&tcs, idx, tag_artist, artist, sizeof artist);
        if (untagged(title))
        {
            const char *base = strrchr(tagbuf, '/');
            strip_extension(title, sizeof title, base ? base + 1 : tagbuf);
        }
        struct lib_item *it = add_item(IT_TRACK, title, untagged(artist) ? "" : artist);
        if (!it)
            break;
        it->idx = idx;
        it->num = tagcache_get_numeric(&tcs, tag_length);
        it->num2 = tagcache_get_numeric(&tcs, tag_playcount);
        sortkey[lib_count - 1] = zero_only ? rand() : v;
    }
    tagcache_search_finish(&tcs);
    sort_by_key(-1, limit);
    return lib_count;
}

/* ── files ─────────────────────────────────────────────────────────── */

static bool is_playlist_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

static int cmp_files(const void *a, const void *b)
{
    const struct lib_item *x = a, *y = b;
    if ((x->kind == IT_DIR) != (y->kind == IT_DIR))
        return x->kind == IT_DIR ? -1 : 1;
    return strnatcasecmp(x->name, y->name);
}

static int load_folder(const char *path, bool playlists_only)
{
    DIR *dir = opendir(path[0] ? path : "/");
    if (!dir)
        return -1;
    struct dirent *ent;
    char full[MAX_PATH];
    while ((ent = readdir(dir)))
    {
        if (ent->d_name[0] == '.')
            continue;
        struct dirinfo info = dir_get_info(dir, ent);
        bool isdir = info.attribute & ATTR_DIRECTORY;
        int kind;
        if (isdir)
        {
            if (playlists_only)
                continue;
            kind = IT_DIR;
        }
        else if (is_playlist_name(ent->d_name))
            kind = IT_PLAYLIST;
        else if (!playlists_only && probe_file_format(ent->d_name) != AFMT_UNKNOWN)
            kind = IT_FILE;
        else
            continue;
        char name[MAX_PATH];
        if (kind == IT_DIR)
            strmemccpy(name, ent->d_name, sizeof name);
        else
            strip_extension(name, sizeof name, ent->d_name);
        struct lib_item *it = add_item(kind, name, NULL);
        if (!it)
            break;
        path_append(full, path[0] ? path : "/", ent->d_name, sizeof full);
        it->path = pool_str(full);
    }
    closedir(dir);
    qsort(items, lib_count, sizeof(items[0]), cmp_files);
    return lib_count;
}

/* the tracks of a playlist file, with their tags when the database has them */
static int load_playlist(const char *file)
{
    int fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
        return lib_count;
    char line[MAX_PATH], full[MAX_PATH], dir[MAX_PATH];
    const char *slash = strrchr(file, '/');
    int dlen = slash ? slash - file : 0;
    memcpy(dir, file, dlen);
    dir[dlen] = 0;
    while (read_line(fd, line, sizeof line) > 0)
    {
        if (!line[0] || line[0] == '#')
            continue;
        if (line[0] == '/')
            strmemccpy(full, line, sizeof full);
        else
            path_append(full, dir[0] ? dir : "/", line, sizeof full);
        char title[160] = "", artist[160] = "";
        int idx = -1;
        long length = 0;
        if (lib_ready() && tagcache_find_index(&tcs, full))
        {
            idx = tcs.idx_id;
            tagcache_retrieve(&tcs, idx, tag_title, title, sizeof title);
            tagcache_retrieve(&tcs, idx, tag_artist, artist, sizeof artist);
            length = tagcache_get_numeric(&tcs, tag_length);
            tagcache_search_finish(&tcs);
        }
        if (untagged(title))
        {
            const char *base = strrchr(full, '/');
            strip_extension(title, sizeof title, base ? base + 1 : full);
        }
        struct lib_item *it = add_item(IT_FILE, title, untagged(artist) ? "" : artist);
        if (!it)
            break;
        it->path = pool_str(full);
        it->idx = idx;
        it->num = length;
    }
    close(fd);
    return lib_count;
}

/* ── loading ───────────────────────────────────────────────────────── */

int lib_load(const struct lib_query *q)
{
    reset();
    switch (q->view)
    {
        case LIB_FOLDER:
            return load_folder(q->key, false);
        case LIB_PLAYLISTS:
            return load_folder(PLAYLIST_DIR, true);
        case LIB_PLAYLIST:
            return load_playlist(q->key);
        case LIB_FAVORITES:
            return load_playlist(player_favorites_file());
        default:
            break;
    }

    if (!lib_ready())
        return -1;

    switch (q->view)
    {
        case LIB_ARTISTS:
            return load_unique(tag_virt_canonicalartist, IT_ARTIST);
        case LIB_GENRES:
            return load_unique(tag_genre, IT_GENRE);
        case LIB_COMPOSERS:
            return load_unique(tag_composer, IT_COMPOSER);
        case LIB_YEARS:
            return load_years();

        case LIB_ALBUMS:
        case LIB_NEW_ALBUMS:
        case LIB_ARTIST_ALBUMS:
        case LIB_GENRE_ALBUMS:
        case LIB_YEAR_ALBUMS:
        case LIB_COMPOSER_ALBUMS:
        {
            if (!tagcache_search(&tcs, tag_filename))
                return -1;
            if (q->view == LIB_ARTIST_ALBUMS)
                set_clause(0, tag_virt_canonicalartist, q->key, false, 0);
            else if (q->view == LIB_GENRE_ALBUMS)
                set_clause(0, tag_genre, q->key, false, 0);
            else if (q->view == LIB_COMPOSER_ALBUMS)
                set_clause(0, tag_composer, q->key, false, 0);
            else if (q->view == LIB_YEAR_ALBUMS)
                set_clause(0, tag_year, NULL, true, q->num);
            load_albums_from_search();
            if (q->view == LIB_NEW_ALBUMS)
                sort_by_key(-1, 60);
            else if (q->view == LIB_ARTIST_ALBUMS)
                qsort(items, lib_count, sizeof(items[0]), cmp_album_by_year);
            else
                qsort(items, lib_count, sizeof(items[0]), cmp_name);
            return lib_count;
        }

        case LIB_ALBUM:
            if (!tagcache_search(&tcs, tag_filename))
                return -1;
            set_clause(0, tag_album, q->key, false, 0);
            if (q->key2[0])
                set_clause(1, tag_virt_canonicalartist, q->key2, false, 0);
            load_tracks_from_search(false);
            qsort(items, lib_count, sizeof(items[0]), cmp_track);
            return lib_count;

        case LIB_ARTIST_TRACKS:
        case LIB_GENRE_TRACKS:
        case LIB_YEAR_TRACKS:
        case LIB_COMPOSER_TRACKS:
        case LIB_ALL_TRACKS:
            if (!tagcache_search(&tcs, tag_filename))
                return -1;
            if (q->view == LIB_ARTIST_TRACKS)
                set_clause(0, tag_virt_canonicalartist, q->key, false, 0);
            else if (q->view == LIB_GENRE_TRACKS)
                set_clause(0, tag_genre, q->key, false, 0);
            else if (q->view == LIB_COMPOSER_TRACKS)
                set_clause(0, tag_composer, q->key, false, 0);
            else if (q->view == LIB_YEAR_TRACKS)
                set_clause(0, tag_year, NULL, true, q->num);
            load_tracks_from_search(true);
            qsort(items, lib_count, sizeof(items[0]), cmp_track_by_album);
            return lib_count;

        case LIB_MOST_PLAYED:
            return load_ranked(tag_playcount, false, 100);
        case LIB_RECENTLY_PLAYED:
            return load_ranked(tag_lastplayed, false, 100);
        case LIB_NEVER_PLAYED:
            return load_ranked(tag_playcount, true, 200);
        case LIB_RECENTLY_ADDED:
        {
            if (!tagcache_search(&tcs, tag_filename))
                return -1;
            load_tracks_from_search(false);
            /* the keys: the order the database met them */
            for (int i = 0; i < lib_count; i++)
                sortkey[i] = items[i].idx;
            sort_by_key(-1, 100);
            return lib_count;
        }
    }
    return -1;
}

bool lib_item_path(const struct lib_item *it, char *buf, int len)
{
    if (it->path)
    {
        strmemccpy(buf, it->path, len);
        return true;
    }
    if (it->idx < 0 || !lib_ready())
        return false;
    bool ok = false;
    if (tagcache_search(&tcs, tag_filename))
    {
        ok = tagcache_retrieve(&tcs, it->idx, tag_filename, buf, len);
        tagcache_search_finish(&tcs);
    }
    return ok;
}

int lib_fill(struct playlist_insert_context *ctx, void *data)
{
    const struct lib_range *r = data;
    int from = r ? MAX(0, r->from) : 0, to = r ? MIN(lib_count, r->to) : lib_count;
    int n = 0;
    char path[MAX_PATH];
    bool open_search = lib_ready() && tagcache_search(&tcs, tag_filename);
    for (int i = from; i < to; i++)
    {
        const struct lib_item *it = &items[i];
        if (it->kind != IT_TRACK && it->kind != IT_FILE)
            continue;
        if (it->path)
            strmemccpy(path, it->path, sizeof path);
        else if (!open_search || it->idx < 0 ||
                 !tagcache_retrieve(&tcs, it->idx, tag_filename, path, sizeof path))
            continue;
        if (playlist_insert_context_add(ctx, path) < 0)
            break;
        n++;
    }
    if (open_search)
        tagcache_search_finish(&tcs);
    return n;
}

int lib_track_position(int i)
{
    int n = 0;
    for (int k = 0; k < i && k < lib_count; k++)
        if (items[k].kind == IT_TRACK || items[k].kind == IT_FILE)
            n++;
    return n;
}

int32_t lib_index_of_path(const char *path)
{
    if (!path || !path[0] || !lib_ready() || !tagcache_find_index(&tcs, path))
        return -1;
    int32_t idx = tcs.idx_id;
    tagcache_search_finish(&tcs);
    return idx;
}

int lib_track_total(void)
{
#ifdef HAVE_TAGCACHE
    struct tagcache_stat *st = tagcache_get_stat();
    return st ? st->total_entries : 0;
#else
    return 0;
#endif
}

bool lib_album_of_track(const char *path, char *album, int alen, char *artist, int arlen)
{
    if (!lib_ready() || !tagcache_find_index(&tcs, path))
        return false;
    tagcache_retrieve(&tcs, tcs.idx_id, tag_album, album, alen);
    tagcache_retrieve(&tcs, tcs.idx_id, tag_virt_canonicalartist, artist, arlen);
    tagcache_search_finish(&tcs);
    if (untagged(artist))
        artist[0] = 0;
    return !untagged(album);
}

void lib_initial(const char *name, char *out)
{
    const char *s = sortable(name);
    ucschar_t c;
    utf8decode((const unsigned char *)s, &c);
    if (c < 0x80 && isalpha(c))
    {
        out[0] = toupper(c);
        out[1] = 0;
    }
    else if (c < 0x80)
        strcpy(out, "#");
    else
    {
        unsigned char *e = utf8encode(c, (unsigned char *)out);
        *e = 0;
    }
}

/* ── covers for the album grid ─────────────────────────────────────── */

/*
 * Covers are decoded by a thread of their own: reading the tags, finding
 * the picture and decoding the JPEG needs more stack than the interface
 * thread has (it lives in the X1000's small IRAM), and it keeps the keys
 * answering while a page of covers comes in.
 *
 * The interface thread owns the cache and the list of requests; it hands
 * the decoder one job at a time and collects it when it is done.
 */
#define THUMB_SLOTS 36
static struct thumb {
    int32_t idx;              /* -1: free */
    bool failed;
    int w, h;
    long used;
    uint16_t px[THUMB_SIZE * THUMB_SIZE];
} thumbs[THUMB_SLOTS];
static bool thumbs_ready;

#define THUMB_QUEUE 8
static int32_t requests[THUMB_QUEUE];
static int requests_len;

enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };
static struct {
    volatile int state;
    int32_t idx;
    bool ok;
    int w, h;
    const uint16_t *px;       /* in decode_buf */
    unsigned generation;
} job;
static unsigned generation;   /* bumps on forget: late results are dropped */

static struct event_queue thumb_queue SHAREDBSS_ATTR;
static long thumb_stack[(16 * 1024) / sizeof(long)];
static unsigned int thumb_thread_id;
#define Q_THUMB_WORK 1

/* the decoder's work area: picture plus the JPEG state */
static unsigned char decode_buf[512 * 1024] CACHEALIGN_ATTR;
static struct mp3entry thumb_id3;
static struct tagcache_search thumb_tcs;

static void thumb_thread(void);

static void thumbs_init(void)
{
    if (thumbs_ready)
        return;
    for (int i = 0; i < THUMB_SLOTS; i++)
        thumbs[i].idx = -1;
    queue_init(&thumb_queue, false);
    thumb_thread_id = create_thread(thumb_thread, thumb_stack, sizeof thumb_stack, 0,
                                    "osm covers" IF_PRIO(, PRIORITY_BACKGROUND)
                                    IF_COP(, CPU));
    thumbs_ready = true;
}

void lib_thumbs_forget(void)
{
    if (!thumbs_ready)
        return;
    for (int i = 0; i < THUMB_SLOTS; i++)
        thumbs[i].idx = -1;
    requests_len = 0;
    generation++;
}

const uint16_t *lib_thumb(int32_t idx, int *w, int *h)
{
    if (idx < 0)
        return NULL;
    thumbs_init();
    for (int i = 0; i < THUMB_SLOTS; i++)
    {
        struct thumb *t = &thumbs[i];
        if (t->idx != idx)
            continue;
        t->used = current_tick;
        if (t->failed)
            return NULL;
        *w = t->w;
        *h = t->h;
        return t->px;
    }
    /* not decoded yet: ask for it, the most recent requests first */
    if (job.state != JOB_IDLE && job.idx == idx)
        return NULL;
    for (int i = 0; i < requests_len; i++)
        if (requests[i] == idx)
            return NULL;
    if (requests_len == THUMB_QUEUE)
    {
        memmove(&requests[0], &requests[1], (THUMB_QUEUE - 1) * sizeof(requests[0]));
        requests_len--;
    }
    requests[requests_len++] = idx;
    return NULL;
}

static bool decode_cover(const char *path, int *w, int *h, const uint16_t **px)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    memset(&thumb_id3, 0, sizeof thumb_id3);
    if (!get_metadata(&thumb_id3, fd, path))
    {
        /* no tags: the pictures next to the file can still be found */
        memset(&thumb_id3, 0, sizeof thumb_id3);
        strmemccpy(thumb_id3.path, path, sizeof thumb_id3.path);
    }
    struct bitmap bm;
    int rc = -1;
    const int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    bm.width = THUMB_SIZE;
    bm.height = THUMB_SIZE;
    bm.data = decode_buf;
#if LCD_DEPTH > 1
    bm.maskdata = NULL;
#endif
    /* the picture in the tags first, as the player does */
    if (thumb_id3.has_embedded_albumart &&
        (thumb_id3.albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
    {
        lseek(fd, thumb_id3.albumart.pos, SEEK_SET);
        rc = clip_jpeg_fd(fd, thumb_id3.albumart.type, thumb_id3.albumart.size,
                          &bm, sizeof decode_buf, format, NULL);
    }
    close(fd);
    /* none, or one the decoder cannot read (PNG, progressive JPEG): the
     * picture files in the folder */
    char file[MAX_PATH];
    if (rc <= 0 && search_albumart_files(&thumb_id3, "", file, sizeof file))
    {
        bm.width = THUMB_SIZE;
        bm.height = THUMB_SIZE;
        bm.data = decode_buf;
        fd = open(file, O_RDONLY);
        if (fd >= 0)
        {
            if (strcasecmp(file + strlen(file) - 4, ".bmp"))
                rc = read_jpeg_fd(fd, &bm, sizeof decode_buf, format, NULL);
            else
                rc = read_bmp_fd(fd, &bm, sizeof decode_buf, format, NULL);
            close(fd);
        }
    }
    if (rc <= 0 || bm.width <= 0 || bm.height <= 0 ||
        bm.width > THUMB_SIZE || bm.height > THUMB_SIZE)
        return false;
    *w = bm.width;
    *h = bm.height;
    *px = (const uint16_t *)bm.data;
    return true;
}

static void thumb_thread(void)
{
    struct queue_event ev;
    char path[MAX_PATH];
    while (1)
    {
        queue_wait(&thumb_queue, &ev);
        if (ev.id != Q_THUMB_WORK || job.state != JOB_RUNNING)
            continue;
        bool ok = false;
        if (lib_ready() && tagcache_search(&thumb_tcs, tag_filename))
        {
            ok = tagcache_retrieve(&thumb_tcs, job.idx, tag_filename, path, sizeof path);
            tagcache_search_finish(&thumb_tcs);
        }
        if (ok)
            ok = decode_cover(path, &job.w, &job.h, &job.px);
        job.ok = ok;
        job.state = JOB_DONE;
    }
}

bool lib_thumbs_busy(void)
{
    return thumbs_ready && (job.state != JOB_IDLE || requests_len);
}

bool lib_thumbs_work(void)
{
    if (!thumbs_ready)
        return false;
    bool arrived = false;
    if (job.state == JOB_DONE)
    {
        if (job.generation == generation)
        {
            /* the least recently used slot */
            int victim = 0;
            for (int i = 0; i < THUMB_SLOTS; i++)
            {
                if (thumbs[i].idx < 0) { victim = i; break; }
                if (thumbs[i].used < thumbs[victim].used)
                    victim = i;
            }
            struct thumb *t = &thumbs[victim];
            t->idx = job.idx;
            t->used = current_tick;
            t->failed = !job.ok;
            if (job.ok)
            {
                t->w = job.w;
                t->h = job.h;
                memcpy(t->px, job.px, job.w * job.h * 2);
            }
            arrived = true;
        }
        job.state = JOB_IDLE;
    }
    if (job.state == JOB_IDLE && requests_len)
    {
        job.idx = requests[--requests_len];
        job.generation = generation;
        job.state = JOB_RUNNING;
        queue_post(&thumb_queue, Q_THUMB_WORK, 0);
    }
    return arrived;
}
