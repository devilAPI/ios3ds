/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "util.h"

/* Boot-time repair of the HFS+ allocation bitmap of rootfs.hfs, run by the
   ARM9 before XNU mounts md0 when the volume was not cleanly unmounted.

   Why: the image is not journaled. A panic, power cut or crash while iOS has
   written file data leaves the allocation bitmap on disk older than the
   catalog (the bitmap is cached by XNU and written late). Blocks that files
   own then read as free, XNU hands them to the next new file, and that file
   overwrites live data. In the 2026-11-27 hardware failure launchd's
   bootstrap launchctl could not reach launchd ("launch_msg(): Socket is not
   connected") and nothing was ever exec'd (docs/re-log.md, launchdhang).
   Apple's fsck cannot run on a mounted md0 (EBUSY), so we do the one part of
   it that matters here ourselves: every block that any fork of the volume
   uses (the special files, the extents overflow tree, every file record of
   the catalog) is marked allocated. Blocks that are marked but unused are
   left alone (a harmless leak), so the repair can only turn free bits into
   used bits. The volume header's free block count (and its backup at the end
   of the volume) is recomputed.

   No FatFs/ARM9 dependencies: tests/test_hfsfix.c compiles this on host. */

typedef struct {
    int (*rd)(void *ctx, u32 lba, u32 cnt, void *buf);        /* 0 = ok */
    int (*wr)(void *ctx, u32 lba, u32 cnt, const void *buf);  /* 0 = ok */
    void *ctx;
} hfsfix_io_t;

typedef struct {
    u32 total_blocks, block_size;
    u32 files;            /* file records walked */
    u32 extents;          /* extent descriptors marked */
    u32 fixed_blocks;     /* bits turned from free to used */
    u32 bad_extents;      /* extents outside the volume, ignored */
    u32 free_before, free_after;   /* free block count in the volume header, then recounted */
    u32 bitmap_sectors_written;
    u32 valences_fixed;   /* folder records whose valence (entry count) was wrong */
    u32 valence_skipped;  /* 1: not enough scratch memory for the valence table */
} hfsfix_result_t;

enum {
    HFSFIX_OK = 0,
    HFSFIX_NOT_HFS = -1,
    HFSFIX_JOURNALED = -2,      /* left to the journal replay of the kernel */
    HFSFIX_UNSUPPORTED = -3,    /* node/bitmap size, bitmap or tree fork with overflow extents */
    HFSFIX_NOMEM = -4,
    HFSFIX_IO = -5,
    HFSFIX_BADTREE = -6,
};

/* Needs scratch >= bitmap bytes (total_blocks / 8, rounded up to 512) + 16 KiB. */
int hfsfix_bitmap(const hfsfix_io_t *io, u8 *scratch, u32 scratch_len, hfsfix_result_t *res);
