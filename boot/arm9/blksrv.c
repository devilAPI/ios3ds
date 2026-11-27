/*
 * Block server: serves XNU's md0 (memdev -> kernel shim -> mailbox) from a
 * file on the SD card. Polled from the monitor loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "blksrv.h"
#include "blkcache.h"
#include "hfsfix.h"
#include "log.h"
#include "util.h"
#include "fatfs/ff.h"

static bool cache_flush(bool sync);
static void dirty_volume_repair(void);

static FIL f;
static DWORD clmt[32768];   /* 128 KiB: a 1 GiB image copied onto a busy card can have ~15k fragments */
static bool open_ok;
static u32 served, errors, sectors_moved;

/* dpkg writes a few sectors at a time and fsyncs after almost every file.
   Keeping the last 256 KiB of written sectors here (and syncing only when the
   disk goes idle or iOS asks for a clean unmount) removes most of those SD
   transactions. 512 slots x 512 B; the ARM9 has ~600 KiB free BSS. */
#define CACHE_SLOTS 512u
#define CACHE_DIRECT_SECTORS 16u      /* bigger writes go straight to the file */
static u8 cache_data[CACHE_SLOTS * BLKC_SECTOR] __attribute__((aligned(4)));
static u32 cache_lba[CACHE_SLOTS];
static u32 cache_map[CACHE_SLOTS * 2];
static u8 cache_dirty[CACHE_SLOTS];
static blkcache_t cache;
static u32 cache_sync_fail;
static bool pending_sync;             /* data written through FatFs, f_sync still due */
static u32 last_write_tick, last_flush_tick, cur_tick, cache_idle_fail;
/* ARM9 timer 2+3 in boot/arm9/main.c runs at 65457 Hz (T9_HZ). */
#define T9_CACHE_IDLE 32728u          /* 500 ms after the last write */
#define T9_CACHE_MAX  (2u * 65457u)   /* 2 s hard cap during a write stream */

/* HFS+ volume header at byte 1024: 'H+' or 'HX', version, then the 32-bit
   big-endian attributes. Bit 8 (kHFSVolumeUnmountedMask, 0x0100) is set by a
   clean unmount and cleared at mount. Our image is not journaled and Apple's
   launchctl fsck cannot open /dev/md0 while it is mounted (EBUSY, re-log 86),
   so this bit is the only durable record of the last shutdown. Read it when
   the image is opened (before XNU mounts md0) and set it again when iOS asks
   us to halt: PEHaltRestart runs after the file systems were unmounted. */
#define HFS_VH_OFF      1024u
#define HFS_VH_ATTR     4u
#define HFS_UNMOUNTED   0x0100u
static bool vh_ok;
static u32 vh_attrs;

static void vh_read(void)
{
    u8 b[8];
    UINT n = 0;
    FRESULT r = f_lseek(&f, HFS_VH_OFF);
    if (r == FR_OK) r = f_read(&f, b, sizeof b, &n);
    vh_ok = r == FR_OK && n == sizeof b && b[0] == 'H' && (b[1] == '+' || b[1] == 'X');
    if (vh_ok) vh_attrs = (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7];
}

bool blksrv_mark_clean(void)
{
    if (!open_ok) return false;
    /* Durability first: everything XNU wrote must be on the card before we
       set the clean flag, or a power cut would lose data in a "clean" image.
       This also flushes XNU's own write of the header (bit 8 cleared on
       mount), so the re-read below sees the real state. */
    if (!cache_flush(true)) {
        /* XNU may already have written the clean bit into its own buffer
           before we got here. Clear it on disk so a power cut cannot leave a
           "clean" image with lost write-back data. */
        u8 hdr[8];
        UINT n = 0;
        FRESULT r = f_lseek(&f, HFS_VH_OFF);
        if (r == FR_OK) r = f_read(&f, hdr, sizeof hdr, &n);
        if (r == FR_OK && n == sizeof hdr && hdr[0] == 'H' && (hdr[1] == '+' || hdr[1] == 'X')) {
            u32 dirty = ((u32)hdr[4] << 24 | (u32)hdr[5] << 16 | (u32)hdr[6] << 8 | hdr[7]) & ~HFS_UNMOUNTED;
            u8 b[4] = { (u8)(dirty >> 24), (u8)(dirty >> 16), (u8)(dirty >> 8), (u8)dirty };
            r = f_lseek(&f, HFS_VH_OFF + HFS_VH_ATTR);
            if (r == FR_OK) r = f_write(&f, b, sizeof b, &n);
            if (r == FR_OK) r = f_sync(&f);
        }
        log_err("[blk] rootfs.hfs: write-back flush failed: cannot mark cleanly unmounted%s\n",
                r == FR_OK ? " (clean bit cleared)" : " (could not clear the clean bit)");
        return false;
    }
    /* Re-read the volume header from disk: the cached copy taken at boot is
       stale by now (XNU clears bit 8 on mount and may set it again on a
       clean unmount), so a cached "already clean" answer would either skip a
       needed write or, worse, trust a dirty image. Only a live clean halt
       ever calls this, so setting the bit here is always correct. */
    u8 hdr[8];
    UINT n = 0;
    FRESULT r = f_lseek(&f, HFS_VH_OFF);
    if (r == FR_OK) r = f_read(&f, hdr, sizeof hdr, &n);
    vh_ok = r == FR_OK && n == sizeof hdr && hdr[0] == 'H' && (hdr[1] == '+' || hdr[1] == 'X');
    if (!vh_ok) {
        log_warn("rootfs: no HFS+ volume header at byte %u: cannot set the clean flag\n", HFS_VH_OFF);
        return false;
    }
    vh_attrs = (u32)hdr[4] << 24 | (u32)hdr[5] << 16 | (u32)hdr[6] << 8 | hdr[7];
    if (vh_attrs & HFS_UNMOUNTED) {
        log_ok("[blk] rootfs.hfs: already cleanly unmounted (attributes 0x%08x)\n", vh_attrs);
        return true;
    }
    u32 clean = vh_attrs | HFS_UNMOUNTED;
    u8 b[4] = { (u8)(clean >> 24), (u8)(clean >> 16), (u8)(clean >> 8), (u8)clean };
    r = f_lseek(&f, HFS_VH_OFF + HFS_VH_ATTR);
    n = 0;
    if (r == FR_OK) r = f_write(&f, b, sizeof b, &n);
    if (r == FR_OK && n != sizeof b) r = FR_DENIED;
    if (r == FR_OK) r = f_sync(&f);
    if (r == FR_OK) {
        vh_attrs = clean;
        log_ok("[blk] rootfs.hfs: marked cleanly unmounted after iOS shutdown (attributes 0x%08x)\n", vh_attrs);
    } else {
        log_err("[blk] rootfs.hfs: could not set the clean flag (FRESULT %d)\n", (int)r);
    }
    return r == FR_OK;
}

/* ---- write-back cache ---------------------------------------------------
   The callback writes one cached sector to the image. No f_sync here: many
   sectors are written in one flush, which is the point. f_sync follows once
   the disk goes idle (blksrv_tick) or before the clean-unmount flag is set. */
static int cache_writeback(u32 lba, const u8 *data, void *ctx)
{
    (void)ctx;
    FRESULT r = f_lseek(&f, (FSIZE_t)lba * BLKC_SECTOR);
    UINT n = 0;
    if (r == FR_OK) r = f_write(&f, data, BLKC_SECTOR, &n);
    if (r == FR_OK && n != BLKC_SECTOR) r = FR_DENIED;
    return (int)r;
}

static bool cache_flush(bool sync)
{
    if (blkc_dirty(&cache)) {
        int r = blkc_flush(&cache, cache_writeback, NULL);
        if (r) {
            cache_sync_fail++;
            return false;
        }
    }
    if (sync || pending_sync) {
        FRESULT r = f_sync(&f);
        if (r != FR_OK) {
            cache_sync_fail++;
            return false;
        }
        pending_sync = false;
    }
    return true;
}

/* Called by the monitor loop even while no md0 request is outstanding.
   A short idle period ends a write burst; a 2 s cap bounds how long a stream
   of writes can stay only in RAM (and in FatFs' own sector buffer). */
void blksrv_tick(u32 ticks)
{
    cur_tick = ticks;
    if (!open_ok) return;
    if (!pending_sync && !blkc_dirty(&cache)) return;
    bool idle = (u32)(ticks - last_write_tick) >= T9_CACHE_IDLE;
    bool old = (u32)(ticks - last_flush_tick) >= T9_CACHE_MAX;
    if (idle || old) {
        if (cache_flush(false)) last_flush_tick = ticks;
        else if (idle) cache_idle_fail++;
    }
}

/* md0 profile (boot.cfg ioprof=1/2, docs/boot-perf.md): 1 = one [io] line per
   second with md0 activity, 2 = also one [blk] line per request */
static u32 prof_level, prof_hz;
static u32 (*prof_now)(void);
static struct io_acc {
    u32 chunks, bufs, rd_sec, wr_sec, meta_sec, page_sec, seq, svc;
} acc, tot;
static u32 prof_t0, last_end = ~0u;

void blksrv_profile(u32 level, u32 (*now)(void), u32 hz)
{
    prof_level = level;
    prof_now = now;
    prof_hz = hz;
    if (level) prof_t0 = now();
}

static u32 ms(u32 ticks) { return (u32)((u64)ticks * 1000 / prof_hz); }

/* call from the monitor loop: the per-second summary */
void blksrv_prof_tick(void)
{
    if (!prof_level) return;
    u32 t = prof_now();
    if (t - prof_t0 < prof_hz) return;
    if (acc.chunks) {
        u32 tm = ms(t);
        logp("[io] t=%u.%03u %u bufs %u chunks | rd %u KiB wr %u KiB (meta %u, pagein %u) seq %u%% | "
             "arm9 %u ms | total %u bufs %u KiB %u ms | cache %u hit %u evict %u flushed %u direct "
             "%u fail\n",
             tm / 1000, tm % 1000, acc.bufs, acc.chunks, acc.rd_sec / 2, acc.wr_sec / 2, acc.meta_sec / 2,
             acc.page_sec / 2, acc.seq * 100 / acc.chunks, ms(acc.svc), tot.bufs, (tot.rd_sec + tot.wr_sec) / 2,
             ms(tot.svc), cache.hits, cache.evictions, cache.written, cache.direct_writes,
             cache_sync_fail + cache_idle_fail);
    }
    memset(&acc, 0, sizeof(acc));
    prof_t0 = t;
}

static void prof_one(u32 op, u32 lba, u32 cnt, u32 xbytes, u32 xflags, u32 t0, u32 t1)
{
    struct io_acc *a[2] = { &acc, &tot };
    for (int i = 0; i < 2; i++) {
        a[i]->chunks++;
        if (xflags & 0x80000000u) a[i]->bufs++;
        if (op == BLK_READ) a[i]->rd_sec += cnt;
        else a[i]->wr_sec += cnt;
        if (xflags & 0x100) a[i]->meta_sec += cnt;
        if (xflags & 0x80) a[i]->page_sec += cnt;
        if (lba == last_end) a[i]->seq++;
        a[i]->svc += t1 - t0;
    }
    last_end = lba + cnt;
    if (prof_level >= 2) {
        u32 tm = ms(t0);
        logp("[blk] %u.%03u %c %u+%u buf %u f%x %u us\n", tm / 1000, tm % 1000, op == BLK_READ ? 'r' : 'w',
             lba, cnt, xbytes, xflags, (u32)((u64)(t1 - t0) * 1000000 / prof_hz));
    }
}

#define MBOX ((volatile blkmbox_t *)(SHIM_PA + SHIM_BLK_OFF))
#define BOUNCE ((u8 *)(SHIM_PA + SHIM_BOUNCE_OFF))

int blksrv_open(const char *path, u32 *sectors)
{
    volatile blkmbox_t *m = MBOX;
    memset((void *)m, 0, sizeof(*m));
    FRESULT r = f_open(&f, path, FA_READ | FA_WRITE);
    if (r != FR_OK) return -(int)r;
    vh_ok = false;
    vh_attrs = 0;
    vh_read();
    if (!vh_ok)
        log_warn("rootfs %s: no HFS+ volume header at byte %u: cannot check the clean-unmount flag\n",
                 path, HFS_VH_OFF);
    else if (!(vh_attrs & HFS_UNMOUNTED))
        log_warn("[blk] rootfs.hfs: NOT cleanly unmounted (attributes 0x%08x, bit 8 clear): the last iOS run "
                 "was cut short. Apple's fsck cannot check md0 while it is mounted (EBUSY); the allocation "
                 "bitmap and directory counts are checked below before iOS mounts it\n", vh_attrs);
    else
        log_ok("[blk] rootfs.hfs: cleanly unmounted (attributes 0x%08x)\n", vh_attrs);
    blkc_init(&cache, cache_data, cache_lba, cache_map, cache_dirty, CACHE_SLOTS);
    pending_sync = false;
    last_write_tick = 0;
    last_flush_tick = 0;
    f.cltbl = clmt;
    clmt[0] = ARRAY_SIZE(clmt);
    r = f_lseek(&f, CREATE_LINKMAP);
    if (r != FR_OK) {
        log_warn("blksrv: fast-seek map needs %u entries (have %u): seeks will be slow\n",
                 (unsigned)clmt[0], (unsigned)ARRAY_SIZE(clmt));
        f.cltbl = NULL;
    }
    if (vh_ok && !(vh_attrs & HFS_UNMOUNTED)) dirty_volume_repair();
    *sectors = f_size(&f) / 512;
    m->dev_sectors = *sectors;
    m->magic = BLK_MAGIC;
    open_ok = true;
    return 0;
}

static int fix_rd(void *ctx, u32 lba, u32 cnt, void *buf)
{
    UINT n = 0;
    (void)ctx;
    if (f_lseek(&f, (FSIZE_t)lba * BLKC_SECTOR) != FR_OK) return 1;
    return f_read(&f, buf, cnt * BLKC_SECTOR, &n) != FR_OK || n != cnt * BLKC_SECTOR;
}

static int fix_wr(void *ctx, u32 lba, u32 cnt, const void *buf)
{
    UINT n = 0;
    (void)ctx;
    if (f_lseek(&f, (FSIZE_t)lba * BLKC_SECTOR) != FR_OK) return 1;
    return f_write(&f, buf, cnt * BLKC_SECTOR, &n) != FR_OK || n != cnt * BLKC_SECTOR;
}

/* Dirty volume: re-mark every block the catalog owns as allocated before XNU
   mounts it (boot/arm9/hfsfix.h, re-log launchdhang). cache_data is idle until
   blkc_init and serves as scratch. */
static void dirty_volume_repair(void)
{
    hfsfix_io_t io = { fix_rd, fix_wr, NULL };
    hfsfix_result_t r;
    int rc = hfsfix_bitmap(&io, cache_data, sizeof cache_data, &r);
    if (rc == HFSFIX_OK && f_sync(&f) != FR_OK) rc = HFSFIX_IO;
    if (rc != HFSFIX_OK)
        log_warn("[blk] rootfs.hfs: allocation bitmap check failed (%d): mounting as-is\n", rc);
    else if (r.fixed_blocks || r.valences_fixed)
        log_warn("[blk] rootfs.hfs: REPAIRED the dirty volume: %u block(s) in use by files were marked free "
                 "(free blocks %u -> %u, %u bitmap sector(s) rewritten), %u directory entry count(s) corrected "
                 "(%u files)\n",
                 r.fixed_blocks, r.free_before, r.free_after, r.bitmap_sectors_written, r.valences_fixed, r.files);
    else
        log_ok("[blk] rootfs.hfs: allocation bitmap and directory counts consistent with the catalog "
               "(%u files, %u free blocks)\n", r.files, r.free_after);
    if (rc == HFSFIX_OK && r.valence_skipped)
        log_warn("[blk] rootfs.hfs: directory entry counts not checked (too many catalog IDs for the scratch table)\n");
    if (rc == HFSFIX_OK && r.bad_extents)
        log_warn("[blk] rootfs.hfs: %u file extent(s) point outside the volume (not repaired)\n", r.bad_extents);
}

/* returns true if a request was served */
bool blksrv_poll(void)
{
    volatile blkmbox_t *m = MBOX;
    if (!open_ok || m->seq == m->done_seq) return false;
    u32 seq = m->seq, op = m->op, lba = m->lba, cnt = m->count;
    u32 xbytes = m->xnu_bytes, xflags = m->xnu_flags;
    u32 t0 = prof_level ? prof_now() : 0;
    int st = 0;
    UINT bx = 0;
    if (cnt == 0 || cnt * 512 > SHIM_BOUNCE_SIZE || (u64)lba + cnt > m->dev_sectors) {
        st = 1000;
    } else if (op == BLK_READ && !blkc_dirty(&cache)) {
        /* No overlay at all: one f_read for the whole request. The dirty hash
           lookup per read sector would cost more ARM9 time than the
           write-back cache saves (docs/boot-perf.md 8). */
        FRESULT r = f_lseek(&f, (FSIZE_t)lba * BLKC_SECTOR);
        if (r == FR_OK) r = f_read(&f, BOUNCE, cnt * BLKC_SECTOR, &bx);
        if (r != FR_OK) st = (int)r;
        else if (bx != cnt * BLKC_SECTOR) st = 1001;
    } else if (op == BLK_READ) {
        /* A dirty sector is in the overlay: read the runs around it and copy
           the cached sector into the bounce buffer. */
        u32 done = 0;
        while (done < cnt && !st) {
            u32 run = done;
            while (run < cnt && !blkc_dirty_lba(&cache, lba + run)) run++;
            if (run > done) {
                FRESULT r = f_lseek(&f, (FSIZE_t)(lba + done) * BLKC_SECTOR);
                if (r == FR_OK) r = f_read(&f, BOUNCE + done * BLKC_SECTOR,
                                           (run - done) * BLKC_SECTOR, &bx);
                if (r != FR_OK) st = (int)r;
                else if (bx != (run - done) * BLKC_SECTOR) st = 1001;
                done = run;
            } else {
                if (!blkc_get(&cache, lba + done, BOUNCE + done * BLKC_SECTOR))
                    st = 1002;
                done++;
            }
        }
    } else if (cnt <= CACHE_DIRECT_SECTORS && lba >= 8) {
        /* Small writes (HFS+ catalog nodes, dpkg metadata, file tails) go
           into the write-back cache. Never cache the volume header at LBA 2:
           mark_clean() must read its real contents after iOS unmounts. */
        for (u32 i = 0; i < cnt && !st; i++)
            st = blkc_put(&cache, lba + i, BOUNCE + i * BLKC_SECTOR,
                          cache_writeback, NULL);
        if (!st) {
            pending_sync = true;
            last_write_tick = cur_tick;
        }
    } else {
        /* Large/streaming writes: write straight to the file, but postpone
           f_sync until the disk goes idle. FatFs keeps one sector in its own
           buffer, so random writes still get merged by its cache. */
        blkc_invalidate_range(&cache, lba, cnt);
        FRESULT r = f_lseek(&f, (FSIZE_t)lba * BLKC_SECTOR);
        if (r == FR_OK) r = f_write(&f, BOUNCE, cnt * BLKC_SECTOR, &bx);
        st = r != FR_OK ? (int)r : (bx != cnt * BLKC_SECTOR ? 1001 : 0);
        if (!st) {
            pending_sync = true;
            cache.direct_writes += cnt;
            last_write_tick = cur_tick;
        }
    }
    if (st) {
        errors++;
        log_err("[blk] %s lba %u x%u failed (%d)\n", op == BLK_READ ? "read" : "write", lba, cnt, st);
    }
    served++;
    if (!st) sectors_moved += cnt;
    u32 t1 = prof_level ? prof_now() : 0;
    m->status = st;
    m->done_seq = seq;
    if (prof_level && !st) prof_one(op, lba, cnt, xbytes, xflags, t0, t1);  /* after: the ARM11 goes on */
    return true;
}

u32 blksrv_sectors(void) { return sectors_moved; }

void blksrv_stats(u32 *nserved, u32 *nerr)
{
    *nserved = served;
    *nerr = errors;
}
