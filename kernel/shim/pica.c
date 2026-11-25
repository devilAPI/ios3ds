/*
 * ios3ds shim: the PICA200 GPU service, top-level sysctl node "ios3ds_gpu"
 * (not under "kern.": see pica_proto.h).
 *
 * Userland (tools/ios3ds-gl's PICA back end) never touches GPU registers or
 * physical memory. It sends requests (tools/ios3ds-pica/include/pica_proto.h)
 * as one extra MIB component = the address of a pica_req_t; this file copies
 * the request in, runs it through the driver core (tools/ios3ds-pica/src/
 * pica_drv.c, the same code the host tests run against the software model)
 * and copies the result back. One request runs at a time.
 *
 * First use ("bring-up"), in the caller's thread:
 *   - PDN_GPU_CNT (0x10141200): Luma leaves the GPU clocked and out of reset
 *     (0x1007F) because it initialised the screens; if it is not, the blocks
 *     are taken out of reset here and that is logged
 *   - CFG11_GPUPROT (0x10140140) = 0: the GPU may read and write all of FCRAM
 *   - the arena: 8 MiB (or less) of physically contiguous kernel memory from
 *     IOMallocContiguous; the GPU addresses it by physical address
 *   - the GX block itself (already mapped at PDC_VA by entry.S) is set up by
 *     pica_drv_init(); the display controllers in it are not touched
 *
 * Kernel facts (iPod2,1 4.2.1 8C148, xnu-1504.58.28, kernelcache symbols;
 * ARM/Thumb from the symbol table's N_ARM_THUMB_DEF bit):
 *   copyin 0x80067c84 (ARM)  copyout 0x80067d04 (ARM)   (uaddr is 32 bit)
 *   IOMallocContiguous 0x801a70e0  ml_io_map 0x80067780  IODelay 0x801a6c2c
 *   IOSleep 0x801a6c40  printf 0x8001c4c4  proc_selfpid 0x8014f1be
 *   proc_find 0x8014f94c  proc_rele 0x8014f862            (all Thumb)
 *   sysctl_root passes a CTLTYPE_NODE handler (name + consumed, namelen -
 *   consumed), which is how the request pointer gets here.
 * Everything is logged with a "[pica]" prefix through the kernel's printf,
 * i.e. the console ring the ARM9 shows and saves.
 *
 * The GPU display mirror (boot/common/gpu_mirror.h, docs/pica200.md 9) lives
 * here too, because it shares the GPU with the GL service:
 *   - shim_gpu_thread_poll() runs in thread context (the input daemon's and
 *     the GL service's sysctl calls): once the ARM9 says the iOS display is
 *     on, it brings the GPU up, self-tests the mirror and switches it on;
 *   - shim_gpu_irq() runs from entry.S on every VBlank and timer interrupt
 *     (IRQ mode, private stack, IRQs masked): it starts a frame every
 *     vbl_div-th VBlank and advances the one in flight. It never waits for
 *     the GPU (unless boot.cfg gpu_spin_us says so).
 *   - `busy` arbitrates: 1 = a GL request owns the GPU, 2 = a mirror frame.
 *     A VBlank that finds GL busy skips its frame; a GL request that finds a
 *     frame in flight sleeps until the interrupts have finished it.
 * boot.cfg gpu=0 (gmbox_t.gpu_enable) keeps the shim away from the GPU
 * entirely: the GL engine falls back to its software rasterizer and the
 * ARM9 keeps copying the screen.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "kpi.h"
#include "pica_drv.h"
#include "pica_regs.h"
#include "pica_mirror.h"
#include "gpu_mirror.h"
#include "stereo.h"
#include "cam_proto.h"              /* frame-buffer reservation for the cameras */

struct sysctl_oid {
    void       *oid_parent;
    void       *oid_link;
    int         oid_number;
    int         oid_kind;
    void       *oid_arg1;
    int         oid_arg2;
    const char *oid_name;
    int       (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};

#define CTLFLAG_RD      0x80000000
#define CTLTYPE_NODE    1
#define OID_AUTO        (-1)
#define EFAULT 14
#define EINVAL 22

#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))    /* Thumb */
#define KPA(addr, ret, ...) ((ret (*)(__VA_ARGS__))(addr))          /* ARM */
#define sysctl_register_oid   KPT(IOS3DS_KPI_SYSCTL_REGISTER_OID, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPT(IOS3DS_KPI_SYSCTL_HANDLE_OPAQUE, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_ROOT_CHILDREN  ((void *)IOS3DS_KPI_SYSCTL__CHILDREN)      /* _sysctl__children: top level */
#define k_copyin              KPA(IOS3DS_KPI_COPYIN, int, u32, void *, u32)
#define k_copyout             KPA(IOS3DS_KPI_COPYOUT, int, const void *, u32, u32)
#define IOMallocContiguous    KPT(IOS3DS_KPI_IOMALLOCCONTIGUOUS, void *, u32, u32, uint32_t *)
#define IOFreeContiguous      KPT(IOS3DS_KPI_IOFREECONTIGUOUS, void, void *, u32)
#define ml_io_map             KPT(IOS3DS_KPI_ML_IO_MAP, void *, u32, u32)
#define IODelay               KPT(IOS3DS_KPI_IODELAY, void, u32)
#define IOSleep               KPT(IOS3DS_KPI_IOSLEEP, void, u32)
#define k_printf              KPT(IOS3DS_KPI_PRINTF, void, const char *, ...)
#define proc_selfpid          KPT(IOS3DS_KPI_PROC_SELFPID, int, void)
#define proc_find             KPT(IOS3DS_KPI_PROC_FIND, void *, int)
#define proc_rele             KPT(IOS3DS_KPI_PROC_RELE, int, void *)

#define GX_VA       0xBFE00000u         /* entry.S: 1 MiB device section over PA 0x10400000 */
#define PRIV_VA     0xBFF00000u         /* entry.S: MPCore private region (watchdog = clock) */
#define AXI_PA      0x1FF00000u         /* entry.S: AXI WRAM section (fake S5L register pages) */
#define AXI_VA      0xBFD00000u
#define GM          ((volatile gmbox_t *)(SHIM_VA + SHIM_GM_OFF))
#define CFG11_PA    0x10140000u
#define PDN_PA      0x10141000u
#define GPUPROT_OFF 0x140u
#define GPU_CNT_OFF 0x200u
#define GPU_CNT_ON  (PDN_GPU_CNT_CLK_EN | PDN_GPU_CNT_NORST_ALL)

/* first words of the GPU part (link.ld puts .gpuhdr at GPU_PART_VA): the
   main shim calls in only when this is there (gpu_glue.c) */
__attribute__((section(".gpuhdr"), used))
static const uint32_t gpu_part_hdr[4] = { GPU_PART_MAGIC, 1, GPU_PART_PA, GM_BLOB_PA };

pica_drv_t drv;                     /* shared with cam.c (arena VA/PA) */
/* The camera's three frame buffers are reserved here, once, while the GPU
   driver is being initialised (no concurrent GL requests yet). cam.c reads
   the PA at START; if there is no arena it falls back to IOMallocContiguous. */
uint32_t shim_cam_reserved_pa;
#define PICA_CAM_OWNER  0xFFFFFFFEu /* never a pid: pica_drv_free_all(client) cannot free it */
static uint32_t bringup_state;               /* 0 = not tried, 1 = done (drv.state says how it went) */
static volatile uint32_t busy;              /* 0 free, 1 GL request, 2 mirror frame */
/* A VBlank found the GPU busy with GL: the mirror goes next. New GL requests
   wait (take) and the next interrupt with the GPU free starts the frame, so
   a GL client drawing back to back cannot starve the screen copy (QEMU
   lockburst: the ARM9 saw no GPU frames for 1 s and copied with the CPU). */
static volatile uint32_t m_pending;
/* 1 = a GL present (or scatter) wrote a fresh frame into the iOS framebuffer,
   or the display config changed (mode / stereo k / want), since the last
   mirror frame: the mirror only does GPU work when there is new content or a
   rebuild is required (docs/perf.md). Cleared when a frame is started. */
volatile uint32_t m_dirty;              /* also set by pica_stereo.c's PDC switch */

/* ---- the ARM9 <-> ARM11 display mailbox (gmbox_t at shim+0x7F00) ---- */
extern uint32_t cfg_clcd_pa;                /* entry.S: fake CLCD page (PA), patched in by mkboot */

static void dc_clean(volatile void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
void shim_l2_inv(uint32_t pa, uint32_t len);   /* entry.S: New 3DS L2, write-through (docs/n3ds.md) */
extern u32 cfg_n3ds;                       /* entry.S: ARM9 N3RES_* word (boot/common/ios3ds.h) */
uint32_t arena_va, arena_pa;       /* shared with cam.c; arena_size stays private */
static uint32_t arena_size;
/* drop stale L2 lines of a kernel buffer: the arena (IOMallocContiguous VA)
   or XNU's linear map. Invalidating clean lines never loses data (the L2
   runs write-through), so a wrong guess only costs time. */
static void l2_inv_kbuf(const volatile void *p, uint32_t len)
{
    uint32_t v = (uint32_t)p;
    if (v - arena_va < arena_size) shim_l2_inv(arena_pa + (v - arena_va), len);
    else if (v - IOS3DS_KPI_VIRT_BASE < FCRAM_N3DS_SIZE)
        shim_l2_inv(v - (IOS3DS_KPI_VIRT_BASE - 0x20000000u), len);
}
static void dc_inval(volatile void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    l2_inv_kbuf(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
/* lines 0-1 and the ARM9's stereo request (line 5) are written uncached by
   the monitor: drop our copies first (256 bytes = the whole mailbox) */
static void gm_fetch(void) { dc_inval(GM, sizeof(gmbox_t)); }
/* lines 2-5 are ours: write them back so the ARM9 sees the mode counters */
static void gm_publish(void) { dc_clean(&GM->state, sizeof(gmbox_t) - 32); }
static int gm_valid(void) { gm_fetch(); return GM->magic == GM_MAGIC; }

static uint32_t p_rd(uint32_t off) { return *(vu32 *)(GX_VA + off); }
static void p_wr(uint32_t off, uint32_t v) { *(vu32 *)(GX_VA + off) = v; }
static int p_copyin(pica_uptr_t src, void *dst, uint32_t len) { return k_copyin((uint32_t)src, dst, len); }
static int p_copyout(const void *src, pica_uptr_t dst, uint32_t len) { return k_copyout(src, (uint32_t)dst, len); }

/* PICA_OP_SCATTER: a CPU mapping of FCRAM for the page copy (XNU's linear
   map; VRAM has none, so a scattered VRAM destination is refused), and drop
   the caller's own lines for a page before it is overwritten. */
static void *p_map_pa(uint32_t pa, uint32_t len)
{
    if (pa < 0x20000000u || pa + len > 0x20000000u + FCRAM_N3DS_SIZE)
        return 0;
    return (void *)(pa + (IOS3DS_KPI_VIRT_BASE - 0x20000000u));   /* XNU linear map */
}
static void p_cache_user(uint32_t va, uint32_t len)
{
    uint32_t a = va & ~31u, end = va + len;
    for (; a < end; a += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* The arena is ordinary cached kernel memory and the GPU reads and writes
   DRAM directly: write dirty lines back before it reads, drop our copies
   after it wrote. Every block is a multiple of 128 bytes, so no cache line
   is shared with someone else's data. */
static void p_to_gpu(void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static void p_from_gpu(void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    l2_inv_kbuf(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static void p_udelay(uint32_t us) { IODelay(us); }
static int p_alive(uint32_t pid)
{
    void *p = proc_find((int)pid);
    if (!p)
        return 0;
    proc_rele(p);
    return 1;
}

static pica_plat_t plat;                /* filled at bring-up (k_printf is not a constant expression) */

static void gpu_clock_fix(void);

static void bringup(void)
{
    /* Old 3DS has only 128 MiB for XNU: 10 MiB first here gives every app 2 MiB
       more wired-free memory than the old 12 MiB default. run16 (CA on the GPU)
       fragmented 8 MiB (a 704 KiB texture found 290 KiB free in 90 blocks), so
       10 - not 8 - is the floor; the camera's 450 KiB block still fits. New
       models keep 12 MiB (re-log 75): they have the extra 128 MiB of FCRAM.
       Fallbacks stay 8/4/2 MiB if even 10 MiB is too fragmented to allocate. */
    static const uint32_t sizes[] = { 0xC00000, 0xA00000, 0x800000, 0x400000, 0x200000 };
    unsigned first = (cfg_n3ds & N3RES_NEW) ? 0 : 1;   /* skip 12 MiB on Old models */
    vu32 *pdn, *cfg11;
    uint32_t cnt = 0, prot = 0, pa = 0, size = 0, i, st;
    void *va = 0;

    bringup_state = 1;
    plat.rd = p_rd; plat.wr = p_wr; plat.copyin = p_copyin; plat.copyout = p_copyout;
    plat.to_gpu = p_to_gpu; plat.from_gpu = p_from_gpu; plat.udelay = p_udelay; plat.alive = p_alive;
    plat.map_pa = p_map_pa; plat.cache_user = p_cache_user;
    plat.log = k_printf;
    drv.p = &plat;
    drv.state = PICA_ERR_GPU_OFF;
    if (gm_valid() && !GM->gpu_enable) {
        k_printf("[pica] GPU switched off in boot.cfg (gpu=0): not touching it\n");
        return;
    }

    pdn = (vu32 *)ml_io_map(PDN_PA, 0x1000);
    cfg11 = (vu32 *)ml_io_map(CFG11_PA, 0x1000);
    if (!pdn || !cfg11) {
        k_printf("[pica] cannot map PDN/CFG11 (%p %p): no GPU\n", pdn, cfg11);
        return;
    }
    cnt = pdn[GPU_CNT_OFF / 4];
    prot = cfg11[GPUPROT_OFF / 4];
    k_printf("[pica] bring-up: PDN_GPU_CNT 0x%08x, GPUPROT 0x%08x, GX clock 0x%08x, GX stat 0x%08x\n",
             cnt, prot, p_rd(0x004), p_rd(0x034));
    drv.gpu_cnt = cnt;
    if ((cnt & GPU_CNT_ON) != GPU_CNT_ON) {
        /* Not what Luma leaves behind after screen init. Clock on, blocks out
           of reset (libn3ds PDN_controlGpu); without Luma's display setup the
           LCDs stay dark, but offscreen rendering can work. */
        k_printf("[pica] GPU was not running (expected 0x%08x): enabling clock, releasing resets\n", GPU_CNT_ON);
        pdn[GPU_CNT_OFF / 4] = PDN_GPU_CNT_CLK_EN;
        IODelay(100);
        pdn[GPU_CNT_OFF / 4] = GPU_CNT_ON;
        IODelay(100);
        p_wr(0x004, 0x70100);           /* GX clock setup, as libn3ds/Luma write it after a reset */
        if ((pdn[GPU_CNT_OFF / 4] & GPU_CNT_ON) != GPU_CNT_ON) {
            k_printf("[pica] PDN_GPU_CNT stays 0x%08x: GPU unusable\n", pdn[GPU_CNT_OFF / 4]);
            return;
        }
    }
    cfg11[GPUPROT_OFF / 4] = 0;         /* GPU DMA may reach all of FCRAM (the arena lives there) */

    drv.state = PICA_ERR_NO_ARENA;
    for (i = first; i < sizeof sizes / sizeof sizes[0] && !va; i++) {
        size = sizes[i];
        va = IOMallocContiguous(size, 0x1000, &pa);
        if (va && pa + size > 0x20000000u + FCRAM_SIZE) {
            /* New 3DS with n3ds_ram: not in the extension, whether the GPU
               reaches it is unverified (docs/n3ds.md section 6) */
            IOFreeContiguous(va, size);
            va = 0;
        }
    }
    if (!va || !pa) {
        k_printf("[pica] no physically contiguous memory for the GPU arena (tried down to %u KiB)\n", size >> 10);
        return;
    }
    /* Whatever the kernel left in the D-cache for these pages (zeroing, a
       previous owner) must not be written back over GPU output later: write
       back and drop the whole data cache once (c7,c14,0), then every later
       CPU access to the arena does its own line maintenance. QEMU has no
       caches, so only hardware can show this matters. */
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 0\n\tmcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    arena_va = (uint32_t)va;
    arena_pa = pa;
    arena_size = size;
    shim_l2_inv(pa, size);              /* New 3DS L2: no stale copies either */
    /* the part of libn3ds' hardwareReset() Luma never does (GX clock, VRAM
       banks, DMA priorities; docs/pica200.md section 12). The staged
       [gpu-diag] diagnostics were moved out of the shim to make room
       (scripts/mkboot.py). */
    gpu_clock_fix();
    st = pica_drv_init(&drv, &plat, va, pa, size, cnt);
    /* Leave the small (2/4 MiB fallback) arenas to the display mirror: the
       camera then uses the old IOKit path. */
    if (st == PICA_OK && size >= (6u << 20)) {
        if (pica_drv_alloc(&drv, PICA_CAM_OWNER, CAM_NBUF * CAM_FRAME_BYTES, 32,
                           &shim_cam_reserved_pa) != PICA_OK)
            shim_cam_reserved_pa = 0;
        if (shim_cam_reserved_pa)
            k_printf("[cam] reserved %u x %u KiB in the GPU arena at PA 0x%08x\n",
                     (unsigned)CAM_NBUF, CAM_FRAME_BYTES >> 10, (unsigned)shim_cam_reserved_pa);
    }
    k_printf("[pica] arena %u KiB at VA %p = PA 0x%08x; driver %s (state %u)\n",
             size >> 10, va, pa, st == PICA_OK ? "ready" : "FAILED", st);
}

/* ---- PICA_OP_PRESENT: the caller's memory as a GPU destination ----------
   Walk the caller's own page tables (TTBR0, we run in its thread) for a
   user VA: the PA, and whether user code may write it (ARMv6 AP = 11, APX
   0). So a process can only aim the GPU at memory it could write itself. */
static uint32_t fcram_va(uint32_t pa, int *ok)
{
    *ok = pa >= 0x20000000u && pa < 0x20000000u + FCRAM_N3DS_SIZE;    /* New 3DS: page tables up there too */
    /* XNU's linear map of FCRAM: VA = PA + (virtBase - physBase) */
    return pa + (IOS3DS_KPI_VIRT_BASE - 0x20000000u);
}

static uint32_t last_l1, last_l2;          /* descriptors of the last walk (for the log) */

static int uva_to_pa(uint32_t va, uint32_t *pa)
{
    uint32_t ttbr0, ttbcr, n, l1, e, l2, ok;
    int okv;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
    n = ttbcr & 7;
    if (n && va >= (0x80000000u >> (n - 1)))
        return -1;                      /* not a TTBR0 (user) address */
    l1 = ttbr0 & ~((1u << (14 - n)) - 1);
    e = *(vu32 *)(fcram_va(l1, &okv) + (va >> 20) * 4);
    last_l1 = e;
    last_l2 = 0;
    if (!okv)
        return -1;
    if ((e & 3) == 2) {                 /* section: AP bits 11:10, APX bit 15 */
        ok = ((e >> 10) & 3) == 3 && !(e & (1u << 15));
        *pa = (e & 0xFFF00000u) | (va & 0xFFFFFu);
        return ok ? 0 : -2;
    }
    if ((e & 3) != 1)
        return -1;
    l2 = *(vu32 *)(fcram_va(e & ~0x3FFu, &okv) + ((va >> 12) & 0xFF) * 4);
    last_l2 = l2;
    if (!okv || !(l2 & 3))
        return -1;
    ok = ((l2 >> 4) & 3) == 3 && !(l2 & (1u << 9));     /* AP 5:4, APX 9 (small and large pages) */
    *pa = (l2 & 1) && !(l2 & 2) ? (l2 & 0xFFFF0000u) | (va & 0xFFFFu) : (l2 & 0xFFFFF000u) | (va & 0xFFFu);
    return ok ? 0 : -2;
}

static uint32_t present(uint32_t src_pa, uint32_t dim, uint32_t va, uint32_t bytes, uint32_t flags)
{
    static int said_ok, said_bad;
    uint32_t pa0 = 0, pa = 0, off;
    int r = 0;
    if (!bytes || (va & 7) || bytes != (dim & 0xFFFF) * (dim >> 16) * 4 ||
        (flags & (PPF_TEXCOPY | PPF_OUT_TILED | PPF_CROP)) || ((flags >> 12) & 7) != PICA_FB_RGBA8)
        return PICA_ERR_RANGE;
    /* every page must be mapped user-writable, and one physical run */
    for (off = 0; off < bytes; off = (((va + off) | 0xFFFu) + 1) - va) {
        if ((r = uva_to_pa(va + off, &pa)) != 0)
            break;
        if (!off)
            pa0 = pa;
        else if (pa != pa0 + off) {
            r = -3;
            break;
        }
    }
    if (!r && !((pa0 >= 0x20000000u && pa0 + bytes <= 0x28000000u) ||
                (pa0 >= PICA_VRAM_PA && pa0 + bytes <= PICA_VRAM_PA + PICA_VRAM_SIZE)))
        r = -4;
    if (!r && (flags & PICA_PRESENT_TEST_SCATTER) && pa0 >= 0x20000000u && pa0 < 0x28000000u)
        r = -3;                     /* test hook: FCRAM destination through PICA_OP_SCATTER */
    if (r) {
        if (!said_bad++) {
            uint32_t ttbr0, ttbcr, dacr, sctlr;
            __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
            __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
            __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
            __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
            k_printf("[pica] present: destination 0x%08x (+%u) not usable: %s (PA 0x%08x at +0x%x; L1 %08x L2 %08x, "
                     "TTBR0 %08x TTBCR %x DACR %08x SCTLR %08x)\n", va, bytes,
                     r == -1 ? "not mapped" : r == -2 ? "not user-writable" : r == -3 ? "not physically contiguous"
                     : "not in FCRAM/VRAM", r == -3 ? pa : pa0, off, last_l1, last_l2, ttbr0, ttbcr, dacr, sctlr);
        }
        return r == -2 || r == -1 ? PICA_ERR_FAULT : PICA_ERR_NOT_CONTIG;
    }
    /* the CPU may hold dirty lines of the destination (CA or the engine wrote
       it before): write them back and drop them, or they could later be
       evicted over the GPU's picture. We run in the caller: its VAs work. */
    p_cache_user(va, bytes);
    shim_l2_inv(pa0, bytes);            /* New 3DS L2: the caller reads the GPU's picture later */
    if (!said_ok++)
        k_printf("[pica] present: GPU writes the caller's surface directly: user VA 0x%08x = PA 0x%08x (%s, %u KiB)\n",
                 va, pa0, pa0 >= 0x20000000u ? "FCRAM" : "VRAM", bytes >> 10);
    {
        uint32_t st = pica_drv_transfer_out(&drv, src_pa, dim, pa0, dim, flags);
        if (st == PICA_OK)
            m_dirty = 1;                /* a fresh frame is waiting to be mirrored */
        return st;
    }
}

/* PICA_OP_SCATTER: the destination PRESENT refused as not one contiguous run
   (CA's second 320x480 surface after an app returns to the home screen, in
   hardware runs 21/25/29 and the New 2DS XL's run 1). The engine has already
   run the same PPF transfer into a linear arena buffer; this copies it into
   the caller's scattered pages, page by page through XNU's linear map, with
   the caller's own lines dropped first and the CPU's copy written back.
   Only pages the caller may write, in FCRAM, are touched. */
static uint32_t scatter(uint32_t src_pa, uint32_t va, uint32_t bytes)
{
    uint32_t off, pa, len, st;
    int r;
    if (!bytes || (va & (PICA_PAGE_SIZE - 1)) || (bytes & 3))
        return PICA_ERR_RANGE;
    for (off = 0; off < bytes; off += len) {
        len = bytes - off < PICA_PAGE_SIZE ? bytes - off : PICA_PAGE_SIZE;
        if ((r = uva_to_pa(va + off, &pa)) != 0)
            return r == -1 || r == -2 ? PICA_ERR_FAULT : PICA_ERR_NOT_CONTIG;
        if (pa < 0x20000000u || pa + len > 0x20000000u + FCRAM_N3DS_SIZE)
            return PICA_ERR_NOT_CONTIG;
        st = pica_drv_copy_page(&drv, src_pa + off, va + off, pa, len);
        if (st != PICA_OK)
            return st;
        shim_l2_inv(pa, len);               /* New 3DS L2: write-through, safe to drop */
    }
    m_dirty = 1;                            /* a fresh frame is waiting to be mirrored */
    return PICA_OK;
}

static void run(pica_req_t *r, uint32_t pid)
{
    uint32_t *a = r->a;
    if (!bringup_state && r->op != PICA_OP_CONFIG)
        bringup();
    switch (r->op) {
    case PICA_OP_INFO:
        pica_drv_info(&drv, (pica_info_t *)a);
        r->status = PICA_OK;
        break;
    case PICA_OP_ALLOC:    r->status = pica_drv_alloc(&drv, pid, a[0], a[1], &a[0]); break;
    case PICA_OP_FREE:     r->status = pica_drv_free(&drv, pid, a[0]); break;
    case PICA_OP_FREE_ALL: pica_drv_free_all(&drv, pid); r->status = PICA_OK; break;
    case PICA_OP_WRITE:    r->status = pica_drv_write(&drv, a[0], a[1], a[2]); break;
    case PICA_OP_READ:     r->status = pica_drv_read(&drv, a[0], a[1], a[2]); break;
    case PICA_OP_SUBMIT:   r->status = pica_drv_submit(&drv, a[0], a[1], a[2], a[3], a[4]); break;
    case PICA_OP_FILL:     r->status = pica_drv_fill(&drv, a); break;
    case PICA_OP_TRANSFER: r->status = pica_drv_transfer(&drv, a[0], a[1], a[2], a[3], a[4]); break;
    case PICA_OP_PRESENT:  r->status = present(a[0], a[1], a[2], a[3], a[4]); break;
    case PICA_OP_SCATTER:  r->status = scatter(a[0], a[1], a[2]); break;
    case PICA_OP_DIRTY:    /* a CPU read-back wrote a surface: the mirror must copy it */
        m_dirty = 1;
        r->status = PICA_OK;
        break;
    case PICA_OP_CONFIG:
        a[0] = gm_valid() && GM->gpu_enable && GM->ca_gpu ? PICA_CFG_CA_GPU : 0;
        r->status = PICA_OK;
        break;
    default:               r->status = PICA_ERR_BAD_OP; break;
    }
}

static int take(void)
{
    uint32_t cpsr, got;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr | 0x80) : "memory");
    got = !busy && !m_pending;
    if (got)
        busy = 1;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    return (int)got;
}

void shim_gpu_thread_poll(void);

/* This file is built at -Os to leave room in gpu.bin for the camera frame
   buffers' allocator (kernel/shim/cam.c). Keep the three hot paths at -O2;
   the rest (bring-up, display-mirror setup) runs once. */
#define PICA_HOT __attribute__((optimize("O2")))
static PICA_HOT int pica_sysctl(struct sysctl_oid *oidp, void *arg1, int namelen, void *req)
{
    int *name = (int *)arg1;
    pica_req_t r;
    if (namelen == 0) {                 /* "sysctl ios3ds_gpu": status only, no side effects */
        pica_info_t info;
        if (bringup_state) {
            pica_drv_info(&drv, &info);
        } else {
            uint32_t *w = (uint32_t *)&info, i;
            for (i = 0; i < sizeof info / 4; i++)
                w[i] = 0;
            info.version = PICA_PROTO_VERSION;
            info.state = PICA_ERR_GPU_OFF;      /* not brought up yet */
        }
        return sysctl_handle_opaque(oidp, &info, sizeof info, req);
    }
    if (namelen != 1)
        return EINVAL;
    if (k_copyin((uint32_t)name[0], &r, sizeof r))
        return EFAULT;
    while (!take())
        IOSleep(1);                     /* a GL request or a mirror frame owns the GPU */
    run(&r, (uint32_t)proc_selfpid());
    busy = 0;
    shim_gpu_thread_poll();
    if (k_copyout(&r, (uint32_t)name[0], sizeof r))
        return EFAULT;
    return 0;
}

static struct sysctl_oid pica_oid = {
    .oid_parent  = SYSCTL_ROOT_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_NODE),
    .oid_name    = PICA_SYSCTL_LEAF,
    .oid_handler = pica_sysctl,
    .oid_fmt     = "N",
};

/* Thread context only. Idempotent. Only registers the node: the GPU is not
   touched until the first request. */
void shim_pica_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&pica_oid);
}

/* ------------------------------------------------------------------------ */
/* GPU display mirror                                                       */
/* ------------------------------------------------------------------------ */

#define WD_COUNT   (*(vu32 *)(PRIV_VA + 0x624))     /* watchdog, timer mode: counts down */
#define TICK_HZ    1047311u
#define GUARD      4096u

uint32_t shim_now32(void);                 /* entry.S: TICK_HZ on every model (docs/n3ds.md) */
static uint32_t m_ticks(void) { return shim_now32(); }
static void m_to_gpu(void *p, uint32_t len) { dc_clean(p, len); }
static void m_from_gpu(void *p, uint32_t len) { dc_inval(p, len); }

static pm_plat_t mplat;
static pica_mirror_t mir;

static void m_relax(void) { IODelay(10); }

static void mplat_init(void)
{
    mplat.rd = p_rd;
    mplat.wr = p_wr;
    mplat.ticks = m_ticks;
    mplat.tick_hz = TICK_HZ;
    mplat.to_gpu = m_to_gpu;
    mplat.from_gpu = m_from_gpu;
    mplat.log = k_printf;
    mplat.relax = m_relax;
}

/* libn3ds hardwareReset() minus the reset: Luma's screen init never sets the
   GX clock register (run12 read 0x00000000) and GSP/libn3ds always write
   0x70100 there. boot.cfg gpu_clk=0 leaves it alone. */
static void gpu_clock_fix(void)
{
    uint32_t clk = p_rd(0x004), vram = p_rd(0x030), pr0 = p_rd(0x050), pr1 = p_rd(0x054);
    if (gm_valid() && !GM->gpu_clk) {
        k_printf("[pica] GX clock 0x%08x left alone (boot.cfg gpu_clk=0)\n", clk);
        return;
    }
    if (!clk)
        p_wr(0x004, 0x70100);
    if (vram & 0xF00)
        p_wr(0x030, vram & ~0xF00u);                /* all VRAM banks on */
    p_wr(0x050, 0x22221200u);                       /* libn3ds PSC_DMA_PRIO0(2,2,2,2,1,2,0,0) */
    p_wr(0x054, 0x00000FF2u);                       /* PSC_DMA_PRIO1(15,15,2) */
    k_printf("[pica] GX setup as libn3ds hardwareReset: clock 0x%08x -> 0x%08x, psc_vram 0x%08x -> 0x%08x, "
             "dma prio 0x%08x/0x%08x -> 0x%08x/0x%08x\n", clk, p_rd(0x004), vram, p_rd(0x030), pr0, pr1,
             p_rd(0x050), p_rd(0x054));
}
static volatile uint32_t m_state;           /* GM_ST_* (the IRQ side only acts on ACTIVE) */
static uint32_t m_tried, m_vbl, m_fb[4], m_nsets;

static void m_set(uint32_t state, uint32_t why)
{
    __asm__ volatile("" ::: "memory");      /* mir, m_fb, m_nsets are complete before the IRQ side may look */
    m_state = state;
    GM->state = state;
    GM->reason = why;
    gm_publish();
}

/* An LCD framebuffer the ARM9 reported: in VRAM, 8-byte aligned, clear of
   /vram (iOS) and of the mirror's own texture and render targets. */
static int fb_ok(uint32_t pa, uint32_t len)
{
    if (!pa || (pa & 7) || pa < PICA_VRAM_PA || pa + len > PICA_VRAM_PA + PICA_VRAM_SIZE)
        return 0;
    if (pa < GM_IOS_VRAM_HI)
        return 0;
    return pa + len <= GM_VRAM_LO || pa >= GM_VRAM_HI;
}

/* Start a mirror frame now: read the CLCD geometry and begin the GPU ops.
   The caller owns the GPU (busy == 2). Returns 1 if a frame started, 0 if it
   was skipped (display off or a bad framebuffer); on 0 the caller clears
   busy = 2 again. */
static int m_begin_frame(void)
{
    vu32 *clcd;
    pm_frame_t f;
    gm_fetch();
    if (GM->ack_seq != GM->seq) {
        GM->ack_seq = GM->seq;          /* nothing built from an older line 0 is in flight */
        gm_publish();
    }
    if (!GM->want)
        return 0;
    /* the iOS framebuffer as AppleM2CLCD scans it out (fake CLCD page) */
    clcd = (vu32 *)(cfg_clcd_pa - AXI_PA + AXI_VA);
    f.src_pa = clcd[0x24 / 4];
    if (!(clcd[0] & 1) || clcd[0x28 / 4] != GM_SRC_W || clcd[0x30 / 4] != (GM_SRC_W << 16 | GM_SRC_H) ||
        f.src_pa < PICA_VRAM_PA || f.src_pa + GM_SRC_W * GM_SRC_H * 4 > GM_IOS_VRAM_HI || (f.src_pa & 7)) {
        GM->reason = GM_WHY_GEOMETRY;   /* skipped; the ARM9 copies while frames stand still */
        return 0;
    }
    f.mode = GM->mode;
    f.nsets = m_nsets;
    f.stereo = GM->stereo_on && GM->stereo_ok;
    f.k = GM->stereo_k;
    f.tex_va = (void *)(drv.va + (mir.tex_pa - drv.pa));
    f.top[0] = m_fb[0];
    f.top[1] = m_fb[1];
    f.bot[0] = m_fb[2];
    f.bot[1] = m_fb[3];
    m_dirty = 0;
    pm_begin(&mir, &f);
    GM->mirror_busy = 1;
    gm_publish();                       /* the ARM9 must see an in-flight frame */
    return 1;
}

/* Thread context (after a GL request, or the input daemon's poll): a mirror
   frame was skipped because GL held the GPU (m_pending, set only on a due
   VBlank); start it now instead of waiting for the next interrupt. The frame
   stays VBlank-cadenced: m_pending is what the VBlank left behind. */
static void m_maybe_start(void)
{
    uint32_t cpsr, start = 0;
    if (m_state != GM_ST_ACTIVE || !m_dirty || !m_pending)
        return;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr | 0x80) : "memory");
    if (!busy) {
        busy = 2;
        m_pending = 0;
        start = 1;
    }
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    if (start && !m_begin_frame())
        busy = 0;
}

/* Thread context. Brings the mirror up once the ARM9 asks for it. */
PICA_HOT void shim_gpu_thread_poll(void)
{
    uint32_t src_pa = 0, out_pa = 0, tex_pa = 0, bad0 = 0, fbad[4], t0, i;
    fbad[0] = fbad[1] = fbad[2] = fbad[3] = 0;
    int v, keep_tex = 0;

    /* PDC0 3D mode is needed even when the display mirror is off (the stereo
       path uses the ARM9 CPU copy), so this runs before the mirror gates. */
    extern void shim_stereo_poll(void);
    shim_stereo_poll();
    /* a present left a fresh frame and a VBlank had skipped the mirror while
       GL held the GPU: start it right away (the frame after a present is the
       mirror's natural slot). */
    m_maybe_start();
    if (m_tried || !gm_valid())
        return;
    if (!GM->gpu_enable || !GM->mirror_enable) {
        m_tried = 1;
        k_printf("[gpu] display mirror off in boot.cfg (gpu=%u gpu_mirror=%u): the ARM9 copies the screen\n",
                 GM->gpu_enable, GM->mirror_enable);
        m_set(GM_ST_DISABLED, GM_WHY_CFG);
        return;
    }
    if (!GM->want) {
        if (GM->state != GM_ST_WAIT)
            m_set(GM_ST_WAIT, GM_WHY_NONE);
        return;
    }
    if (!take())
        return;                         /* GL is using the GPU: next call */
    m_tried = 1;
    m_set(GM_ST_TEST, GM_WHY_NONE);
    if (!bringup_state)
        bringup();
    if (drv.state != PICA_OK) {
        k_printf("[gpu] display mirror: GPU not usable (state %u): CPU copy stays\n", drv.state);
        m_set(GM_ST_FAILED, GM_WHY_NO_GPU);
        goto out;
    }
    mplat_init();
    /* the texture: 512x512 RGBA8, power of two (run13: NPOT textures sample
       garbage), 1 MiB - only the FCRAM arena has room */
    if (pica_drv_alloc(&drv, 0, GM_TEX_BYTES, 0x1000, &tex_pa) != PICA_OK) {
        k_printf("[gpu] display mirror: no arena memory for the 512x512 texture: CPU copy stays\n");
        m_set(GM_ST_FAILED, GM_WHY_NO_MEM);
        goto out;
    }
    if (pm_init(&mir, &mplat, (void *)GM_BLOB_VA, GM_BLOB_PA, tex_pa)) {
        k_printf("[gpu] display mirror: no valid gpumirror.bin at PA 0x%08x (magic 0x%08x): CPU copy stays\n",
                 GM_BLOB_PA, *(vu32 *)GM_BLOB_VA);
        m_set(GM_ST_FAILED, GM_WHY_NO_BLOB);
        goto out;
    }
    gm_fetch();
    m_fb[0] = GM->top_fb[0];
    m_fb[1] = GM->top_fb[1];
    m_fb[2] = GM->bot_fb[0];
    m_fb[3] = GM->bot_fb[1];
    m_nsets = m_fb[1] && m_fb[3] && (m_fb[1] != m_fb[0] || m_fb[3] != m_fb[2]) ? 2 : 1;
    k_printf("[gpu] display mirror: LCD framebuffers top 0x%08x/0x%08x bottom 0x%08x/0x%08x (%u set%s); "
             "upload 0x%08x, texture 0x%08x (FCRAM, 512x512), targets 0x%08x/0x%08x..0x%08x\n", m_fb[0], m_fb[1],
             m_fb[2], m_fb[3], m_nsets, m_nsets > 1 ? "s" : "", GM_SCR_PA, tex_pa, GM_RT_TOP_PA, GM_RT_BOT_PA,
             GM_VRAM_HI);
    for (i = 0; i < 2 * m_nsets; i++)
        if (!fb_ok(m_fb[i % 2 * 2 + i / 2], i % 2 ? GM_BOT_FB_BYTES : GM_TOP_FB_BYTES)) {
            k_printf("[gpu] display mirror: framebuffer 0x%08x is not where the mirror can write it: CPU copy stays\n",
                     m_fb[i % 2 * 2 + i / 2]);
            m_set(GM_ST_FAILED, GM_WHY_LAYOUT);
            goto out;
        }
    if (pica_drv_alloc(&drv, 0, GM_SRC_W * GM_SRC_H * 4, 0, &src_pa) != PICA_OK ||
        pica_drv_alloc(&drv, 0, GM_TOP_FB_BYTES + GM_BOT_FB_BYTES + 2 * GUARD, 0, &out_pa) != PICA_OK) {
        k_printf("[gpu] display mirror: no arena memory for the self-test: CPU copy stays\n");
        m_set(GM_ST_FAILED, GM_WHY_NO_MEM);
        goto out;
    }
    t0 = m_ticks();
    v = pm_selftest(&mir, drv.va + (src_pa - drv.pa), src_pa, drv.va + (out_pa - drv.pa), out_pa, GUARD,
                    &bad0, fbad);
    GM->selftest_ticks = m_ticks() - t0;
    GM->selftest_bad = bad0;
    for (i = 0; i < 4; i++)
        GM->first_bad[i] = fbad[i];
    if (v == PM_SELFTEST_TIMEOUT) {
        /* the GPU (or the emulator's model of it) did not finish an
           operation: not a picture mismatch, say so */
        k_printf("[gpu] display mirror: self-test aborted, GPU operation %u did not finish: CPU copy stays\n",
                 fbad[0]);
        m_set(GM_ST_FAILED, GM_WHY_TIMEOUT);
        goto out;
    }
    if (v < 0) {
        k_printf("[gpu] display mirror: self-test FAILED (%u bytes differ as modelled): CPU copy stays\n", bad0);
        m_set(GM_ST_FAILED, GM_WHY_SELFTEST);
        goto out;
    }
    mir.variant = (uint32_t)v;
    GM->stereo_ok = ((const gm_blob_t *)mir.blob)->st_list_bytes[0] != 0;
    GM->st_frames = GM->flat_frames = GM->mirror_busy = 0;
    mir.frames = 0;
    mir.timeouts = 0;
    for (i = 0; i < 3; i++)
        mir.m_stage[i] = 0;
    mir.m_frame = mir.sum_frame = 0;
    GM->variant = (uint32_t)v;
    k_printf("[gpu] display mirror: self-test passed (variant %u) in %u us: the PICA200 copies the screen now "
             "(every %u. VBlank)\n", v, (GM->selftest_ticks * 1000u) / (TICK_HZ / 1000u), GM->vbl_div ? GM->vbl_div : 1);
    keep_tex = 1;
    m_set(GM_ST_ACTIVE, GM_WHY_NONE);
out:
    if (tex_pa && !keep_tex)
        pica_drv_free(&drv, 0, tex_pa);
    if (src_pa)
        pica_drv_free(&drv, 0, src_pa);
    if (out_pa)
        pica_drv_free(&drv, 0, out_pa);
    busy = 0;
}

static void m_stats(void)
{
    GM->frames = mir.frames;
    GM->timeouts = mir.timeouts;
    GM->t_in = mir.t_stage[0];
    GM->t_3d = mir.t_stage[1];
    GM->t_out = mir.t_stage[2];
    GM->t_frame = mir.t_frame;
    GM->m_in = mir.m_stage[0];
    GM->m_3d = mir.m_stage[1];
    GM->m_out = mir.m_stage[2];
    GM->m_frame = mir.m_frame;
    GM->sum_frame = mir.sum_frame;
}

/* a frame operation came back from pm_poll(): bookkeeping (IRQ context) */
static void m_after(int st)
{
    if (st == PM_BUSY)
        return;
    busy = 0;
    GM->mirror_busy = 0;
    m_stats();
    if (st == PM_DONE) {
        /* the ARM9 waits for a frame in the new mode before it moves the
           barrier or gives the screen back to the CPU copy */
        volatile uint32_t *c = mir.stereo ? &GM->st_n : &GM->fl_n;
        if (mir.stereo)
            GM->st_frames++;
        else
            GM->flat_frames++;
        c[0]++;                         /* st_n/st_sum/st_sum_3d or fl_* */
        c[1] += mir.t_frame;
        c[2] += mir.t_stage[1];
    }
    if (st == PM_TIMEOUT) {
        /* the GPU did not finish: stop mirroring for good; the ARM9 notices
           (frames stop) and copies with the CPU again */
        GM->first_bad[0] = mir.cur + 1;
        GM->first_bad[3] = mir.why_timeout;
        m_set(GM_ST_FAILED, GM_WHY_TIMEOUT);
        return;
    }
    gm_publish();
}

/* IRQ mode, IRQs masked, private stack (entry.S). id = GIC interrupt ID. */
PICA_HOT void shim_gpu_irq(uint32_t id)
{
    uint32_t t0, spin;

    if (id == PICA_IRQ_PDC0) {
        /* which framebuffer set each LCD shows: for the ARM9's CPU copy */
        uint32_t s0 = *(vu32 *)(GX_VA + 0x478), s1 = *(vu32 *)(GX_VA + 0x578);
        if (GM->pdc_magic != GM_PDC_MAGIC || GM->pdc_swap[0] != s0 || GM->pdc_swap[1] != s1) {
            GM->pdc_swap[0] = s0;
            GM->pdc_swap[1] = s1;
            GM->pdc_magic = GM_PDC_MAGIC;
            dc_clean(&GM->pdc_magic, 12);
        }
    }
    if (m_state != GM_ST_ACTIVE) {
        m_pending = 0;                  /* never leave GL waiting for a mirror that stopped */
        return;
    }
    t0 = m_ticks();
    spin = GM->spin_us * (TICK_HZ / 1000u) / 1000u;
    if (busy == 2) {                    /* a frame in flight: next operation if the last one is done */
        GM->steps++;
        m_after(pm_poll(&mir, spin));
        goto out;
    }
    if (m_pending) {
        if (busy)                       /* GL still running: its end frees the GPU */
            return;
        m_pending = 0;                  /* GPU free: the frame skipped earlier starts now */
    } else {
        if (id != PICA_IRQ_PDC0 || ++m_vbl < (GM->vbl_div ? GM->vbl_div : 1))
            return;
        m_vbl = 0;
    }
    gm_fetch();
    if (GM->ack_seq != GM->seq) {
        GM->ack_seq = GM->seq;          /* nothing built from an older line 0 is in flight */
        m_dirty = 1;                    /* a mode / stereo-k / want change needs a rebuild */
        gm_publish();
    }
    if (!GM->want)
        return;
    if (busy) {
        GM->busy_skips++;
        m_pending = 1;
        return;
    }
    if (!m_dirty) {
        /* nothing new since the last frame: the LCDs already hold the current
           picture. Skip the GPU work but keep the liveness counter moving so
           the ARM9 does not fall back to the CPU copy (docs/perf.md). */
        GM->idle_frames++;
        gm_publish();
        return;
    }
    busy = 2;
    if (!m_begin_frame())
        busy = 0;                       /* display off or a bad framebuffer */
    GM->steps++;
    m_after(pm_poll(&mir, spin));
out:
    GM->sum_irq += m_ticks() - t0;
}
