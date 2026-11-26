/*
 * ios3ds shim GPU part (opt-in, built only with IOS3DS_IDLE=1):
 *
 *  1. shim_idle_clock_apply() runs on every MPCore timer IRQ. The ARM9 writes
 *     an 'IDL' request into shim+0x6C before changing PDN_LGR_SOCMODE from
 *     804 MHz (mode 5, PERIPHCLK 402 MHz) to 536 MHz (mode 3, PERIPHCLK 268 MHz)
 *     or back. XNU's time base must stay at 1047311 Hz across the switch, so
 *     this changes the MPCore timer prescaler 191 -> 255 (or back) and the
 *     shim's tick shift 1 -> 0 (or back) and rescales the live counts.
 *  2. shim_idle_gl_wake() restores PDN_GPU_CNT if a GL request arrives while
 *     the ARM9 had clock-gated the PICA200 for a dark screen.
 *
 * Both are no-ops on an Old 3DS and in a default build. The file is named
 * pica_idle.c so link.ld puts it in gpu.bin, leaving the main shim's 52 free
 * bytes alone. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "gpu_mirror.h"
#include "kpi.h"

#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define k_printf KPT(IOS3DS_KPI_PRINTF, void, const char *, ...)

#define PRIV_VA      0xBFF00000u
#define PT_LOAD      0x600u
#define PT_COUNT     0x604u
#define PT_CTRL      0x608u
#define PT_ISR       0x60Cu
#define WD_LOAD      0x620u
#define WD_COUNT     0x624u
#define WD_CTRL      0x628u
#define SHIM_IDLE_REQ_OFF 0x6Cu
#define IDLE_REQ_MAGIC  0x49440000u
#define IDLE_REQ_REFRESH (1u << 12)      /* ARM9 acked PDN_SOCMODE: update the timer rate */
#define PDN_PA           0x10141000u
#define GPU_CNT_OFF      0x200u          /* PDN_GPU_CNT = PDN_PA + 0x200 */

extern uint32_t cfg_n3ds;             /* entry.S, shim +0x0C */
extern uint32_t shim_pt_ctrl_run;     /* entry.S: cached private-timer control */

/* PDN is not in the shim's static device map: pica.c maps it with ml_io_map
   at bring-up. The GL wake can arrive before that bring-up, so map it here
   too. Using the raw physical address faults (kernel data abort 0x101411c0). */
static volatile uint32_t *idle_pdn(void)
{
    static volatile uint32_t *pdn;
    if (!pdn)
        pdn = KPT(IOS3DS_KPI_ML_IO_MAP, volatile uint32_t *, uint32_t, uint32_t)(PDN_PA, 0x1000);
    return pdn;
}

static inline void clean_line(void *p)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");
}

static inline void inv_line(void *p)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory");
}

void shim_idle_clock_apply(void)
{
    volatile uint32_t *reqp = (volatile uint32_t *)(SHIM_VA + SHIM_IDLE_REQ_OFF);
    uint32_t req, cur, target, oldpre, newpre, oldshift, newshift;
    uint32_t up, newup;
    bool halve, double_;

    inv_line((void *)reqp);
    req = *reqp;
    if ((req & 0xFFFF0000u) != IDLE_REQ_MAGIC) return;
    if (req & IDLE_REQ_REFRESH) {
        /* PDN_LGR_SOCMODE has changed PERIPHCLK. Rewriting the control words
           with the prescaler already in cfg_n3ds makes the MPCore pick up the
           new rate without disturbing the counts (no reset, same enable bits). */
        volatile uint32_t *p = (volatile uint32_t *)PRIV_VA;
        uint32_t pre = (cfg_n3ds & 0xFFu) ? (cfg_n3ds & 0xFFu) : 127u;
        p[WD_CTRL / 4] = (pre << 8) | 2u | 1u;
        p[PT_CTRL / 4] = (pre << 8) | 4u | 1u;
        shim_pt_ctrl_run = p[PT_CTRL / 4];
        clean_line((void *)&shim_pt_ctrl_run);
        *reqp = req & ~IDLE_REQ_REFRESH;      /* ack for the ARM9 */
        clean_line((void *)reqp);
        k_printf("[pica] idle clock: MPCore control rewritten for the new PERIPHCLK\n");
        return;
    }
    cur = cfg_n3ds;
    target = (cur & ~0xFFFu) | (req & 0xFFFu);
    if (cur == target) return;
    oldpre = (cur & 0xFFu) ? (cur & 0xFFu) : 127u;
    newpre = (target & 0xFFu) ? (target & 0xFFu) : 127u;
    oldshift = (cur >> 8) & 0xFu;
    newshift = (target >> 8) & 0xFu;
    halve = oldpre == 191u && oldshift == 1u && newpre == 255u && newshift == 0u;
    double_ = oldpre == 255u && oldshift == 0u && newpre == 191u && newshift == 1u;
    if (!halve && !double_) return;              /* only the two known New 3DS modes */

    volatile uint32_t *priv = (volatile uint32_t *)PRIV_VA;
    /* While the ARM9 writes PDN_LGR_SOCMODE the CPU clock has not changed yet.
       Rescale the free-running watchdog up-count and the private timer's down
       count so that (count >> shift) is the same number of timebase ticks. */
    up = ~priv[WD_COUNT / 4];
    newup = halve ? up >> 1 : (up > 0x7FFFFFFFu ? 0xFFFFFFFFu : up << 1);
    priv[WD_CTRL / 4] = 0;
    priv[WD_LOAD / 4] = ~newup;
    priv[WD_CTRL / 4] = (newpre << 8) | 2u | 1u;   /* auto-reload + enable, timer mode */
    *(volatile uint32_t *)(SHIM_VA + 0x48) = newup;  /* st_last_up, entry.S */

    uint32_t down = priv[PT_COUNT / 4];
    uint32_t newdown = halve ? down >> 1 : (down > 0x7FFFFFFFu ? 0xFFFFFFFFu : down << 1);
    priv[PT_CTRL / 4] = 0;
    priv[PT_ISR / 4] = 1;
    priv[PT_LOAD / 4] = newdown;
    shim_pt_ctrl_run = (newpre << 8) | 4u | 1u;   /* one-shot + IT enable + enable */
    priv[PT_CTRL / 4] = shim_pt_ctrl_run;
    cfg_n3ds = target;
    clean_line((void *)&cfg_n3ds);
    clean_line((void *)&shim_pt_ctrl_run);
    clean_line((void *)(SHIM_VA + 0x40));          /* +0x40..+0x5F for the ARM9 */
    k_printf("[pica] idle clock: timer prescaler %u -> %u, tick shift %u -> %u\n",
             oldpre, newpre, oldshift, newshift);
}

void shim_idle_gl_wake(void)
{
    volatile gmbox_t *gm = (volatile gmbox_t *)(SHIM_VA + SHIM_GM_OFF);
    volatile uint32_t *pdn;
    if (!gm->idle_gate) return;
    pdn = idle_pdn();
    if (!pdn) return;                  /* keep the request for the next GL call */
    pdn[GPU_CNT_OFF / 4] = gm->idle_saved ? gm->idle_saved : 0x1007Fu;
    gm->idle_gate = 0;
    gm->idle_saved = 0;
    clean_line((void *)&gm->idle_gate);
    k_printf("[pica] GPU clock restored for a GL request (PDN_GPU_CNT %08x)\n",
             (unsigned)pdn[GPU_CNT_OFF / 4]);
}
