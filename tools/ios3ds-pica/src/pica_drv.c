/*
 * ios3ds-pica: GPU driver core (see include/pica_drv.h). Freestanding: no
 * libc, no floating point, no globals.
 *
 * Register sequences follow libn3ds' gfx.c (hardwareReset, GX_memoryFill,
 * GX_displayTransfer, GX_processCommandList), minus the block reset: the
 * display controllers in the same block are live (Luma's framebuffers).
 * Completion is polled, not interrupt driven: PSC_FILLn_CNT bit 1 (only for
 * the raw register diagnostics; see pica_drv_fill below), PPF_CNT bit 8 and
 * bit 31 of the GX interrupt status register (0x034).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pica_drv.h"
#include "pica_regs.h"

#define GXO(word)   ((word) * 4u)               /* GX word index -> byte offset */
#define P3DO(reg)   (0x1000u + (reg) * 4u)
#define POLL_US     20u
#define TIMEOUT_US  2000000u

static int in_arena(const pica_drv_t *d, uint32_t pa, uint32_t len)
{
    return pa >= d->pa && len <= d->size && pa - d->pa <= d->size - len;
}

/* wait until (reg & mask) != 0; 0 = ok */
static int wait_bits(pica_drv_t *d, uint32_t off, uint32_t mask, const char *what)
{
    uint32_t t;
    for (t = 0; t < TIMEOUT_US; t += POLL_US) {
        if (d->p->rd(off) & mask)
            return 0;
        d->p->udelay(POLL_US);
    }
    d->timeouts++;
    d->p->log("[pica] TIMEOUT waiting for %s: reg 0x%03x = 0x%08x, GX stat 0x%08x, PSC0 0x%08x PSC1 0x%08x PPF 0x%08x\n",
              what, off, d->p->rd(off), d->p->rd(GXO(GX_PSC_IRQ_STAT)),
              d->p->rd(GXO(GX_PSC_FILL0) + 12), d->p->rd(GXO(GX_PSC_FILL1) + 12), d->p->rd(GXO(GX_PPF_CNT)));
    return 1;
}

uint32_t pica_drv_init(pica_drv_t *d, const pica_plat_t *p, void *arena_va, uint32_t arena_pa,
                       uint32_t arena_size, uint32_t gpu_cnt)
{
    uint32_t i;
    d->p = p;
    d->va = (uint8_t *)arena_va;
    d->pa = arena_pa;
    d->size = arena_size & ~(PICA_DRV_ALIGN - 1);
    d->gpu_cnt = gpu_cnt;
    d->submits = d->timeouts = 0;
    d->nblocks = 0;
    d->huge_floor = PICA_DRV_HUGE_FLOOR(d->size);
    d->p->log("[pica] arena pools: medium below 0x%08x (%u KiB), huge above (%u KiB)\n",
              d->pa + d->huge_floor, d->huge_floor >> 10, (d->size - d->huge_floor) >> 10);
    d->state = PICA_ERR_NO_ARENA;
    if (!arena_va || (arena_pa & (PICA_DRV_ALIGN - 1)) || d->size < PICA_DRV_CMD_SIZE * 2)
        return d->state;

    /* the fill and transfer engines idle, their interrupt flags cleared */
    p->wr(GXO(GX_PSC_FILL0) + 12, 0);
    p->wr(GXO(GX_PSC_FILL1) + 12, 0);
    p->wr(GXO(GX_PPF_CNT), 0);
    /* 3D core: end-of-list marker, and start in configuration mode (without
       this the first command list hangs the GPU - libn3ds) */
    p->wr(P3DO(P3D_IRQ_ACK), 0);
    p->wr(P3DO(P3D_IRQ_CMP), PICA_FINALIZE_MAGIC);
    p->wr(P3DO(P3D_IRQ_MASK), 0xFFFFFFF0u);
    p->wr(P3DO(P3D_IRQ_AUTOSTOP), 1);
    p->wr(P3DO(P3D_START_DRAW_FUNC0), 1);

    /* block 0: the command list staging buffer, owned by the driver */
    d->block[0].off = 0;
    d->block[0].size = PICA_DRV_CMD_SIZE;
    d->block[0].owner = 0;
    d->nblocks = 1;

    /* Hardware quirk (libn3ds GFX_init): the first PPF operation after reset
       should be a throw-away one, or later transfers can glitch. A 16-byte
       texture copy inside the staging buffer. */
    for (i = 0; i < 64; i++)
        d->va[i] = 0;
    p->to_gpu(d->va, 64);
    p->wr(GXO(GX_PPF_IN_ADDR), d->pa >> 3);
    p->wr(GXO(GX_PPF_OUT_ADDR), (d->pa + 32) >> 3);
    p->wr(GXO(GX_PPF_FLAGS), PPF_TEXCOPY);
    p->wr(GXO(GX_PPF_TC_LEN), 16);
    p->wr(GXO(GX_PPF_TC_INDIM), 0);
    p->wr(GXO(GX_PPF_TC_OUTDIM), 0);
    p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    if (wait_bits(d, GXO(GX_PPF_CNT), PPF_CNT_DONE, "the first PPF texture copy")) {
        d->state = PICA_ERR_TIMEOUT;
        return d->state;
    }
    p->wr(GXO(GX_PPF_CNT), 0);
    d->state = PICA_OK;
    return d->state;
}

static uint32_t arena_free(const pica_drv_t *d)
{
    uint32_t used = 0, i;
    for (i = 0; i < d->nblocks; i++)
        used += d->block[i].size;
    return d->size - used;
}

/* Largest contiguous hole in [lo, hi), for the arena-full diagnostic. */
static uint32_t largest_gap(const pica_drv_t *d, uint32_t lo, uint32_t hi)
{
    uint32_t prev = lo, max = 0, i;
    if (hi <= lo)
        return 0;
    for (i = 0; i < d->nblocks; i++) {
        uint32_t off = d->block[i].off, end = off + d->block[i].size;
        if (end <= lo || off >= hi)
            continue;
        if (off > prev && off - prev > max)
            max = off - prev;
        if (end > prev)
            prev = end;
    }
    if (hi > prev && hi - prev > max)
        max = hi - prev;
    return max;
}

void pica_drv_info(pica_drv_t *d, pica_info_t *out)
{
    out->version = PICA_PROTO_VERSION;
    out->state = d->state;
    out->gpu_cnt = d->gpu_cnt;
    out->arena_pa = d->pa;
    out->arena_size = d->size;
    out->arena_free = d->state == PICA_OK ? arena_free(d) : 0;
    out->submits = d->submits;
    out->timeouts = d->timeouts;
}

static void drop_block(pica_drv_t *d, uint32_t i)
{
    for (; i + 1 < d->nblocks; i++)
        d->block[i] = d->block[i + 1];
    d->nblocks--;
}

uint32_t pica_drv_free_all(pica_drv_t *d, uint32_t owner)
{
    uint32_t i = 1, n = 0;
    while (i < d->nblocks) {
        if (d->block[i].owner == owner) {
            drop_block(d, i);
            n++;
        } else {
            i++;
        }
    }
    return n;
}

static void insert_block(pica_drv_t *d, uint32_t i, uint32_t owner, uint32_t start, uint32_t size)
{
    uint32_t j;
    for (j = d->nblocks; j > i; j--)
        d->block[j] = d->block[j - 1];
    d->block[i].off = start;
    d->block[i].size = size;
    d->block[i].owner = owner;
    d->nblocks++;
}

/* Hand-built drivers (host tests) need the same floor as pica_drv_init. */
static uint32_t huge_floor(const pica_drv_t *d)
{
    uint32_t f = d->huge_floor;
    if (!f || f > d->size)
        f = PICA_DRV_HUGE_FLOOR(d->size);
    /* Tiny host-test arenas have no room for a separate pool: use it all. */
    if (f <= PICA_DRV_CMD_SIZE + PICA_DRV_ALIGN)
        f = d->size;
    return f;
}

enum { POOL_SMALL = 0, POOL_BIG = 1, POOL_HUGE = 2 };

/* How much of the reserve smaller blocks may take. On New models 4 MiB at
   the top is never given away, so a 4 MiB texture still fits after churn. */
static int reserve_room(const pica_drv_t *d, uint32_t floor, uint32_t size)
{
    uint32_t i, used = 0, keep = PICA_DRV_HUGE_KEEP(d->size);
    if (floor >= d->size || d->size - floor <= keep)
        return 0;
    for (i = 1; i < d->nblocks; i++)
        if (d->block[i].off >= floor)
            used += d->block[i].size;
    return used + size <= d->size - floor - keep;
}

/* Last resort for small blocks: the bottom of the reserve, growing upward. */
static int try_reserve_bottom(pica_drv_t *d, uint32_t owner, uint32_t size, uint32_t align,
                              uint32_t floor, uint32_t *pa)
{
    uint32_t i, prev = floor;
    if (!reserve_room(d, floor, size))
        return 0;
    for (i = 0; i <= d->nblocks; i++) {
        uint32_t next = i < d->nblocks ? d->block[i].off : d->size;
        uint32_t end = i < d->nblocks ? d->block[i].off + d->block[i].size : d->size;
        if (next > floor) {
            uint32_t start = (prev + align - 1) & ~(align - 1);
            if (start <= next && next - start >= size) {
                insert_block(d, i, owner, start, size);
                *pa = d->pa + start;
                return 1;
            }
        }
        if (end > prev)
            prev = end;
    }
    return 0;
}

/* Last resort for medium blocks: the top of the reserve, growing downward.
   Small blocks use its bottom, so the two sizes do not fight over one hole. */
static int try_reserve_top(pica_drv_t *d, uint32_t owner, uint32_t size, uint32_t align,
                           uint32_t floor, uint32_t *pa)
{
    uint32_t i;
    if (!reserve_room(d, floor, size))
        return 0;
    for (i = d->nblocks; i > 0; i--) {
        uint32_t next = i < d->nblocks ? d->block[i].off : d->size;
        uint32_t lo = d->block[i - 1].off + d->block[i - 1].size;
        uint32_t start;
        if (lo < floor)
            lo = floor;
        if (next < lo + size)
            continue;
        start = (next - size) & ~(align - 1);
        if (start >= lo) {
            insert_block(d, i, owner, start, size);
            *pa = d->pa + start;
            return 1;
        }
    }
    return 0;
}

/* Three size bands, two directions. Small textures use page-granular first
   fit from the bottom; 256 KiB - 2 MiB blocks (render targets, depth
   buffers, read-back staging, the mirror's 1 MiB texture) come down from the
   top of the *medium* pool, as before, and fall back to the bottom of the
   reserved pool when it is full; >= 2 MiB blocks come down from the top of
   that reserve. Hardware log 2026-11-25 had 10.8 MiB free in a 12 MiB arena
   and still could not place 4 MiB, because small blocks and 1 MiB targets
   had reached the top. The two-sided reserve keeps both sizes servable.
   Free space is the
   gaps between live blocks: adjacent freed blocks coalesce implicitly. */
static int try_alloc(pica_drv_t *d, uint32_t owner, uint32_t size, uint32_t align, int pool, uint32_t *pa)
{
    uint32_t i, prev_end = 0, floor = huge_floor(d);
    if (pool == POOL_SMALL) {
        /* Bottom-up first fit packs textures low and leaves the largest gap
           next to the medium pool's top. Best fit was tried here and made
           QEMU log 220-256 KiB retries with 2 MiB free: it scattered small
           blocks over all the holes instead of coalescing them. */
        for (i = 0; i <= d->nblocks; i++) {
            uint32_t next = i < d->nblocks ? d->block[i].off : d->size;
            uint32_t start = (prev_end + align - 1) & ~(align - 1);
            if (next > floor)
                next = floor;
            if (start <= next && next - start >= size) {
                insert_block(d, i, owner, start, size);
                *pa = d->pa + start;
                return 1;
            }
            if (i < d->nblocks)
                prev_end = d->block[i].off + d->block[i].size;
        }
        return try_reserve_bottom(d, owner, size, align, floor, pa);
    }
    for (i = d->nblocks; i > 0; i--) {
        uint32_t next = i < d->nblocks ? d->block[i].off : (pool == POOL_HUGE ? d->size : floor);
        uint32_t lo = d->block[i - 1].off + d->block[i - 1].size;   /* block 0 = command staging */
        uint32_t start;
        if (pool == POOL_BIG) {
            if (next > floor)               /* a huge block above the floor ends this pool */
                next = floor;
        } else if (lo < floor) {
            lo = floor;                     /* huge blocks never take small/medium space */
        }
        if (next < lo + size)
            continue;
        start = (next - size) & ~(align - 1);
        if (start >= lo) {
            insert_block(d, i, owner, start, size);
            *pa = d->pa + start;
            return 1;
        }
    }
    if (pool == POOL_BIG)
        return try_reserve_top(d, owner, size, align, floor, pa) ||
               try_reserve_bottom(d, owner, size, align, floor, pa);
    return 0;
}

/* Page granularity for small blocks. Power-of-two classes were tried and
   rejected: they wasted up to half of a 10 MiB arena and caused 256 KiB
   retries in QEMU. Best fit plus the two-sided huge reserve does the job
   without throwing away memory; free holes coalesce when blocks are freed. */
static uint32_t small_class(uint32_t bytes)
{
    return (bytes + 0xFFF) & ~0xFFFu;
}

uint32_t pica_drv_alloc(pica_drv_t *d, uint32_t owner, uint32_t size, uint32_t align, uint32_t *pa)
{
    uint32_t i, last = 0, reaped = 0;
    int pool;
    if (d->state != PICA_OK)
        return d->state;
    if (align < PICA_DRV_ALIGN)
        align = PICA_DRV_ALIGN;
    if (!size || size > d->size || (align & (align - 1)))
        return PICA_ERR_RANGE;
    size = (size + PICA_DRV_ALIGN - 1) & ~(PICA_DRV_ALIGN - 1);
    pool = size >= PICA_DRV_HUGE ? POOL_HUGE : size >= PICA_DRV_BIG ? POOL_BIG : POOL_SMALL;
    if (pool != POOL_SMALL) {
        if (align < 0x1000)
            align = 0x1000;                 /* big/huge blocks stay page aligned */
        size = (size + 0xFFF) & ~0xFFFu;
    } else {
        size = small_class(size);
        if (align < size)
            align = size;
    }
    if (d->nblocks < PICA_DRV_BLOCKS && try_alloc(d, owner, size, align, pool, pa))
        return PICA_OK;
    /* full: give back what dead clients left behind, then try once more */
    for (i = 1; i < d->nblocks; ) {
        uint32_t o = d->block[i].owner;
        if (o != owner && o != last && !d->p->alive(o)) {
            reaped += pica_drv_free_all(d, o);
            i = 1;
            continue;
        }
        last = o;
        i++;
    }
    if (reaped) {
        d->p->log("[pica] arena full: reclaimed %u blocks of exited clients\n", reaped);
        if (d->nblocks < PICA_DRV_BLOCKS && try_alloc(d, owner, size, align, pool, pa))
            return PICA_OK;
    }
    /* A full arena is normal now: the GL engine evicts textures and retries
       (docs/pica200.md 16). Say it for the first few and then every 256th. */
    if (d->full_logs++ < 4 || !(d->full_logs & 255)) {
        uint32_t f = huge_floor(d), nres = 0;
        for (i = 1; i < d->nblocks; i++)
            if (d->block[i].off >= f)
                nres++;
        d->p->log("[pica] arena full: %u bytes wanted, %u free in %u blocks (%u times): largest gaps %u KiB medium / "
                  "%u KiB reserve, %u reserve blocks\n", size, arena_free(d), d->nblocks, d->full_logs,
                  largest_gap(d, PICA_DRV_CMD_SIZE, f) >> 10, largest_gap(d, f, d->size) >> 10, nres);
    }
    return PICA_ERR_NO_MEM;
}

uint32_t pica_drv_free(pica_drv_t *d, uint32_t owner, uint32_t pa)
{
    uint32_t i;
    for (i = 1; i < d->nblocks; i++)
        if (d->pa + d->block[i].off == pa && d->block[i].owner == owner) {
            drop_block(d, i);
            return PICA_OK;
        }
    return PICA_ERR_RANGE;
}

uint32_t pica_drv_write(pica_drv_t *d, uint32_t pa, pica_uptr_t src, uint32_t len)
{
    uint8_t *p;
    if (d->state != PICA_OK)
        return d->state;
    if (!in_arena(d, pa, len) || pa - d->pa < PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    p = d->va + (pa - d->pa);
    if (len && d->p->copyin(src, p, len))
        return PICA_ERR_FAULT;
    d->p->to_gpu(p, len);
    return PICA_OK;
}

uint32_t pica_drv_read(pica_drv_t *d, uint32_t pa, pica_uptr_t dst, uint32_t len)
{
    uint8_t *p;
    if (d->state != PICA_OK)
        return d->state;
    if (!in_arena(d, pa, len) || pa - d->pa < PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    p = d->va + (pa - d->pa);
    d->p->from_gpu(p, len);
    if (len && d->p->copyout(p, dst, len))
        return PICA_ERR_FAULT;
    return PICA_OK;
}

uint32_t pica_drv_submit(pica_drv_t *d, pica_uptr_t list, uint32_t bytes,
                         uint32_t vtx_pa, pica_uptr_t vtx, uint32_t vtx_len)
{
    uint32_t t, st;
    if (d->state != PICA_OK)
        return d->state;
    if (!bytes || (bytes & 15) || bytes > PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    if (vtx_len && (st = pica_drv_write(d, vtx_pa, vtx, vtx_len)) != PICA_OK)
        return st;
    if (d->p->copyin(list, d->va, bytes))
        return PICA_ERR_FAULT;
    d->p->to_gpu(d->va, bytes);

    /* acknowledge the previous list's interrupt and wait for the flag to drop */
    d->p->wr(P3DO(P3D_IRQ_ACK), 0);
    for (t = 0; (d->p->rd(GXO(GX_PSC_IRQ_STAT)) & GX_STAT_P3D) && t < 1000; t++)
        d->p->udelay(POLL_US);
    d->p->wr(P3DO(P3D_CMDBUF_SIZE0), bytes >> 3);
    d->p->wr(P3DO(P3D_CMDBUF_ADDR0), d->pa >> 3);
    d->p->wr(P3DO(P3D_CMDBUF_JUMP0), 1);
    d->submits++;
    if (wait_bits(d, GXO(GX_PSC_IRQ_STAT), GX_STAT_P3D, "the end of a command list (P3D)")) {
        d->p->log("[pica]   list: %u bytes at PA 0x%08x, submit #%u\n", bytes, d->pa, d->submits);
        return PICA_ERR_TIMEOUT;
    }
    return PICA_OK;
}

/* The PSC fill engine only writes VRAM (hardware runs 12/13: a fill into
   FCRAM sets the done bit, increments its counter and changes nothing). The
   arena is FCRAM, so PICA_OP_FILL is done by the CPU instead. Every value
   the GL engine needs to clear (0, 0xFFFF, far depth) is a constant, and a
   constant is identical in linear and tiled memory, so this is exactly the
   same result a working fill would have had. P3D clears and the mirror's
   VRAM fills do not take this path. */
static void fill_bytes(uint8_t *p, uint32_t len, uint32_t v, uint32_t width)
{
    uint32_t i;
    if ((width & 0x300) == PSC_FILL_32) {
        for (i = 0; i < len / 4; i++)
            ((uint32_t *)p)[i] = v;
    } else if ((width & 0x300) == PSC_FILL_16) {
        for (i = 0; i < len / 2; i++)
            ((uint16_t *)p)[i] = (uint16_t)v;
    } else {                                /* 24-bit: three bytes per word */
        for (i = 0; i < len; i++)
            p[i] = (uint8_t)(v >> (8 * (i % 3)));
    }
}

uint32_t pica_drv_fill(pica_drv_t *d, const uint32_t a[8])
{
    uint32_t n;
    if (d->state != PICA_OK)
        return d->state;
    for (n = 0; n < 2; n++) {
        const uint32_t *s = a + 4 * n;
        if (s[1] && (!in_arena(d, s[0], s[1]) || s[0] - d->pa < PICA_DRV_CMD_SIZE || ((s[0] | s[1]) & 15)))
            return PICA_ERR_RANGE;
    }
    for (n = 0; n < 2; n++) {
        const uint32_t *s = a + 4 * n;
        uint8_t *p;
        if (!s[1])
            continue;
        p = d->va + (s[0] - d->pa);
        d->p->from_gpu(p, s[1]);            /* drop stale lines over GPU output */
        fill_bytes(p, s[1], s[2], s[3]);
        d->p->to_gpu(p, s[1]);              /* and write the new constant back */
    }
    return PICA_OK;
}

uint32_t pica_drv_copy_page(pica_drv_t *d, uint32_t src_pa, uint32_t dst_va,
                            uint32_t dst_pa, uint32_t bytes)
{
    uint8_t *src, *dst;
    uint32_t k;
    if (d->state != PICA_OK)
        return d->state;
    if (!bytes || bytes > PICA_PAGE_SIZE || (bytes & 3) || (dst_va & (PICA_PAGE_SIZE - 1)) ||
        !d->p->map_pa || !in_arena(d, src_pa, bytes) || src_pa - d->pa < PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    src = d->va + (src_pa - d->pa);
    if (d->p->from_gpu)
        d->p->from_gpu(src, bytes);     /* the GPU wrote it: drop our copies */
    dst = d->p->map_pa(dst_pa, bytes);
    if (!dst)
        return PICA_ERR_RANGE;
    /* the caller's lines for this page may hold stale (dirty) data: write them
       back and drop them, then copy through the kernel's mapping */
    if (d->p->cache_user)
        d->p->cache_user(dst_va, bytes);
    for (k = 0; k < bytes / 4; k++)
        ((uint32_t *)dst)[k] = ((const uint32_t *)src)[k];
    if (d->p->to_gpu)
        d->p->to_gpu(dst, bytes);       /* the CPU wrote it: write the lines back */
    return PICA_OK;
}

uint32_t pica_drv_transfer(pica_drv_t *d, uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa,
                           uint32_t dst_dim, uint32_t flags)
{
    /* worst case 4 bytes per pixel on either side */
    uint32_t slen = (src_dim & 0xFFFF) * (src_dim >> 16) * 4, dlen = (dst_dim & 0xFFFF) * (dst_dim >> 16) * 4;
    uint32_t st = PICA_OK;
    if (d->state != PICA_OK)
        return d->state;
    if (!slen || !dlen || (flags & PPF_TEXCOPY) || ((src_pa | dst_pa) & 7) ||
        !in_arena(d, src_pa, slen) || !in_arena(d, dst_pa, dlen) ||
        src_pa - d->pa < PICA_DRV_CMD_SIZE || dst_pa - d->pa < PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    d->p->from_gpu(d->va + (dst_pa - d->pa), dlen);
    d->p->wr(GXO(GX_PPF_IN_ADDR), src_pa >> 3);
    d->p->wr(GXO(GX_PPF_OUT_ADDR), dst_pa >> 3);
    d->p->wr(GXO(GX_PPF_DT_INDIM), src_dim);
    d->p->wr(GXO(GX_PPF_DT_OUTDIM), dst_dim);
    d->p->wr(GXO(GX_PPF_FLAGS), flags);
    d->p->wr(GXO(GX_PPF_UNK14), 0);
    d->p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    if (wait_bits(d, GXO(GX_PPF_CNT), PPF_CNT_DONE, "a display transfer (PPF)"))
        st = PICA_ERR_TIMEOUT;
    d->p->wr(GXO(GX_PPF_CNT), 0);
    return st;
}

uint32_t pica_drv_transfer_out(pica_drv_t *d, uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa,
                               uint32_t dst_dim, uint32_t flags)
{
    uint32_t slen = (src_dim & 0xFFFF) * (src_dim >> 16) * 4, st = PICA_OK;
    if (d->state != PICA_OK)
        return d->state;
    if (!slen || (flags & (PPF_TEXCOPY | PPF_OUT_TILED)) || ((src_pa | dst_pa) & 7) || !in_arena(d, src_pa, slen) ||
        src_pa - d->pa < PICA_DRV_CMD_SIZE)
        return PICA_ERR_RANGE;
    d->p->wr(GXO(GX_PPF_IN_ADDR), src_pa >> 3);
    d->p->wr(GXO(GX_PPF_OUT_ADDR), dst_pa >> 3);
    d->p->wr(GXO(GX_PPF_DT_INDIM), src_dim);
    d->p->wr(GXO(GX_PPF_DT_OUTDIM), dst_dim);
    d->p->wr(GXO(GX_PPF_FLAGS), flags);
    d->p->wr(GXO(GX_PPF_UNK14), 0);
    d->p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    if (wait_bits(d, GXO(GX_PPF_CNT), PPF_CNT_DONE, "a display transfer out of the arena (PPF)"))
        st = PICA_ERR_TIMEOUT;
    d->p->wr(GXO(GX_PPF_CNT), 0);
    return st;
}
