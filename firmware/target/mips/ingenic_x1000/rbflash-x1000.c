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
 * Rockbox stored in NAND, in place of the original firmware's kernel
 * (ROCKBOX_FLASH_ADDR, ROCKBOX_FLASH_LENGTH). Used by the bootloader,
 * which boots it, and by the firmware, which updates it when the system
 * lives on the internal storage, where the bootloader cannot look.
 *
 * The flash region holds a small header followed by the rockbox.<ext> file
 * exactly as produced by "scramble -add" (checksum, model, image). Data is
 * laid out contiguously over the good blocks of the region; blocks carrying
 * a bad block marker are skipped both when writing and when reading.
 */

#include "config.h"
#include "rbflash-x1000.h"
#include "core_alloc.h"
#include "system.h"
#include "file.h"
#include "checksum.h"
#include "nand-ingenic.h"
#include <string.h>

static const char rbflash_magic[8] = {'O','S','M','R','B','X','0','1'};

struct rbflash_hdr {
    char magic[8];
    uint32_t length;    /* length of the scrambled file */
    uint32_t reserved;
};

/* scramble -add header: big-endian checksum + 4-byte model name */
#define SCRAMBLE_HDR_SIZE 8

const char* rbflash_strerror(int rc)
{
    switch(rc) {
    case RBF_SUCCESS:    return "Success";
    case RBF_NO_IMAGE:   return "No image in flash";
    case RBF_NAND_OPEN:  return "NAND open error";
    case RBF_NAND_READ:  return "NAND read error";
    case RBF_NAND_WRITE: return "NAND write error";
    case RBF_NO_SPACE:   return "Too many bad blocks";
    case RBF_BAD_CHKSUM: return "Bad checksum";
    case RBF_NO_MEMORY:  return "Out of memory";
    case RBF_FILE_IO:    return "File I/O error";
    case RBF_TOO_BIG:    return "Image too big";
    case RBF_VERIFY:     return "Verify failed";
    default:             return "Unknown error!?";
    }
}

/* Cursor over the pages of the good blocks in the flash region */
struct rbflash_cursor {
    struct nand_drv* ndrv;
    nand_page_t page;
    nand_page_t end_page;
};

static void cursor_init(struct rbflash_cursor* c, struct nand_drv* ndrv)
{
    c->ndrv = ndrv;
    c->page = ROCKBOX_FLASH_ADDR / ndrv->chip->page_size;
    c->end_page = c->page + ROCKBOX_FLASH_LENGTH / ndrv->chip->page_size;
}

static bool page_is_bad_block(struct nand_drv* ndrv, int rc)
{
    /* Same rules as the uImage reader: an ECC failure on the first page or
     * a bad block marker means the block is skipped. */
    return rc == NAND_ERR_ECC_FAIL ||
           ndrv->page_buf[ndrv->chip->bbm_pos] != 0xff;
}

/* Read the next data page into ndrv->page_buf, skipping bad blocks */
static int cursor_read(struct rbflash_cursor* c)
{
    struct nand_drv* ndrv = c->ndrv;

    while(c->page < c->end_page) {
        int rc = nand_page_read(ndrv, c->page, ndrv->page_buf);

        if(!(c->page & (ndrv->ppb - 1)) && page_is_bad_block(ndrv, rc)) {
            c->page += ndrv->ppb;
            continue;
        }

        if(rc < 0)
            return RBF_NAND_READ;

        c->page++;
        return RBF_SUCCESS;
    }

    return RBF_NO_SPACE;
}

/* Read 'len' bytes from the start of the region */
static int rbflash_read(struct nand_drv* ndrv, void* buf, size_t len)
{
    struct rbflash_cursor c;
    unsigned pg_size = ndrv->chip->page_size;
    uint8_t* dst = buf;

    cursor_init(&c, ndrv);
    while(len > 0) {
        int rc = cursor_read(&c);
        if(rc < 0)
            return rc;

        size_t count = MIN(len, pg_size);
        memcpy(dst, ndrv->page_buf, count);
        dst += count;
        len -= count;
    }

    return RBF_SUCCESS;
}

/* Bytes that fit in the good blocks of the region */
static size_t rbflash_capacity(struct nand_drv* ndrv)
{
    struct rbflash_cursor c;
    size_t blk_size = ndrv->chip->page_size << ndrv->chip->log2_ppb;
    size_t capacity = 0;

    cursor_init(&c, ndrv);
    for(; c.page < c.end_page; c.page += ndrv->ppb) {
        int rc = nand_page_read(ndrv, c.page, ndrv->page_buf);
        if(!page_is_bad_block(ndrv, rc))
            capacity += blk_size;
    }

    return capacity;
}

/* Write 'len' bytes to the start of the region. Only the blocks needed to
 * hold the data are erased; factory bad blocks are left untouched. */
static int rbflash_write(struct nand_drv* ndrv, const void* buf, size_t len)
{
    struct rbflash_cursor c;
    unsigned pg_size = ndrv->chip->page_size;
    const uint8_t* src = buf;

    /* check before erasing anything, so the old image survives */
    if(len > rbflash_capacity(ndrv))
        return RBF_NO_SPACE;

    cursor_init(&c, ndrv);
    while(len > 0) {
        if(c.page >= c.end_page)
            return RBF_NO_SPACE;

        /* at the start of each block, skip it if bad, else erase it */
        if(!(c.page & (ndrv->ppb - 1))) {
            int rc = nand_page_read(ndrv, c.page, ndrv->page_buf);
            if(page_is_bad_block(ndrv, rc)) {
                c.page += ndrv->ppb;
                continue;
            }

            if(nand_block_erase(ndrv, c.page) < 0)
                return RBF_NAND_WRITE;
        }

        size_t count = MIN(len, pg_size);
        memset(ndrv->page_buf, 0xff, ndrv->fpage_size);
        memcpy(ndrv->page_buf, src, count);
        if(nand_page_program(ndrv, c.page, ndrv->page_buf) < 0)
            return RBF_NAND_WRITE;

        c.page++;
        src += count;
        len -= count;
    }

    return RBF_SUCCESS;
}

/* Compare 'len' bytes at the start of the region with 'buf' */
static int rbflash_verify(struct nand_drv* ndrv, const void* buf, size_t len)
{
    struct rbflash_cursor c;
    unsigned pg_size = ndrv->chip->page_size;
    const uint8_t* src = buf;

    cursor_init(&c, ndrv);
    while(len > 0) {
        int rc = cursor_read(&c);
        if(rc < 0)
            return rc;

        size_t count = MIN(len, pg_size);
        if(memcmp(ndrv->page_buf, src, count))
            return RBF_VERIFY;

        src += count;
        len -= count;
    }

    return RBF_SUCCESS;
}

static struct nand_drv* rbflash_open(void)
{
    struct nand_drv* ndrv = nand_drv_init();
    nand_lock(ndrv);
    if(nand_drv_open(ndrv) != NAND_SUCCESS) {
        nand_unlock(ndrv);
        return NULL;
    }

    return ndrv;
}

static void rbflash_close(struct nand_drv* ndrv)
{
    nand_drv_close(ndrv);
    nand_unlock(ndrv);
}

/* Read the header and the scramble checksum that follows it */
static int read_flash_header(struct nand_drv* ndrv, struct rbflash_hdr* hdr,
                             uint32_t* chksum)
{
    uint8_t buf[sizeof(*hdr) + SCRAMBLE_HDR_SIZE];
    int rc = rbflash_read(ndrv, buf, sizeof(buf));
    if(rc < 0)
        return rc;

    memcpy(hdr, buf, sizeof(*hdr));
    if(memcmp(hdr->magic, rbflash_magic, sizeof(rbflash_magic)) ||
       hdr->length <= SCRAMBLE_HDR_SIZE ||
       hdr->length > ROCKBOX_FLASH_LENGTH - sizeof(*hdr))
        return RBF_NO_IMAGE;

    *chksum = load_be32(&buf[sizeof(*hdr)]);
    return RBF_SUCCESS;
}

int rbflash_load(void* buffer, size_t bufsize)
{
    struct nand_drv* ndrv = rbflash_open();
    if(!ndrv)
        return RBF_NAND_OPEN;

    struct rbflash_hdr hdr;
    uint32_t chksum;
    int rc = read_flash_header(ndrv, &hdr, &chksum);
    if(rc == RBF_SUCCESS && sizeof(hdr) + hdr.length > bufsize)
        rc = RBF_TOO_BIG;
    if(rc == RBF_SUCCESS)
        rc = rbflash_read(ndrv, buffer, sizeof(hdr) + hdr.length);
    rbflash_close(ndrv);
    if(rc < 0)
        return rc;

    /* strip the flash and scramble headers */
    uint8_t* buf = buffer;
    int len = hdr.length - SCRAMBLE_HDR_SIZE;
    memmove(buf, buf + sizeof(hdr) + SCRAMBLE_HDR_SIZE, len);
    if(!verify_checksum(chksum, buf, len))
        return RBF_BAD_CHKSUM;

    return len;
}

int rbflash_compare_file(const char* filename)
{
    int fd = open(filename, O_RDONLY);
    if(fd < 0)
        return RBF_FILE_IO;

    uint8_t fhdr[SCRAMBLE_HDR_SIZE];
    off_t fsize = ffilesize(fd);
    ssize_t cnt = read(fd, fhdr, sizeof(fhdr));
    close(fd);
    if(cnt != (ssize_t)sizeof(fhdr))
        return RBF_FILE_IO;

    struct nand_drv* ndrv = rbflash_open();
    if(!ndrv)
        return RBF_NAND_OPEN;

    struct rbflash_hdr hdr;
    uint32_t chksum;
    int rc = read_flash_header(ndrv, &hdr, &chksum);
    rbflash_close(ndrv);

    return !(rc == RBF_SUCCESS && hdr.length == (uint32_t)fsize &&
             chksum == load_be32(fhdr));
}

/* Write a rockbox.<ext> file from disk to flash */
/* Check the file in buf (after room for the flash header) and write both */
static int write_image(uint8_t* buf, size_t fsize)
{
    struct rbflash_hdr* hdr = (struct rbflash_hdr*)buf;
    uint8_t* file = buf + sizeof(*hdr);
    size_t total = sizeof(*hdr) + fsize;

    /* never put a broken image in flash */
    if(!verify_checksum(load_be32(file), file + SCRAMBLE_HDR_SIZE,
                        fsize - SCRAMBLE_HDR_SIZE))
        return RBF_BAD_CHKSUM;

    memcpy(hdr->magic, rbflash_magic, sizeof(rbflash_magic));
    hdr->length = fsize;
    hdr->reserved = 0;

    struct nand_drv* ndrv = rbflash_open();
    if(!ndrv)
        return RBF_NAND_OPEN;

    int rc = rbflash_write(ndrv, buf, total);
    if(rc == RBF_SUCCESS)
        rc = rbflash_verify(ndrv, buf, total);

    rbflash_close(ndrv);
    return rc;
}

/* Room for the header and a file of fsize bytes, or a negative error */
static int alloc_image(size_t fsize)
{
    if(fsize <= SCRAMBLE_HDR_SIZE)
        return RBF_FILE_IO;
    if(fsize > ROCKBOX_FLASH_LENGTH - sizeof(struct rbflash_hdr))
        return RBF_TOO_BIG;

    int handle = core_alloc_ex(sizeof(struct rbflash_hdr) + fsize,
                               &buflib_ops_locked);
    return handle < 0 ? RBF_NO_MEMORY : handle;
}

int rbflash_write_file(const char* filename)
{
    int fd = open(filename, O_RDONLY);
    if(fd < 0)
        return RBF_FILE_IO;

    off_t fsize = ffilesize(fd);
    int handle = alloc_image(fsize);
    if(handle < 0) {
        close(fd);
        return handle;
    }

    uint8_t* buf = core_get_data(handle);
    int rc = RBF_FILE_IO;
    if(read(fd, buf + sizeof(struct rbflash_hdr), fsize) == fsize)
        rc = write_image(buf, fsize);

    core_free(handle);
    close(fd);
    return rc;
}

int rbflash_write_buffer(const void* file, size_t size)
{
    int handle = alloc_image(size);
    if(handle < 0)
        return handle;

    uint8_t* buf = core_get_data(handle);
    memcpy(buf + sizeof(struct rbflash_hdr), file, size);
    int rc = write_image(buf, size);

    core_free(handle);
    return rc;
}
