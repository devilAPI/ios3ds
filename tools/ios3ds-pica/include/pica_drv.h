/*
 * ios3ds-pica: the GPU driver core. It owns the GX registers and the arena
 * and runs one operation at a time. It is freestanding C behind a small
 * platform table, so the very same code runs
 *   - in the kernel shim (kernel/shim/pica.c: real registers, or the QEMU
 *     PICA model behind them), and
 *   - on the build host against tools/ios3ds-pica/sim (src/pica_dev_host.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include "pica_proto.h"

typedef uintptr_t pica_uptr_t;          /* a pointer in the client's address space */

typedef struct pica_plat {
    uint32_t (*rd)(uint32_t off);                   /* GX register, byte offset from 0x10400000 */
    void (*wr)(uint32_t off, uint32_t v);
    int (*copyin)(pica_uptr_t src, void *dst, uint32_t len);        /* 0 = ok */
    int (*copyout)(const void *src, pica_uptr_t dst, uint32_t len);
    void (*to_gpu)(void *p, uint32_t len);          /* the CPU wrote arena memory: write the cache back */
    void (*from_gpu)(void *p, uint32_t len);        /* the GPU wrote arena memory: drop cached copies */
    void (*udelay)(uint32_t us);
    int (*alive)(uint32_t owner);                   /* is this client (pid) still there? */
    void (*log)(const char *fmt, ...);
    /* PICA_OP_SCATTER: a CPU mapping of a physical range outside the arena
       (the kernel's linear map of FCRAM), and clean+invalidate of the
       caller's own cache lines for a page before it is overwritten through
       that mapping. Optional (map_pa is required for pica_drv_copy_pages). */
    void *(*map_pa)(uint32_t pa, uint32_t len);
    void (*cache_user)(uint32_t va, uint32_t len);
} pica_plat_t;

#define PICA_DRV_BLOCKS   224                       /* issue 1 (New 2DS XL): 160 filled up with CA textures */
#define PICA_DRV_CMD_SIZE 0x20000u                  /* command list staging inside the arena */
#define PICA_DRV_ALIGN    0x80u                     /* allocation granule (>= 2 cache lines, 16-byte GPU units) */
#define PICA_DRV_BIG      0x40000u                  /* blocks this big are placed from the top of the arena */
/* Keep a top reserve for >= 2 MiB blocks (6 MiB in a 12 MiB arena, 2 MiB in
   8-11 MiB, a quarter below), with smaller blocks falling back into its
   bottom if the medium pool fills. A 4 MiB texture then fits even when
   small layer textures and 1 MiB targets have filled the rest; the smaller
   reserve keeps an Old-model 10 MiB arena from running out of medium space. */
#define PICA_DRV_HUGE     0x200000u                 /* blocks this big get the reserved top pool */
#define PICA_DRV_HUGE_RESERVE(sz) \
    ((sz) >= (12u << 20) ? (6u << 20) : (sz) >= (8u << 20) ? (2u << 20) : ((sz) >> 2))
#define PICA_DRV_HUGE_FLOOR(sz) \
    ((sz) - PICA_DRV_HUGE_RESERVE(sz))
/* Part of the reserve smaller blocks may never take: a 4 MiB request must
   still fit on a New-model arena even after medium-pool churn. */
#define PICA_DRV_HUGE_KEEP(sz) \
    ((sz) >= (12u << 20) ? (4u << 20) : 0u)

typedef struct pica_drv {
    const pica_plat_t *p;
    uint8_t *va;                    /* arena, driver's mapping */
    uint32_t pa, size;
    uint32_t state;                 /* PICA_OK when usable */
    uint32_t gpu_cnt;
    uint32_t submits, timeouts;
    uint32_t nblocks;
    uint32_t full_logs;                     /* failed allocations (log rate limit) */
    uint32_t huge_floor;                    /* >= PICA_DRV_HUGE blocks stay above this offset */
    struct { uint32_t off, size, owner; } block[PICA_DRV_BLOCKS];   /* sorted by off */
} pica_drv_t;

/* Program the GX block for use (the GPU must already be powered and out of
   reset) and take over the arena. Returns PICA_OK or PICA_ERR_*. */
uint32_t pica_drv_init(pica_drv_t *d, const pica_plat_t *p, void *arena_va, uint32_t arena_pa,
                       uint32_t arena_size, uint32_t gpu_cnt);
void pica_drv_info(pica_drv_t *d, pica_info_t *out);

uint32_t pica_drv_alloc(pica_drv_t *d, uint32_t owner, uint32_t size, uint32_t align, uint32_t *pa);
uint32_t pica_drv_free(pica_drv_t *d, uint32_t owner, uint32_t pa);
uint32_t pica_drv_free_all(pica_drv_t *d, uint32_t owner);
uint32_t pica_drv_write(pica_drv_t *d, uint32_t pa, pica_uptr_t src, uint32_t len);
uint32_t pica_drv_read(pica_drv_t *d, uint32_t pa, pica_uptr_t dst, uint32_t len);
uint32_t pica_drv_submit(pica_drv_t *d, pica_uptr_t list, uint32_t bytes,
                         uint32_t vtx_pa, pica_uptr_t vtx, uint32_t vtx_len);
uint32_t pica_drv_fill(pica_drv_t *d, const uint32_t slot[8]);
/* the same, but into memory outside the arena that the caller has validated
   (PICA_OP_PRESENT); the source must be in the arena */
uint32_t pica_drv_transfer_out(pica_drv_t *d, uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa,
                               uint32_t dst_dim, uint32_t flags);
uint32_t pica_drv_transfer(pica_drv_t *d, uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa,
                           uint32_t dst_dim, uint32_t flags);
/* Copy an arena block into one of the caller's pages (PICA_OP_SCATTER): the
   source must be a normal arena block, dst_va is page aligned and dst_pa is
   that page's physical address. The page is mapped with map_pa, the caller's
   lines for it are dropped with cache_user, and the CPU's copy is written
   back with to_gpu. */
#define PICA_PAGE_SIZE 0x1000u
uint32_t pica_drv_copy_page(pica_drv_t *d, uint32_t src_pa, uint32_t dst_va,
                            uint32_t dst_pa, uint32_t bytes);
