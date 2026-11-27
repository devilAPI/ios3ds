/* Host driver for boot/arm9/hfsfix.c: repair an HFS+ image file in place.
     hfsfix_tool image.hfs
   Used by tests/test_hfsfix.py; also usable on a copy of a card's rootfs.hfs.
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hfsfix.h"

static int rd(void *ctx, u32 lba, u32 cnt, void *buf)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)lba * 512, SEEK_SET)) return 1;
    return fread(buf, 512, cnt, f) == cnt ? 0 : 1;
}

static int wr(void *ctx, u32 lba, u32 cnt, const void *buf)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)lba * 512, SEEK_SET)) return 1;
    return fwrite(buf, 512, cnt, f) == cnt ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s image.hfs\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r+b");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    static u8 scratch[1 << 20];
    hfsfix_io_t io = { rd, wr, f };
    hfsfix_result_t r;
    int rc = hfsfix_bitmap(&io, scratch, sizeof scratch, &r);
    fclose(f);
    printf("rc=%d blocks=%u files=%u extents=%u fixed=%u bad=%u free %u -> %u sectors_written=%u valences_fixed=%u skipped=%u\n", rc,
           r.total_blocks, r.files, r.extents, r.fixed_blocks, r.bad_extents, r.free_before, r.free_after,
           r.bitmap_sectors_written, r.valences_fixed, r.valence_skipped);
    return rc ? 1 : 0;
}
