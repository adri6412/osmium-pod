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
 * Rockbox stored in NAND, in place of the original firmware's kernel: the
 * bootloader side (boot, sync from the card, recovery menu). The flash
 * format and I/O are in firmware/target/mips/ingenic_x1000/rbflash-x1000.c.
 */

#include "x1000bootloader.h"

#ifdef ROCKBOX_FLASH_ADDR

#include "core_alloc.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "rbflash-x1000.h"
#include <stdio.h>

#if defined(HAVE_BOOTDATA) || defined(HAVE_MULTIBOOT)
#include "multiboot.h"
#endif

#ifdef HAVE_DEVICEDATA
#include "devicedata.h"
#endif

#define RBFLASH_FILE BOOTDIR "/" BOOTFILE

int load_rockbox_flash(size_t* sizep)
{
    size_t max_size;
    int handle = core_alloc_maximum(&max_size, &buflib_ops_locked);
    if(handle < 0)
        return RBF_NO_MEMORY;

    uint8_t* buf = core_get_data(handle);
    int len = rbflash_load(buf, max_size);
    if(len < 0) {
        core_free(handle);
        /* an empty region is expected before the first install, stay quiet */
        if(len != RBF_NO_IMAGE)
            splashf(5*HZ, "Rockbox in flash is damaged\n%s", rbflash_strerror(len));
        return len;
    }

#ifdef HAVE_BOOTDATA
    write_bootdata(buf, len, 0);
#endif
#ifdef HAVE_DEVICEDATA
    write_devicedata(buf, len);
#endif

    core_shrink(handle, buf, len);
    *sizep = len;
    return handle;
}

void rockbox_flash_sync(bool force)
{
    if(check_disk(false) != DISK_PRESENT)
        return;

    int rc = rbflash_compare_file(RBFLASH_FILE);
    if(rc < 0 || (rc == 0 && !force))
        return; /* no file, or up to date: the common case */

    splashf(0, "Updating Rockbox in flash\nDo not power off");
    rc = rbflash_write_file(RBFLASH_FILE);
    if(rc != RBF_SUCCESS)
        splashf(5*HZ, "Flash update failed\n%s (%d)", rbflash_strerror(rc), rc);
}

void rockbox_flash_install(void)
{
    if(check_disk(true) != DISK_PRESENT) {
        splashf(5*HZ, "Install aborted\nCannot access SD card");
        return;
    }

    splashf(0, "Installing Rockbox in flash\n" RBFLASH_FILE);

    int rc = rbflash_write_file(RBFLASH_FILE);

    static char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d)", rbflash_strerror(rc), rc);
    const char* msg1 = rc == 0 ? "Success" : buf;
    const char* msg2 = "Press " BL_QUIT_NAME " to continue";
    splashf(0, "%s\n%s", msg1, msg2);

    while(get_button(TIMEOUT_BLOCK) != BL_QUIT);
}

void dump_rockbox_flash(void)
{
    dump_flash_file("/rockbox_flash.img", ROCKBOX_FLASH_ADDR,
                    ROCKBOX_FLASH_LENGTH);
}

#endif /* ROCKBOX_FLASH_ADDR */
