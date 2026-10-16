/* Osmium Sound for Rockbox: the system (.rockbox) on the internal storage
 *
 * The card stays the root ("/": the music, so its paths never change).
 * Once the system has moved, the firmware sends "/.rockbox" to the internal
 * storage (rb_namespace.c, nand-ftl-ingenic.c) and the card keeps only the
 * music. Without a card, the internal storage is the root by itself.
 *
 * Moving the system either way, and taking a release copied to the card,
 * happen at boot, before anything has a file open in there: the settings
 * only leave a request in .rockbox and restart.
 */
#include "config.h"
#include "osm_storage.h"

#if (CONFIG_STORAGE & STORAGE_NAND) && defined(HAVE_ROCKBOX_DIR_REDIRECT)

#include "system.h"
#include "kernel.h"
#include "storage.h"
#include "disk.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "misc.h"
#include "powermgmt.h"
#include "string-extra.h"
#include "fs_attr.h"
#include "nand-ftl-ingenic.h"
#include "rbflash-x1000.h"
#include "osmium.h"
#include "osm_strings.h"
#include <stdio.h>
#include <string.h>

#define SYSTEM_DIR      ROCKBOX_DIR             /* "/.rockbox" */
#define BACKUP_DIR      ROCKBOX_DIR ".bak"      /* the card's old copy */
#define REQUEST_NAME    "osm-move"              /* in .rockbox */
#define REQ_INTERNAL    "internal"
#define REQ_CARD        "card"

enum copy_mode {
    COPY_ALL,       /* moving the system */
    COPY_RELEASE,   /* a release copied to the card: keep the user's state */
};

static int nand_drive = -1;
static int nand_volume = -1;
static int card_volume = -1;
static char nand_root[VOL_MAX_LEN + 2];
static char card_root[VOL_MAX_LEN + 2];
static bool system_internal;

/* ---------------------------------------------------------------- helpers */

static int volume_of(int drive)
{
    for (int v = 0; v < NUM_VOLUMES; v++)
        if (volume_drive(v) == drive)
            return v;
    return -1;
}

static void find_volumes(void)
{
    nand_drive = nand_ftl_drive();
    nand_volume = card_volume = -1;
    nand_root[0] = card_root[0] = '\0';
    if (nand_drive < 0)
        return;

    nand_volume = volume_of(nand_drive);
    for (int d = 0; d < NUM_DRIVES; d++)
        if (d != nand_drive && card_volume < 0)
            card_volume = volume_of(d);

    if (nand_volume >= 0)
        make_volume_root(nand_volume, nand_root);
    if (card_volume >= 0)
        make_volume_root(card_volume, card_root);
}

static void join(char *buf, const char *root, const char *path)
{
    snprintf(buf, MAX_PATH, "%s%s", root, path);
}

static bool skip_name(const char *name, enum copy_mode mode)
{
    if (!strcasecmp(name, REQUEST_NAME) || !strcasecmp(name, "dircache.dat"))
        return true;
    if (mode == COPY_ALL)
        return false;

    /* settings, database, resume, bookmarks, favorites: not in a release,
     * and whatever the card holds of them is older than the system's */
    static const char *const state_ext[] = {
        ".cfg", ".tcd", ".bmark", ".m3u8", ".m3u", ".dat", ".tmp",
    };
    const char *ext = strrchr(name, '.');
    for (unsigned i = 0; ext && i < ARRAYLEN(state_ext); i++)
        if (!strcasecmp(ext, state_ext[i]))
            return true;
    return !strcasecmp(name, "nvram.bin") ||
           !strcasecmp(name, ".playlist_control");
}

/* Walk src (a directory) and copy it to dst, or with dst == NULL only count
 * the files. The paths are edited in place, so they need MAX_PATH bytes. */
static unsigned char copy_buf[32 * 1024];
static int copied, to_copy;
static const char *copy_msg;

static bool copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return false;
    }

    bool ok = true;
    off_t total = 0;
    ssize_t n;
    while ((n = read(in, copy_buf, sizeof copy_buf)) > 0) {
        if (write(out, copy_buf, n) != n) {
            ok = false;
            break;
        }
        total += n;
    }

    if (n < 0 || total != ffilesize(in))
        ok = false;
    close(in);
    if (close(out) < 0)
        ok = false;
    return ok;
}

static bool copy_tree(char *src, char *dst, enum copy_mode mode)
{
    DIR *dir = opendir(src);
    if (!dir)
        return false;
    if (dst && !dir_exists(dst) && mkdir(dst) < 0) {
        closedir(dir);
        return false;
    }

    bool ok = true;
    size_t slen = strlen(src), dlen = dst ? strlen(dst) : 0;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (is_dotdir_name(e->d_name) || skip_name(e->d_name, mode))
            continue;
        struct dirinfo info = dir_get_info(dir, e);
        if (slen + 1 + strlen(e->d_name) >= MAX_PATH ||
            dlen + 1 + strlen(e->d_name) >= MAX_PATH) {
            ok = false;
            break;
        }

        snprintf(src + slen, MAX_PATH - slen, "/%s", e->d_name);
        if (dst)
            snprintf(dst + dlen, MAX_PATH - dlen, "/%s", e->d_name);

        if (info.attribute & ATTR_DIRECTORY) {
            ok = copy_tree(src, dst, mode);
        } else if (dst) {
            osm_boot_message(copy_msg, copied, to_copy);
            ok = copy_file(src, dst);
            copied++;
        } else {
            to_copy++;
        }

        src[slen] = '\0';
        if (dst)
            dst[dlen] = '\0';
    }

    closedir(dir);
    return ok;
}

static bool copy_system(const char *from_root, const char *to_root,
                        enum copy_mode mode, const char *msg)
{
    static char src[MAX_PATH], dst[MAX_PATH];

    copy_msg = msg;
    copied = to_copy = 0;
    join(src, from_root, SYSTEM_DIR);
    if (!copy_tree(src, NULL, mode))
        return false;

    join(src, from_root, SYSTEM_DIR);
    join(dst, to_root, SYSTEM_DIR);
    bool ok = copy_tree(src, dst, mode);
    osm_boot_message(msg, to_copy, to_copy);
    return ok;
}

/* remove a directory and everything in it (path: MAX_PATH bytes, edited) */
static void remove_tree(char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return;

    size_t len = strlen(path);
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (is_dotdir_name(e->d_name) || len + 1 + strlen(e->d_name) >= MAX_PATH)
            continue;
        struct dirinfo info = dir_get_info(dir, e);
        snprintf(path + len, MAX_PATH - len, "/%s", e->d_name);
        if (info.attribute & ATTR_DIRECTORY)
            remove_tree(path);
        else
            remove(path);
        path[len] = '\0';
    }

    closedir(dir);
    rmdir(path);
}

/* the card's .rockbox becomes .rockbox.bak, replacing an older one */
static void retire_card_system(void)
{
    static char path[MAX_PATH], to[MAX_PATH];
    join(path, card_root, BACKUP_DIR);
    remove_tree(path);
    join(path, card_root, SYSTEM_DIR);
    join(to, card_root, BACKUP_DIR);
    rename(path, to);
}

static void restart(void)
{
    storage_flush();
    system_reboot();
}

static void failed(enum osm_str_id msg)
{
    osm_boot_message(osm_str(msg), -1, -1);
    sleep(4 * HZ);
}

/* ---------------------------------------------------------------- moves */

static void move_to_internal(void)
{
    const char *msg = osm_str(S_MOVING_TO_INTERNAL);
    osm_boot_message(msg, 0, 0);

    disk_unmount(nand_drive);
    bool ok = nand_ftl_format() == 0 && disk_mount(nand_drive) > 0;
    find_volumes();
    ok = ok && nand_volume >= 0 &&
         copy_system(card_root, nand_root, COPY_ALL, msg);

    if (!ok) {
        /* leave the internal storage blank: the system stays on the card */
        disk_unmount(nand_drive);
        nand_ftl_wipe();
        failed(S_MOVE_FAILED);
        return;
    }

    retire_card_system();
    restart();
}

static void move_to_card(void)
{
    const char *msg = osm_str(S_MOVING_TO_CARD);
    osm_boot_message(msg, 0, 0);

    if (!copy_system(nand_root, card_root, COPY_ALL, msg)) {
        failed(S_MOVE_FAILED);
        return;
    }

    static char path[MAX_PATH];
    join(path, card_root, BACKUP_DIR);
    remove_tree(path);

    disk_unmount(nand_drive);
    nand_ftl_wipe();
    restart();
}

/* a release copied to the card (by hand or by the web installer) */
static void update_from_card(void)
{
    const char *msg = osm_str(S_UPDATING_FROM_CARD);
    if (copy_system(card_root, nand_root, COPY_RELEASE, msg))
        retire_card_system();
    else
        failed(S_UPDATE_FAILED);    /* tried again at the next start */
}

/* the bootloader starts the copy of rockbox.m3k kept in flash, and it only
 * looks for updates on the card: keep that copy in step with the system */
static void update_flash(void)
{
    static char path[MAX_PATH];
    join(path, nand_root, SYSTEM_DIR "/" BOOTFILE);
    if (rbflash_compare_file(path) <= 0)
        return;     /* up to date, or no file */

    osm_boot_message(osm_str(S_UPDATING_FIRMWARE), -1, -1);
    /* restart only once flash really matches: never a restart loop */
    if (rbflash_write_file(path) == RBF_SUCCESS &&
        rbflash_compare_file(path) == 0)
        restart();  /* start the new firmware */
    failed(S_UPDATE_FAILED);
}

static void read_request(char *buf, int len)
{
    static char path[MAX_PATH];
    join(path, system_internal ? nand_root : card_root,
         SYSTEM_DIR "/" REQUEST_NAME);

    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, len - 1);
    close(fd);
    remove(path);       /* one try only */
    buf[n > 0 ? n : 0] = '\0';
}

/* ---------------------------------------------------------------- boot */

void osm_storage_mount(void)
{
    static char path[MAX_PATH];

    find_volumes();
    join(path, nand_root, SYSTEM_DIR);
    system_internal = nand_volume >= 0 && dir_exists(path);

    /* with the card as the root, /.rockbox must point inside; without a
     * card the internal storage is the root and needs no help */
    if (system_internal && nand_volume != 0)
        ns_set_rockbox_dir(nand_drive, nand_volume);
}

void osm_storage_boot(void)
{
    if (nand_drive < 0)
        return;

    char req[16];
    read_request(req, sizeof req);

    if (card_volume >= 0) {
        if (!system_internal && !strcmp(req, REQ_INTERNAL))
            move_to_internal();
        else if (system_internal && !strcmp(req, REQ_CARD))
            move_to_card();
    }

    if (!system_internal)
        return;

    static char path[MAX_PATH];
    join(path, card_root, SYSTEM_DIR "/" BOOTFILE);
    if (card_volume >= 0 && file_exists(path))
        update_from_card();

    update_flash();
}

/* ---------------------------------------------------------------- settings */

bool osm_storage_available(void)
{
    return nand_ftl_drive() >= 0;
}

bool osm_storage_system_internal(void)
{
    return system_internal;
}

bool osm_storage_card_present(void)
{
    find_volumes();
    return card_volume >= 0;
}

void osm_storage_info(char *buf, int len)
{
    find_volumes();
    if (nand_drive < 0) {
        strmemccpy(buf, osm_str(S_NOT_AVAILABLE), len);
        return;
    }
    if (nand_volume < 0) {
        strmemccpy(buf, osm_str(S_NOT_IN_USE), len);
        return;
    }

    sector_t size = 0, free = 0;
    volume_size(nand_volume, &size, &free);
    char fs[24], ts[24];
    output_dyn_value(fs, sizeof fs, (int64_t)free * 1024, byte_units, 4, true);
    output_dyn_value(ts, sizeof ts, (int64_t)size * 1024, byte_units, 4, true);
    snprintf(buf, len, osm_str(S_FREE_OF), fs, ts);
}

void osm_storage_fs_changed(void)
{
    int was_root = nand_volume == 0;
    find_volumes();
    if (was_root && nand_volume == 0 && card_volume >= 0)
        sys_reboot();
}

void osm_storage_request_move(bool to_internal)
{
    /* SYSTEM_DIR is wherever the system lives now */
    int fd = open(SYSTEM_DIR "/" REQUEST_NAME, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        const char *req = to_internal ? REQ_INTERNAL : REQ_CARD;
        write(fd, req, strlen(req));
        close(fd);
    }
    sys_reboot();
}

#else /* no internal storage (simulator) */

void osm_storage_mount(void) {}
void osm_storage_boot(void) {}
bool osm_storage_available(void) { return false; }
bool osm_storage_system_internal(void) { return false; }
bool osm_storage_card_present(void) { return true; }
void osm_storage_info(char *buf, int len) { if (len > 0) buf[0] = '\0'; }
void osm_storage_request_move(bool to_internal) { (void)to_internal; }
void osm_storage_fs_changed(void) {}

#endif
