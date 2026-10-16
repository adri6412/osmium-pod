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
 * Internal storage on the SPI NAND, as a Rockbox storage driver.
 *
 * The NAND from NAND_FTL_START to the end of the chip (minus a few blocks)
 * is managed by Dhara (dhara/, by Daniel Beer), which gives wear leveling,
 * bad block handling and a map that survives power loss. Dhara sectors are
 * NAND pages (2 KiB); Rockbox sees 512-byte sectors, and partial pages are
 * read, patched and written back.
 *
 * Writes become durable when the map is synced: on storage_flush(), and
 * after a second without disk activity.
 */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "storage.h"
#include "nand-ingenic.h"
#include "nand-ftl-ingenic.h"
#include "dhara/map.h"
#include "logf.h"
#include <string.h>

/* Garbage collection ratio: Dhara keeps 1/(GC_RATIO+1) of the space free
 * so that writes stay fast when the drive is full. Never change it for a
 * formatted drive. */
#define GC_RATIO        4

/* Bad blocks allowed for when sizing the drive: the size must not change
 * as blocks wear out, so it assumes this many from the start. */
#define BAD_ALLOWANCE   24

/* Blocks at the end of the chip left alone (Linux keeps its bad block
 * tables there on some chips) */
#define RESERVED_END    4

#define SECTOR_SHIFT    9

struct ftl_nand {
    struct dhara_nand dn;       /* must be first: Dhara passes it back */
    struct nand_drv* drv;
    nand_page_t first_page;     /* page of block 0 of the FTL area */
};

static struct ftl_nand fnand;
static struct dhara_map map;
static uint8_t map_buf[NAND_DRV_MAXPAGESIZE];
static uint8_t rmw_buf[NAND_DRV_MAXPAGESIZE];
static uint32_t bad_map[(1024 + 31) / 32];

static bool ftl_present;
static bool ftl_dirty;
static int ftl_drive = -1;
static unsigned spp_shift;      /* log2 of 512-byte sectors per page */
static sector_t num_sectors;
static long last_disk_activity = -1;

/* ---------------------------------------------------------------- Dhara glue */

static inline struct nand_drv* drv_of(const struct dhara_nand* n)
{
    return ((const struct ftl_nand*)n)->drv;
}

static inline nand_page_t phys_page(const struct dhara_nand* n, dhara_page_t p)
{
    return ((const struct ftl_nand*)n)->first_page + p;
}

static inline bool bad_get(dhara_block_t b)
{
    return bad_map[b / 32] & (1u << (b % 32));
}

static inline void bad_set(dhara_block_t b)
{
    bad_map[b / 32] |= 1u << (b % 32);
}

int dhara_nand_is_bad(const struct dhara_nand* n, dhara_block_t b)
{
    (void)n;
    return bad_get(b);
}

void dhara_nand_mark_bad(const struct dhara_nand* n, dhara_block_t b)
{
    struct nand_drv* drv = drv_of(n);
    nand_page_t page = phys_page(n, b << n->log2_ppb);

    logf("ftl: block %lu goes bad", (unsigned long)b);
    bad_set(b);

    /* write the marker where the bootloader and Linux look for it */
    nand_block_erase(drv, page);
    memset(drv->page_buf, 0xff, drv->fpage_size);
    drv->page_buf[drv->chip->bbm_pos] = 0;
    nand_page_program(drv, page, drv->page_buf);
}

int dhara_nand_erase(const struct dhara_nand* n, dhara_block_t b,
                     dhara_error_t* err)
{
    if(nand_block_erase(drv_of(n), phys_page(n, b << n->log2_ppb)) < 0) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return 0;
}

int dhara_nand_prog(const struct dhara_nand* n, dhara_page_t p,
                    const uint8_t* data, dhara_error_t* err)
{
    struct nand_drv* drv = drv_of(n);
    unsigned pg_size = drv->chip->page_size;

    memcpy(drv->page_buf, data, pg_size);
    memset(&drv->page_buf[pg_size], 0xff, drv->chip->oob_size);
    if(nand_page_program(drv, phys_page(n, p), drv->page_buf) < 0) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return 0;
}

int dhara_nand_is_free(const struct dhara_nand* n, dhara_page_t p)
{
    struct nand_drv* drv = drv_of(n);
    if(nand_page_read(drv, phys_page(n, p), drv->page_buf) < 0)
        return 0;

    /* the spare area too: programmed pages carry ECC bytes there */
    for(unsigned i = 0; i < drv->fpage_size; ++i)
        if(drv->page_buf[i] != 0xff)
            return 0;

    return 1;
}

int dhara_nand_read(const struct dhara_nand* n, dhara_page_t p,
                    size_t offset, size_t length, uint8_t* data,
                    dhara_error_t* err)
{
    struct nand_drv* drv = drv_of(n);
    int rc = nand_page_read(drv, phys_page(n, p), drv->page_buf);
    if(rc < 0) {
        dhara_set_error(err, rc == NAND_ERR_ECC_FAIL ? DHARA_E_ECC
                                                     : DHARA_E_BAD_BLOCK);
        return -1;
    }

    memcpy(data, &drv->page_buf[offset], length);
    return 0;
}

int dhara_nand_copy(const struct dhara_nand* n, dhara_page_t src,
                    dhara_page_t dst, dhara_error_t* err)
{
    struct nand_drv* drv = drv_of(n);
    int rc = nand_page_read(drv, phys_page(n, src), drv->page_buf);
    if(rc < 0) {
        dhara_set_error(err, rc == NAND_ERR_ECC_FAIL ? DHARA_E_ECC
                                                     : DHARA_E_BAD_BLOCK);
        return -1;
    }

    memset(&drv->page_buf[drv->chip->page_size], 0xff, drv->chip->oob_size);
    if(nand_page_program(drv, phys_page(n, dst), drv->page_buf) < 0) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------- setup */

static void scan_bad_blocks(void)
{
    struct nand_drv* drv = fnand.drv;

    memset(bad_map, 0, sizeof(bad_map));
    for(dhara_block_t b = 0; b < fnand.dn.num_blocks; ++b) {
        /* only the marker counts: an ECC error on a data page is
         * Dhara's business, not a reason to retire the block */
        nand_page_read(drv, phys_page(&fnand.dn, b << fnand.dn.log2_ppb),
                       drv->page_buf);
        if(drv->page_buf[drv->chip->bbm_pos] != 0xff)
            bad_set(b);
    }
}

/* Size of the drive in pages: what Dhara offers with BAD_ALLOWANCE bad
 * blocks (cf. dhara_journal_capacity() and dhara_map_capacity()) */
static dhara_page_t fixed_capacity(void)
{
    const struct dhara_nand* n = &fnand.dn;
    const int log2_ppc = map.journal.log2_ppc;

    if(n->num_blocks <= BAD_ALLOWANCE + 1)
        return 0;

    dhara_page_t good_cps = (dhara_page_t)(n->num_blocks - BAD_ALLOWANCE - 1)
                            << (n->log2_ppb - log2_ppc);
    dhara_page_t cap = (good_cps << log2_ppc) - good_cps;
    dhara_page_t reserve = cap / (GC_RATIO + 1);
    dhara_page_t margin = DHARA_MAX_RETRIES << n->log2_ppb;

    return reserve + margin < cap ? cap - reserve - margin : 0;
}

int nand_init(void)
{
    struct nand_drv* drv = nand_drv_init();

    nand_lock(drv);
    if(nand_drv_open(drv) != NAND_SUCCESS) {
        /* no internal storage, but the SD card still works */
        nand_unlock(drv);
        logf("ftl: no NAND");
        return 0;
    }

    const struct nand_chip* chip = drv->chip;
    nand_block_t blk_size = chip->page_size << chip->log2_ppb;
    nand_block_t first_block = NAND_FTL_START / blk_size;
    if(first_block + RESERVED_END >= chip->nr_blocks ||
       chip->nr_blocks - first_block - RESERVED_END > sizeof(bad_map) * 8 ||
       chip->page_size > NAND_DRV_MAXPAGESIZE) {
        nand_drv_close(drv);
        nand_unlock(drv);
        return 0;
    }

    fnand.drv = drv;
    fnand.first_page = first_block << chip->log2_ppb;
    fnand.dn.log2_page_size = find_first_set_bit(chip->page_size);
    fnand.dn.log2_ppb = chip->log2_ppb;
    fnand.dn.num_blocks = chip->nr_blocks - first_block - RESERVED_END;
    spp_shift = fnand.dn.log2_page_size - SECTOR_SHIFT;

    scan_bad_blocks();

    dhara_error_t err;
    dhara_map_init(&map, &fnand.dn, map_buf, GC_RATIO);
    if(dhara_map_resume(&map, &err) < 0)
        logf("ftl: no map (%s), empty drive", dhara_strerror(err));

    num_sectors = (sector_t)fixed_capacity() << spp_shift;
    ftl_present = num_sectors > 0;
    nand_unlock(drv);
    return 0;
}

/* ---------------------------------------------------------------- I/O */

int nand_read_sectors(IF_MD(int drive,) sector_t start, int count, void* buf)
{
    IF_MD((void)drive);
    if(!ftl_present || count < 0 || start + count > num_sectors)
        return -1;

    const unsigned spp = 1u << spp_shift;
    uint8_t* dst = buf;
    int rc = 0;

    nand_lock(fnand.drv);
    while(count > 0) {
        dhara_sector_t ds = start >> spp_shift;
        unsigned first = start & (spp - 1);
        unsigned n = MIN((unsigned)count, spp - first);
        dhara_error_t err;

        if(first == 0 && n == spp) {
            if(dhara_map_read(&map, ds, dst, &err) < 0)
                rc = -2;
        } else {
            if(dhara_map_read(&map, ds, rmw_buf, &err) < 0)
                rc = -2;
            else
                memcpy(dst, &rmw_buf[first << SECTOR_SHIFT], n << SECTOR_SHIFT);
        }

        if(rc < 0) {
            logf("ftl: read %lu: %s", (unsigned long)ds, dhara_strerror(err));
            break;
        }

        dst += n << SECTOR_SHIFT;
        start += n;
        count -= n;
    }

    last_disk_activity = current_tick;
    nand_unlock(fnand.drv);
    return rc;
}

static int write_locked(sector_t start, int count, const void* buf)
{
    const unsigned spp = 1u << spp_shift;
    const uint8_t* src = buf;

    while(count > 0) {
        dhara_sector_t ds = start >> spp_shift;
        unsigned first = start & (spp - 1);
        unsigned n = MIN((unsigned)count, spp - first);
        dhara_error_t err;
        int rc;

        if(first == 0 && n == spp) {
            rc = dhara_map_write(&map, ds, src, &err);
        } else {
            rc = dhara_map_read(&map, ds, rmw_buf, &err);
            if(rc == 0) {
                memcpy(&rmw_buf[first << SECTOR_SHIFT], src, n << SECTOR_SHIFT);
                rc = dhara_map_write(&map, ds, rmw_buf, &err);
            }
        }

        ftl_dirty = true;
        if(rc < 0) {
            logf("ftl: write %lu: %s", (unsigned long)ds, dhara_strerror(err));
            return -2;
        }

        src += n << SECTOR_SHIFT;
        start += n;
        count -= n;
    }

    return 0;
}

int nand_write_sectors(IF_MD(int drive,) sector_t start, int count,
                       const void* buf)
{
    IF_MD((void)drive);
    if(!ftl_present || count < 0 || start + count > num_sectors)
        return -1;

    nand_lock(fnand.drv);
    int rc = write_locked(start, count, buf);
    last_disk_activity = current_tick;
    nand_unlock(fnand.drv);
    return rc;
}

static int sync_locked(void)
{
    if(!ftl_dirty)
        return 0;

    dhara_error_t err;
    if(dhara_map_sync(&map, &err) < 0) {
        logf("ftl: sync: %s", dhara_strerror(err));
        return -1;
    }

    ftl_dirty = false;
    return 0;
}

int nand_flush(void)
{
    if(!ftl_present)
        return 0;

    nand_lock(fnand.drv);
    int rc = sync_locked();
    nand_unlock(fnand.drv);
    return rc;
}

int nand_event(long id, intptr_t data)
{
    /* make writes durable once the disk has been idle for a second */
    if(id == Q_STORAGE_TICK && ftl_dirty &&
       TIME_AFTER(current_tick, last_disk_activity + HZ))
        nand_flush();

    return storage_event_default_handler(id, data, last_disk_activity,
                                         STORAGE_NAND);
}

/* ---------------------------------------------------------------- driver API */

void nand_enable(bool on)       { (void)on; }
void nand_sleepnow(void)        {}
void nand_spin(void)            {}
void nand_spindown(int seconds) { (void)seconds; }
int nand_spinup_time(void)      { return 0; }
long nand_last_disk_activity(void) { return last_disk_activity; }

#ifdef STORAGE_GET_INFO
void nand_get_info(IF_MD(int drive,) struct storage_info* info)
{
    IF_MD((void)drive);
    info->sector_size = 1 << SECTOR_SHIFT;
    info->num_sectors = num_sectors;
    info->vendor = "Rockbox";
    info->product = "Internal Storage";
    info->revision = "0.00";
}
#endif

#ifdef CONFIG_STORAGE_MULTI
int nand_num_drives(int first_drive)
{
    ftl_drive = first_drive;
    return 1;
}
#endif

/* ---------------------------------------------------------------- app API */

int nand_ftl_drive(void)
{
    return ftl_present ? ftl_drive : -1;
}

void nand_ftl_get_info(struct nand_ftl_info* info)
{
    memset(info, 0, sizeof(*info));
    info->present = ftl_present;
    if(!ftl_present)
        return;

    nand_lock(fnand.drv);
    info->sectors = num_sectors;
    for(dhara_block_t b = 0; b < fnand.dn.num_blocks; ++b)
        info->bad_blocks += bad_get(b);
    info->used_pages = dhara_map_size(&map);
    info->total_pages = num_sectors >> spp_shift;
    nand_unlock(fnand.drv);
}

/* FAT16 with 2 KiB clusters, laid out so that every cluster fills exactly
 * one NAND page, behind an MBR as a computer expects. */
#define PART_START      2048        /* sectors: 1 MiB, page aligned */
#define SEC_PER_CLUS    4
#define ROOT_ENTRIES    512
#define ROOT_SECTORS    (ROOT_ENTRIES * 32 / 512)

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static int write_fat16(void)
{
    static uint8_t sec[512];
    uint32_t part_size = num_sectors - PART_START;

    /* FAT size: an upper bound, counting every sector as data */
    uint32_t fat_size = (part_size / SEC_PER_CLUS + 2 + 255) / 256;
    uint32_t reserved = 1;
    while((reserved + 2 * fat_size + ROOT_SECTORS) % SEC_PER_CLUS)
        reserved++;

    uint32_t data_start = reserved + 2 * fat_size + ROOT_SECTORS;
    uint32_t clusters = (part_size - data_start) / SEC_PER_CLUS;
    if(clusters < 4085 || clusters >= 65525)
        return -3;

    /* MBR */
    memset(sec, 0, sizeof(sec));
    uint8_t* pe = &sec[446];
    pe[0] = 0x00;                   /* not bootable */
    pe[1] = 0xfe; pe[2] = 0xff; pe[3] = 0xff;   /* CHS: use LBA */
    pe[4] = 0x0e;                   /* FAT16, LBA */
    pe[5] = 0xfe; pe[6] = 0xff; pe[7] = 0xff;
    put32(&pe[8], PART_START);
    put32(&pe[12], part_size);
    sec[510] = 0x55;
    sec[511] = 0xaa;
    if(write_locked(0, 1, sec))
        return -4;

    /* boot sector */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xeb; sec[1] = 0x3c; sec[2] = 0x90;
    memcpy(&sec[3], "ROCKBOX ", 8);
    put16(&sec[11], 512);
    sec[13] = SEC_PER_CLUS;
    put16(&sec[14], reserved);
    sec[16] = 2;                    /* FATs */
    put16(&sec[17], ROOT_ENTRIES);
    if(part_size < 65536)
        put16(&sec[19], part_size);
    else
        put32(&sec[32], part_size);
    sec[21] = 0xf8;                 /* fixed disk */
    put16(&sec[22], fat_size);
    put16(&sec[24], 63);            /* sectors per track */
    put16(&sec[26], 255);           /* heads */
    put32(&sec[28], PART_START);    /* hidden sectors */
    sec[36] = 0x80;                 /* drive number */
    sec[38] = 0x29;                 /* extended boot signature */
    put32(&sec[39], current_tick ^ 0x4f534d31);  /* volume id */
    memcpy(&sec[43], "INTERNAL   ", 11);
    memcpy(&sec[54], "FAT16   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xaa;
    if(write_locked(PART_START, 1, sec))
        return -4;

    /* the rest of the reserved area, the FATs and the root directory */
    for(uint32_t s = 1; s < data_start; ++s) {
        memset(sec, 0, sizeof(sec));
        uint32_t fat_sec = s - reserved;
        if(s >= reserved && s < reserved + 2 * fat_size &&
           fat_sec % fat_size == 0) {
            put16(&sec[0], 0xfff8);     /* media */
            put16(&sec[2], 0xffff);     /* clean shutdown, no errors */
        } else if(s == reserved + 2 * fat_size) {
            memcpy(&sec[0], "INTERNAL   ", 11);
            sec[11] = 0x08;             /* volume label */
        }

        if(write_locked(PART_START + s, 1, sec))
            return -4;
    }

    return 0;
}

/* Erase the whole area: the map and every sector go */
static void erase_locked(void)
{
    struct nand_drv* drv = fnand.drv;

    /* the area may hold anything, such as the original firmware's UBI
     * volumes: start from erased blocks */
    for(dhara_block_t b = 0; b < fnand.dn.num_blocks; ++b) {
        if(bad_get(b))
            continue;
        if(nand_block_erase(drv, phys_page(&fnand.dn, b << fnand.dn.log2_ppb)) < 0)
            dhara_nand_mark_bad(&fnand.dn, b);
    }

    dhara_map_init(&map, &fnand.dn, map_buf, GC_RATIO);
    dhara_map_clear(&map);
    ftl_dirty = false;
    last_disk_activity = current_tick;
}

int nand_ftl_wipe(void)
{
    if(!ftl_present)
        return -1;

    nand_lock(fnand.drv);
    erase_locked();
    nand_unlock(fnand.drv);
    return 0;
}

int nand_ftl_format(void)
{
    if(!ftl_present)
        return -1;

    nand_lock(fnand.drv);
    erase_locked();
    ftl_dirty = true;

    int rc = write_fat16();
    if(rc == 0)
        rc = sync_locked();

    nand_unlock(fnand.drv);
    return rc;
}
