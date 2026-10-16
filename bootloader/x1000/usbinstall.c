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
 * USB installer (build type U): the bootloader, loaded over USB boot by the
 * web installer together with a package it puts at the top of RAM. It never
 * goes to flash itself. It installs everything with no card and no menu:
 *
 *  1. a backup on the card, if the original firmware is still in flash;
 *  2. the bootloader in block 0 (first: whatever fails later, the player
 *     then starts a Rockbox bootloader, which can always be fixed);
 *  3. the Rockbox image (rbflash-x1000.c);
 *  4. the .rockbox folder on the internal storage (nand-ftl-ingenic.c),
 *     formatted if it holds no file system; a system found on the card is
 *     carried over first, and the card's copy becomes .rockbox.bak.
 *
 * Package, at X1000_USB_INSTALLER_PAYLOAD_ADDR (the linker leaves that
 * memory alone in this build):
 *   0  "OSMINST1"
 *   8  u32 length of the tar that follows
 *  12  u32 CRC-32 (zlib) of the tar
 *  16  u32 flags
 *  32  ustar archive: "bootloader.m3k", then ".rockbox/..." files
 */

#include "x1000bootloader.h"

#ifdef X1000_USB_INSTALLER

#include "system.h"
#include "kernel.h"
#include "storage.h"
#include "disk.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "fs_attr.h"
#include "crc32.h"
#include "nand-ingenic.h"
#include "nand-ftl-ingenic.h"
#include "installer-ingenic.h"
#include "rbflash-x1000.h"
#include <stdio.h>
#include <string.h>

#define PKG_MAGIC           "OSMINST1"
#define PKG_HDR_SIZE        32
#define PKG_ALLOW_NO_BACKUP 0x01    /* the user chose to go on without */

#define SYSTEM_DIR  BOOTDIR             /* "/.rockbox" */

extern unsigned char pluginbuf[];       /* the package, see app.lds */

/* ---------------------------------------------------------------- tar */

struct tar_entry {
    char name[101];
    const uint8_t* data;
    size_t size;
};

/* Next regular file of the archive; false at its end */
static bool tar_next(const uint8_t* tar, size_t len, size_t* off,
                     struct tar_entry* e)
{
    while(*off + 512 <= len) {
        const uint8_t* h = tar + *off;
        if(h[0] == 0)
            return false;

        size_t size = 0;
        for(int i = 124; i < 136 && h[i] >= '0' && h[i] <= '7'; ++i)
            size = size * 8 + (h[i] - '0');

        size_t data = *off + 512;
        if(data + size > len)
            return false;
        *off = data + ((size + 511) & ~511);

        if(h[156] != '0' && h[156] != 0)
            continue;   /* not a regular file */

        memcpy(e->name, h, 100);
        e->name[100] = '\0';
        e->data = tar + data;
        e->size = size;
        return true;
    }

    return false;
}

static bool tar_find(const uint8_t* tar, size_t len, const char* name,
                     struct tar_entry* e)
{
    size_t off = 0;
    while(tar_next(tar, len, &off, e))
        if(!strcmp(e->name, name))
            return true;
    return false;
}

/* the bootloader archive inside the package, for install_bootloader_mem() */
static struct tar_entry bl_archive;

static long bootloader_file(const char* name, const void** data)
{
    struct tar_entry e;
    if(!tar_find(bl_archive.data, bl_archive.size, name, &e))
        return -1;
    *data = e.data;
    return e.size;
}

/* ---------------------------------------------------------------- files */

static char root_nand[VOL_MAX_LEN + 2];
static char root_card[VOL_MAX_LEN + 2];

static int volume_of(int drive)
{
    for(int v = 0; v < NUM_VOLUMES; ++v)
        if(volume_drive(v) == drive)
            return v;
    return -1;
}

static void find_roots(void)
{
    int nand = nand_ftl_drive();
    int vn = nand >= 0 ? volume_of(nand) : -1;
    int vc = -1;
    for(int d = 0; d < NUM_DRIVES && vc < 0; ++d)
        if(d != nand)
            vc = volume_of(d);

    root_nand[0] = root_card[0] = '\0';
    if(vn >= 0)
        make_volume_root(vn, root_nand);
    if(vc >= 0)
        make_volume_root(vc, root_card);
}

/* create the directories leading to path */
static bool make_parents(char* path)
{
    for(char* p = path + 1; *p; ++p) {
        if(*p != '/')
            continue;
        *p = '\0';
        bool ok = dir_exists(path) || mkdir(path) == 0;
        *p = '/';
        if(!ok)
            return false;
    }

    return true;
}

static bool write_file(const char* path, const void* data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd < 0)
        return false;
    bool ok = write(fd, data, size) == (ssize_t)size;
    return close(fd) == 0 && ok;
}

static uint8_t copy_buf[32 * 1024];

static bool copy_file(const char* src, const char* dst)
{
    int in = open(src, O_RDONLY);
    if(in < 0)
        return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(out < 0) {
        close(in);
        return false;
    }

    bool ok = true;
    ssize_t n;
    while((n = read(in, copy_buf, sizeof(copy_buf))) > 0)
        if(write(out, copy_buf, n) != n) {
            ok = false;
            break;
        }

    close(in);
    return close(out) == 0 && ok && n == 0;
}

/* copy the directory src to dst (MAX_PATH buffers, edited in place) */
static bool copy_tree(char* src, char* dst)
{
    DIR* dir = opendir(src);
    if(!dir)
        return false;
    if(!dir_exists(dst) && mkdir(dst) < 0) {
        closedir(dir);
        return false;
    }

    bool ok = true;
    size_t sl = strlen(src), dl = strlen(dst);
    struct dirent* e;
    while(ok && (e = readdir(dir))) {
        if(is_dotdir_name(e->d_name) || !strcasecmp(e->d_name, "dircache.dat"))
            continue;
        if(sl + strlen(e->d_name) + 2 > MAX_PATH ||
           dl + strlen(e->d_name) + 2 > MAX_PATH) {
            ok = false;
            break;
        }

        struct dirinfo info = dir_get_info(dir, e);
        snprintf(src + sl, MAX_PATH - sl, "/%s", e->d_name);
        snprintf(dst + dl, MAX_PATH - dl, "/%s", e->d_name);
        ok = (info.attribute & ATTR_DIRECTORY) ? copy_tree(src, dst)
                                               : copy_file(src, dst);
        src[sl] = dst[dl] = '\0';
    }

    closedir(dir);
    return ok;
}

/* ---------------------------------------------------------------- steps */

static bool original_firmware_present(void)
{
    static uint8_t buf[4];
    struct nand_drv* drv = nand_drv_init();
    bool present = false;

    nand_lock(drv);
    if(nand_drv_open(drv) == NAND_SUCCESS) {
        /* the FiiO kernel is a uImage where Rockbox now goes */
        if(nand_read_bytes(drv, ROCKBOX_FLASH_ADDR, 4, buf) == NAND_SUCCESS)
            present = load_be32(buf) == 0x27051956;
        nand_drv_close(drv);
    }
    nand_unlock(drv);
    return present;
}

static bool backup(uint32_t flags)
{
    if(!original_firmware_present())
        return true;    /* nothing left to save */

    if(check_disk(false) != DISK_PRESENT) {
        if(flags & PKG_ALLOW_NO_BACKUP)
            return true;
        splashf(0, "The FiiO firmware is still here\n"
                   "Insert a card for its backup,\n"
                   "or allow installing without one\n"
                   "on the web page");
        return false;
    }

    /* an earlier, interrupted run may already have saved it, and block 0
     * may now hold a Rockbox bootloader: never overwrite the real one */
    if(!file_exists(BOOTBACKUP_FILE)) {
        splashf(0, "Saving a backup to the card\n" BOOTBACKUP_FILE);
        int rc = backup_bootloader(BOOTBACKUP_FILE);
        if(rc != IERR_SUCCESS) {
            splashf(0, "Backup failed\n%s", installer_strerror(rc));
            return false;
        }
    }

    /* the whole flash: the only copy of the original firmware */
    if(!file_exists("/flash.img"))
        dump_entire_flash();
    if(!file_exists("/flash.img")) {
        splashf(0, "Backup failed\nflash.img");
        return false;
    }

    return true;
}

/* the internal storage, with a file system, mounted; root_nand set */
static bool prepare_internal(bool* fresh)
{
    static char path[MAX_PATH];
    int drive = nand_ftl_drive();
    if(drive < 0)
        return false;

    find_roots();
    *fresh = !root_nand[0];
    if(*fresh) {
        splashf(0, "Formatting the internal storage");
        disk_unmount(drive);
        if(nand_ftl_format() != 0 || disk_mount(drive) <= 0)
            return false;
        find_roots();
    }

    snprintf(path, sizeof(path), "%s" SYSTEM_DIR, root_nand);
    *fresh = !dir_exists(path);
    return root_nand[0] != '\0';
}

static bool install_files(const uint8_t* tar, size_t len)
{
    static char src[MAX_PATH], dst[MAX_PATH];
    bool fresh;
    if(!prepare_internal(&fresh)) {
        splashf(0, "Internal storage failed");
        return false;
    }

    /* a system on the card (settings, library): carry it over */
    if(fresh && root_card[0]) {
        snprintf(src, sizeof(src), "%s" SYSTEM_DIR, root_card);
        snprintf(dst, sizeof(dst), "%s" SYSTEM_DIR, root_nand);
        if(dir_exists(src)) {
            splashf(0, "Copying the settings\nfrom the card");
            if(!copy_tree(src, dst)) {
                splashf(0, "Copy from the card failed");
                return false;
            }
        }
    }

    /* the release */
    int count = 0, done = 0;
    size_t off = 0;
    struct tar_entry e;
    while(tar_next(tar, len, &off, &e))
        count += !strncmp(e.name, ".rockbox/", 9);

    off = 0;
    while(tar_next(tar, len, &off, &e)) {
        if(strncmp(e.name, ".rockbox/", 9))
            continue;
        if(done % 8 == 0)
            splashf(0, "Installing the files\n%d / %d", done, count);

        snprintf(dst, sizeof(dst), "%s/%s", root_nand, e.name);
        if(!make_parents(dst) || !write_file(dst, e.data, e.size)) {
            splashf(0, "Writing failed\n%s", e.name);
            return false;
        }
        done++;
    }

    /* the card's copy would otherwise be taken as an update at boot */
    if(root_card[0])
        bl_retire_card_system(root_card);

    return true;
}

void usb_install(void)
{
    const uint8_t* pkg = pluginbuf;
    uint32_t len = load_le32(pkg + 8);
    uint32_t crc = load_le32(pkg + 12);
    uint32_t flags = load_le32(pkg + 16);
    const uint8_t* tar = pkg + PKG_HDR_SIZE;

    if(memcmp(pkg, PKG_MAGIC, 8) ||
       len > X1000_USB_INSTALLER_PAYLOAD_SIZE - PKG_HDR_SIZE)
        return;     /* loaded without a package: just the recovery menu */

    splashf(0, "Checking the package");
    if(~crc_32r(tar, len, 0xffffffff) != crc) {
        splashf(5*HZ, "The package is damaged\nTry again");
        return;
    }

    struct tar_entry rb;
    if(!tar_find(tar, len, "bootloader.m3k", &bl_archive) ||
       !tar_find(tar, len, ".rockbox/" BOOTFILE, &rb)) {
        splashf(5*HZ, "The package is incomplete");
        return;
    }

    const char* failed = NULL;
    if(!backup(flags)) {
        failed = "";    /* the message is on screen */
    } else {
        splashf(0, "Installing the bootloader");
        int rc = install_bootloader_mem(bootloader_file);
        if(rc != IERR_SUCCESS)
            failed = installer_strerror(rc);
    }

    if(!failed) {
        splashf(0, "Installing Rockbox");
        int rc = rbflash_write_buffer(rb.data, rb.size);
        if(rc != RBF_SUCCESS)
            failed = rbflash_strerror(rc);
    }

    if(!failed && !install_files(tar, len))
        failed = "";

    storage_flush();

    if(failed) {
        if(*failed)
            splashf(0, "Installation failed\n%s", failed);
        sleep(5*HZ);
        return;     /* to the recovery menu */
    }

    splashf(3*HZ, "Osmium Pod is installed\nRestarting");
    system_reboot();
}

#endif /* X1000_USB_INSTALLER */
