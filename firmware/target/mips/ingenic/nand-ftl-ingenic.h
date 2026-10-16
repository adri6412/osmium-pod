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

#ifndef __NAND_FTL_INGENIC_H__
#define __NAND_FTL_INGENIC_H__

#include "config.h"
#include <stdbool.h>

/* Internal storage: the Dhara FTL on the NAND from NAND_FTL_START to the
 * end of the chip, seen by Rockbox as a 512-byte sector drive. */

struct nand_ftl_info {
    bool present;           /* NAND found and FTL started */
    sector_t sectors;       /* size of the drive, in 512-byte sectors */
    unsigned bad_blocks;    /* in the FTL area */
    unsigned used_pages;    /* pages holding data */
    unsigned total_pages;   /* pages the FTL can hold */
};

/* Rockbox drive number of the internal storage, -1 if absent */
int nand_ftl_drive(void);

/* Start the internal storage if nand_init() left it (NAND_FTL_LAZY, the
 * bootloader); 0 once it is ready, -1 if the NAND is unusable */
int nand_ftl_start(void);

/* Its drive number even before it is started, -1 if none */
int nand_ftl_drive_number(void);

void nand_ftl_get_info(struct nand_ftl_info* info);

/* Erase the FTL area and write an empty FAT16 file system. The volume
 * must be unmounted first. Returns 0 or a negative error. */
int nand_ftl_format(void);

/* Erase the FTL area, leaving the drive blank (no file system). The volume
 * must be unmounted first. */
int nand_ftl_wipe(void);

#endif /* __NAND_FTL_INGENIC_H__ */
