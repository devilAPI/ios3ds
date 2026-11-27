/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hfsfix.h"

#define SEC 512u
#define NODE_MAX 8192u
#define VH_SEC 2u                 /* the volume header is at byte 1024 */
#define MAX_CAT_EXT 64u

typedef struct { u32 start, count; } ext_t;

typedef struct {
    const hfsfix_io_t *io;
    u8 *bitmap;
    u8 *node;
    u32 bs, total, bm_bytes;
    hfsfix_result_t *res;
    ext_t cat[MAX_CAT_EXT + 8];
    u32 ncat;
    u16 *kids;           /* children per directory CNID (0xFFFF = unknown) */
    u32 nkids;
    int pass;            /* catalog pass: 1 = mark extents + count, 2 = fix valences */
} fix_t;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void mark(fix_t *x, u32 start, u32 count)
{
    if (!count) return;
    if (start >= x->total || count > x->total - start) {
        x->res->bad_extents++;
        return;
    }
    x->res->extents++;
    for (u32 b = start; b < start + count; b++) {
        u8 m = (u8)(0x80u >> (b & 7));
        if (!(x->bitmap[b >> 3] & m)) {
            x->bitmap[b >> 3] |= m;
            x->res->fixed_blocks++;
        }
    }
}

/* 8 extent descriptors */
static void mark8(fix_t *x, const u8 *e)
{
    for (int i = 0; i < 8; i++) mark(x, be32(e + 8 * i), be32(e + 8 * i + 4));
}

/* Read or write `len` bytes at byte `pos` of a fork described by ext[0..n). */
static int fork_io(fix_t *x, const ext_t *ext, u32 n, u64 pos, u8 *dst, u32 len, int write)
{
    while (len) {
        u64 blk = pos / x->bs;
        u32 off = (u32)(pos % x->bs), i = 0;
        u64 skip = 0;
        while (i < n && skip + ext[i].count <= blk) skip += ext[i++].count;
        if (i == n) return HFSFIX_BADTREE;
        u32 inext = (u32)(blk - skip);
        u64 bytes_left = (u64)(ext[i].count - inext) * x->bs - off;
        u32 chunk = bytes_left < len ? (u32)bytes_left : len;
        u64 byte = ((u64)ext[i].start + inext) * x->bs + off;
        /* node and block sizes are multiples of 512 */
        if ((byte % SEC) || (chunk % SEC)) return HFSFIX_UNSUPPORTED;
        if (write ? x->io->wr(x->io->ctx, (u32)(byte / SEC), chunk / SEC, dst)
                  : x->io->rd(x->io->ctx, (u32)(byte / SEC), chunk / SEC, dst))
            return HFSFIX_IO;
        dst += chunk;
        pos += chunk;
        len -= chunk;
    }
    return 0;
}

static int fork_read(fix_t *x, const ext_t *ext, u32 n, u64 pos, u8 *dst, u32 len)
{
    return fork_io(x, ext, n, pos, dst, len, 0);
}

/* Walk the leaf chain of a B-tree whose fork is ext[0..n): mark the extents of
   every record (catalog: the two forks of each file record; extents overflow:
   all eight descriptors). */
static int walk_leaves(fix_t *x, const ext_t *ext, u32 n, int is_cat)
{
    u8 *node = x->node;
    int r = fork_read(x, ext, n, 0, node, 512);   /* the header node starts with the node size */
    if (r) return r;
    u32 nsz = be16(node + 32), first = be32(node + 24), guard = 0;
    if (nsz < 512 || nsz > NODE_MAX || (nsz & 511)) return HFSFIX_UNSUPPORTED;
    for (u32 nd = first; nd && guard < 0x1000000u; guard++) {
        r = fork_read(x, ext, n, (u64)nd * nsz, node, nsz);
        if (r) return r;
        u32 flink = be32(node);
        int changed = 0;
        if (node[8] == 0xFF) {
            u32 nrec = be16(node + 10);
            for (u32 i = 0; i < nrec && 2 * (i + 1) < nsz; i++) {
                u32 o = be16(node + nsz - 2 * (i + 1));
                if (o < 14 || o + 2 > nsz) continue;
                u32 klen = be16(node + o), d = o + 2 + klen;
                d += d & 1;
                if (d + 2 > nsz) continue;
                if (is_cat) {
                    u32 rt = be16(node + d);
                    u32 parent = be32(node + o + 2);
                    if (rt == 2 && d + 168 + 80 <= nsz && x->pass == 1) {   /* file record */
                        x->res->files++;
                        mark8(x, node + d + 88 + 16);
                        mark8(x, node + d + 168 + 16);
                    }
                    if ((rt == 1 || rt == 2) && x->kids && x->pass == 1 && parent > 1 && parent < x->nkids &&
                        x->kids[parent] != 0xFFFF)
                        x->kids[parent]++;
                    if (rt == 1 && x->kids && x->pass == 2 && d + 16 <= nsz) {   /* folder record: valence */
                        u32 cnid = be32(node + d + 8);
                        if (cnid < x->nkids && x->kids[cnid] != 0xFFFF && be32(node + d + 4) != x->kids[cnid]) {
                            put32(node + d + 4, x->kids[cnid]);
                            x->res->valences_fixed++;
                            changed = 1;
                        }
                    }
                } else if (d + 64 <= nsz) {
                    mark8(x, node + d);
                    if (klen == 10 && node[o + 2] == 0 && be32(node + o + 4) == 4 && x->ncat < MAX_CAT_EXT) {
                        /* overflow extents of the catalog file (fileID 4, data fork): needed to walk it */
                        for (int k = 0; k < 8; k++) {
                            u32 c = be32(node + d + 8 * k + 4);
                            if (c) { x->cat[x->ncat].start = be32(node + d + 8 * k); x->cat[x->ncat].count = c; x->ncat++; }
                        }
                    }
                }
            }
        }
        if (changed && fork_io(x, ext, n, (u64)nd * nsz, node, nsz, 1)) return HFSFIX_IO;
        nd = flink;
    }
    return 0;
}

int hfsfix_bitmap(const hfsfix_io_t *io, u8 *scratch, u32 scratch_len, hfsfix_result_t *res)
{
    fix_t x;
    u8 vh[SEC];
    memset(res, 0, sizeof *res);
    memset(&x, 0, sizeof x);
    if (io->rd(io->ctx, VH_SEC, 1, vh)) return HFSFIX_IO;
    if (vh[0] != 'H' || (vh[1] != '+' && vh[1] != 'X')) return HFSFIX_NOT_HFS;
    if (be32(vh + 4) & 0x2000) return HFSFIX_JOURNALED;
    x.bs = be32(vh + 40);
    x.total = be32(vh + 44);
    res->block_size = x.bs;
    res->total_blocks = x.total;
    res->free_before = be32(vh + 48);
    if (x.bs < SEC || (x.bs & (x.bs - 1)) || !x.total) return HFSFIX_UNSUPPORTED;
    x.bm_bytes = ALIGN_UP((x.total + 7) / 8, SEC);
    if (scratch_len < x.bm_bytes + NODE_MAX + SEC) return HFSFIX_NOMEM;
    x.io = io;
    x.res = res;
    x.bitmap = scratch;
    x.node = scratch + x.bm_bytes;
    u8 *chk = x.node + NODE_MAX;

    /* the bitmap file: inline extents only */
    {
        ext_t ext[8];
        u32 n = 0;
        for (int i = 0; i < 8; i++) {
            u32 c = be32(vh + 112 + 16 + 8 * i + 4);
            if (c) { ext[n].start = be32(vh + 112 + 16 + 8 * i); ext[n].count = c; n++; }
        }
        u64 size = ((u64)be32(vh + 112 + 8) << 32) | be32(vh + 112 + 12);
        if (size < (x.total + 7) / 8) return HFSFIX_UNSUPPORTED;
        u64 have = 0;
        for (u32 i = 0; i < n; i++) have += ext[i].count;
        if (have * x.bs < x.bm_bytes || be32(vh + 112 + 12) == 0) return HFSFIX_UNSUPPORTED;
        /* read the whole (sector rounded) bitmap; the part past `size` is padding */
        int r = fork_read(&x, ext, n, 0, x.bitmap, x.bm_bytes);
        if (r) return r;
    }

    /* the boot blocks and volume header, and the backup header at the end */
    mark(&x, 0, (1536 + x.bs - 1) / x.bs);
    {
        u64 end = (u64)x.total * x.bs;
        u32 first = (u32)((end - 1024) / x.bs);
        mark(&x, first, x.total - first);
    }

    /* every special file's inline extents; the bitmap's own blocks too */
    static const u32 special[] = { 112, 192, 272, 352, 432 };
    for (u32 i = 0; i < ARRAY_SIZE(special); i++) mark8(&x, vh + special[i] + 16);

    /* the extents overflow tree: inline extents only (Apple never overflows it) */
    ext_t ext_tree[8];
    u32 next = 0;
    for (int i = 0; i < 8; i++) {
        u32 c = be32(vh + 192 + 16 + 8 * i + 4);
        if (c) { ext_tree[next].start = be32(vh + 192 + 16 + 8 * i); ext_tree[next].count = c; next++; }
    }
    if (next) {
        int r = walk_leaves(&x, ext_tree, next, 0);
        if (r) return r;
    }

    /* the catalog: inline extents, then the overflow records seen above */
    {
        ext_t cat[8 + MAX_CAT_EXT];
        u32 n = 0;
        for (int i = 0; i < 8; i++) {
            u32 c = be32(vh + 272 + 16 + 8 * i + 4);
            if (c) { cat[n].start = be32(vh + 272 + 16 + 8 * i); cat[n].count = c; n++; }
        }
        for (u32 i = 0; i < x.ncat && n < ARRAY_SIZE(cat); i++) cat[n++] = x.cat[i];
        u32 used = x.bm_bytes + NODE_MAX + SEC, nk = be32(vh + 64);
        if (nk > 1 && nk <= (scratch_len - used) / 2) {
            x.kids = (u16 *)(chk + SEC);
            x.nkids = nk;
            memset(x.kids, 0, nk * 2);
        } else {
            res->valence_skipped = 1;
        }
        x.pass = 1;
        int r = walk_leaves(&x, cat, n, 1);
        if (r) return r;
        if (x.kids) {
            x.pass = 2;
            r = walk_leaves(&x, cat, n, 1);
            if (r) return r;
        }
    }

    /* write back the bitmap sectors that changed */
    for (u32 s = 0; s < x.bm_bytes / SEC; s++) {
        ext_t ext[8];
        u32 n = 0;
        for (int i = 0; i < 8; i++) {
            u32 c = be32(vh + 112 + 16 + 8 * i + 4);
            if (c) { ext[n].start = be32(vh + 112 + 16 + 8 * i); ext[n].count = c; n++; }
        }
        if (fork_read(&x, ext, n, (u64)s * SEC, chk, SEC)) return HFSFIX_IO;
        if (!memcmp(chk, x.bitmap + (u64)s * SEC, SEC)) continue;
        /* fork_read gave us the lba logic; recompute it for the write */
        u64 blk = ((u64)s * SEC) / x.bs, skip = 0;
        u32 i = 0;
        while (i < n && skip + ext[i].count <= blk) skip += ext[i++].count;
        u64 byte = ((u64)ext[i].start + (blk - skip)) * x.bs + ((u64)s * SEC) % x.bs;
        if (io->wr(io->ctx, (u32)(byte / SEC), 1, x.bitmap + (u64)s * SEC)) return HFSFIX_IO;
        res->bitmap_sectors_written++;
    }

    /* recount the free blocks */
    u32 used = 0;
    for (u32 b = 0; b < x.total; b++)
        if (x.bitmap[b >> 3] & (0x80u >> (b & 7))) used++;
    res->free_after = x.total - used;
    if (res->free_after != res->free_before) {
        put32(vh + 48, res->free_after);
        if (io->wr(io->ctx, VH_SEC, 1, vh)) return HFSFIX_IO;
        /* the backup volume header: 1024 bytes before the end of the volume */
        u64 alt = (u64)x.total * x.bs - 1024;
        u8 av[SEC];
        if (alt / SEC > VH_SEC && !io->rd(io->ctx, (u32)(alt / SEC), 1, av) && av[0] == 'H' &&
            (av[1] == '+' || av[1] == 'X') && be32(av + 44) == x.total) {
            put32(av + 48, res->free_after);
            if (io->wr(io->ctx, (u32)(alt / SEC), 1, av)) return HFSFIX_IO;
        }
    }
    return HFSFIX_OK;
}
