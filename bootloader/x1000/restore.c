/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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

/*
 * Restore the system from a release zip on the card, with no computer:
 * osmium-pod-fiiom3k-<version>.zip, as published, in the card's root.
 * Its .rockbox goes to the internal storage (formatted only if it holds no
 * file system; settings and library are kept, a release has none) and its
 * rockbox.m3k to flash. A .rockbox left on the card becomes .rockbox.bak,
 * or it would be taken as an update at the next start.
 */

#include "x1000bootloader.h"

#if (CONFIG_STORAGE & STORAGE_NAND) && defined(ROCKBOX_FLASH_ADDR)

#include "system.h"
#include "kernel.h"
#include "button.h"
#include "storage.h"
#include "disk.h"
#include "file.h"
#include "dir.h"
#include "fs_attr.h"
#include "pathfuncs.h"
#include "zip.h"
#include "nand-ftl-ingenic.h"
#include "rbflash-x1000.h"
#include <stdio.h>
#include <string.h>

#define ZIP_PREFIX  "osmium-pod"

static int volume_of(int drive)
{
    for(int v = 0; v < NUM_VOLUMES; ++v)
        if(volume_drive(v) == drive)
            return v;
    return -1;
}

void bl_remove_tree(char* path)
{
    DIR* dir = opendir(path);
    if(!dir)
        return;

    size_t len = strlen(path);
    struct dirent* e;
    while((e = readdir(dir))) {
        if(is_dotdir_name(e->d_name) || len + strlen(e->d_name) + 2 > MAX_PATH)
            continue;
        struct dirinfo info = dir_get_info(dir, e);
        snprintf(path + len, MAX_PATH - len, "/%s", e->d_name);
        if(info.attribute & ATTR_DIRECTORY)
            bl_remove_tree(path);
        else
            remove(path);
        path[len] = '\0';
    }

    closedir(dir);
    rmdir(path);
}

/* the card's .rockbox becomes .rockbox.bak, replacing an older one */
void bl_retire_card_system(const char* card_root)
{
    static char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s" BOOTDIR, card_root);
    if(!dir_exists(src))
        return;
    snprintf(dst, sizeof(dst), "%s" BOOTDIR ".bak", card_root);
    bl_remove_tree(dst);
    rename(src, dst);
}

/* the newest osmium-pod*.zip in the card's root */
static bool find_zip(char* path, size_t size)
{
    DIR* dir = opendir("/");
    if(!dir)
        return false;

    time_t newest = 0;
    bool found = false;
    struct dirent* e;
    while((e = readdir(dir))) {
        size_t len = strlen(e->d_name);
        if(len < 4 || strncasecmp(e->d_name, ZIP_PREFIX, sizeof(ZIP_PREFIX) - 1) ||
           strcasecmp(e->d_name + len - 4, ".zip"))
            continue;
        struct dirinfo info = dir_get_info(dir, e);
        if(info.attribute & ATTR_DIRECTORY)
            continue;
        if(!found || info.mtime > newest) {
            snprintf(path, size, "/%s", e->d_name);
            newest = info.mtime;
            found = true;
        }
    }

    closedir(dir);
    return found;
}

static int progress(const struct zip_args* args, int pass, void* ctx)
{
    (void)ctx;
    if(pass == ZIP_PASS_START && args->entry % 8 == 0)
        splashf(0, "Restoring the system\n%d / %d", args->entry, args->entries);
    return 0;
}

static bool confirm(const char* zip)
{
    splashf(0, "Restore Osmium Pod from\n%s ?\n\n" BL_SELECT_NAME ": yes   "
            BL_QUIT_NAME ": no", zip + 1);
    for(;;) {
        int btn = get_button(TIMEOUT_BLOCK);
        if(btn == BL_SELECT)
            return true;
        if(btn == BL_QUIT)
            return false;
    }
}

void restore_from_zip(void)
{
    static char zip[MAX_PATH], path[MAX_PATH];
    static char root_nand[VOL_MAX_LEN + 2], root_card[VOL_MAX_LEN + 2];

    if(check_disk(true) != DISK_PRESENT)
        return;
    if(!find_zip(zip, sizeof(zip))) {
        splashf(5*HZ, "No " ZIP_PREFIX "-fiiom3k-*.zip\nin the card's root");
        return;
    }
    if(!confirm(zip))
        return;

    /* the internal storage, with a file system */
    splashf(0, "Starting the internal storage");
    int drive = nand_ftl_start() == 0 ? nand_ftl_drive() : -1;
    if(drive < 0) {
        splashf(5*HZ, "Internal storage\nnot available");
        return;
    }
    if(volume_of(drive) < 0)
        disk_mount(drive);
    if(volume_of(drive) < 0) {
        splashf(0, "Formatting the internal storage");
        disk_unmount(drive);
        if(nand_ftl_format() != 0 || disk_mount(drive) <= 0) {
            splashf(5*HZ, "Formatting failed");
            return;
        }
    }
    make_volume_root(volume_of(drive), root_nand);
    make_volume_root(volume_of(0), root_card);

    /* the release's files: .rockbox (and INSTALL.txt, harmless) */
    splashf(0, "Restoring the system");
    struct zip* z = zip_open(zip, true);
    if(!z) {
        splashf(5*HZ, "Cannot open\n%s", zip + 1);
        return;
    }
    int rc = zip_extract(z, root_nand, progress, NULL);
    zip_close(z);
    storage_flush();
    if(rc != 0) {
        splashf(5*HZ, "Restore failed (%d)\nThe zip may be damaged", rc);
        return;
    }

    /* the firmware the bootloader starts */
    splashf(0, "Installing Rockbox");
    snprintf(path, sizeof(path), "%s" BOOTDIR "/" BOOTFILE, root_nand);
    rc = rbflash_write_file(path);
    if(rc != RBF_SUCCESS) {
        splashf(5*HZ, "Restore failed\n%s", rbflash_strerror(rc));
        return;
    }

    bl_retire_card_system(root_card);
    storage_flush();

    splashf(3*HZ, "Osmium Pod is restored\nRestarting");
    system_reboot();
}

#endif
