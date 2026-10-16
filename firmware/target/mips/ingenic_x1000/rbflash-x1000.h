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

#ifndef __RBFLASH_X1000_H__
#define __RBFLASH_X1000_H__

#include <stddef.h>

/* Rockbox image in NAND at ROCKBOX_FLASH_ADDR (see rbflash-x1000.c) */

enum {
    RBF_SUCCESS     = 0,
    RBF_NO_IMAGE    = -1,
    RBF_NAND_OPEN   = -2,
    RBF_NAND_READ   = -3,
    RBF_NAND_WRITE  = -4,
    RBF_NO_SPACE    = -5,
    RBF_BAD_CHKSUM  = -6,
    RBF_NO_MEMORY   = -7,
    RBF_FILE_IO     = -8,
    RBF_TOO_BIG     = -9,
    RBF_VERIFY      = -10,
};

const char* rbflash_strerror(int rc);

/* Read the image into buffer and check it. Returns the length of the
 * binary, which starts at buffer, or a negative RBF_ error. */
int rbflash_load(void* buffer, size_t bufsize);

/* 1 if the rockbox.<ext> file differs from the image in flash (or flash
 * holds none), 0 if they match, or a negative RBF_ error */
int rbflash_compare_file(const char* filename);

/* Check a rockbox.<ext> file and write it to flash, then read it back */
int rbflash_write_file(const char* filename);

#endif /* __RBFLASH_X1000_H__ */
