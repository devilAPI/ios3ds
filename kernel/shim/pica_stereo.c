/*
 * ios3ds shim GPU part: PDC0 (top LCD) 2D/3D switching for the experimental
 * stereoscopic home screen (docs/stereo3d.md, boot/common/stereo.h).
 *
 * The ARM9 cannot reach the GPU external registers (3dbrew "GPU/External
 * Registers": PDC0 at PA 0x10400400), but it owns the parallax barrier
 * (3dbrew "LCD Registers": 0x10202000/04). It hands us the mode through the
 * gmbox_t line 5, we save Luma's 2D preset once, write the 3D preset and
 * acknowledge; the ARM9 only turns the barrier on after the ack.
 *
 * Register values: 3dbrew GPU/External Registers (PDC0 offsets, AB
 * interlacing = bits 5-4 of +0x70, right-eye framebuffers +0x94/+0x98) and
 * the 3D preset from libn3ds's pdc_presets.h (v_total 827, borders 802,
 * v_irq 802..806, picture 240x400). These are register facts, not code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "gpu_mirror.h"
#include "stereo.h"

#define ST_GX_VA      0xBFE00000u              /* entry.S: PA 0x10400000 */
/* PDC0 sits at +0x400 in the GX block (entry.S PDC0). The default gpu.bin
   was built with base 0, so its "PDC0" writes hit 0x10400024..0x10400098
   instead (found in QEMU: PDC0 +0x70 never left 2D, re-log NEXT stereo 4).
   That gpu.bin must stay byte-identical (the 4.2.1 hashes), so the fix ships
   as a second GPU part, gpu3d.bin: scripts/mkboot.py copies gpu.bin and
   moves this file's one ST_GX_VA literal to 0xBFE00400 (PDC0), so no other
   byte or address changes. The ARM9 loads it only with boot.cfg stereo3d=1. */
#define ST_REG(off)   (*(volatile uint32_t *)(ST_GX_VA + (off)))

/* the mirror only runs on a GL present or a config change; a PDC 2D/3D
   switch is one such config change (the eyes must be rebuilt even when the
   CA frame did not move, e.g. the slider on an idle home screen) */
extern volatile uint32_t m_dirty;

/* 3D values from libn3ds's top-screen stereo preset. The 2D values are
   whatever Luma left there and are saved on the first switch. */
static const uint8_t st_off[] = { 0x24, 0x2C, 0x30, 0x34, 0x40, 0x5C, 0x64 };
static const uint16_t st_lo[] = { 827, 802, 802, 802, 802, 240, 2 };
static const uint16_t st_hi[] = { 0, 0, 0, 0, 806, 400, 802 };
static uint32_t st_saved[sizeof st_off / sizeof st_off[0]];
static uint32_t st_saved_fmt, st_saved_left[2], st_saved_right[2];
static uint32_t st_ack, st_init;

static uint32_t st_rd(uint32_t off) { return ST_REG(off); }
static void st_wr(uint32_t off, uint32_t v) { ST_REG(off) = v; }

/* One cache helper for the mailbox line (invalidate before read, clean after
   write) keeps the GPU part inside its 32 KiB budget. */
static void st_cache(volatile uint32_t *p, int clean)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + 12;
    for (; a < end; a += 32) {
        if (clean)
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
        else
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    }
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* Called in thread context from shim_gpu_thread_poll(), before its mirror
   checks: PDC switching is needed even though stereo3d disables the mirror. */
void shim_stereo_poll(void)
{
    volatile gmbox_t *gm = (volatile gmbox_t *)(SHIM_VA + SHIM_GM_OFF);

    if (gm->magic != GM_MAGIC || !gm->gpu_enable)
        return;
    st_cache(&gm->stereo_on, 0);        /* line 5: ARM9-owned request + our ack */
    uint32_t seq = gm->stereo_seq;
    if (!seq || seq == st_ack)
        return;
    uint32_t on = gm->stereo_on;
    if (on && !st_init) {
        for (uint32_t i = 0; i < sizeof st_off / sizeof st_off[0]; i++)
            st_saved[i] = st_rd(st_off[i]);
        st_saved_fmt = st_rd(ST_PDC_FMT_OFF);
        st_saved_left[0] = st_rd(0x68);
        st_saved_left[1] = st_rd(0x6C);
        st_saved_right[0] = st_rd(0x94);
        st_saved_right[1] = st_rd(0x98);
        st_init = 1;
    }
    if (on) {
        for (uint32_t i = 0; i < sizeof st_off / sizeof st_off[0]; i++)
            st_wr(st_off[i], (uint32_t)st_hi[i] << 16 | st_lo[i]);
        /* both eyes point at the two LCD sets the ARM9 just wrote */
        uint32_t left = gm->top_fb[0], right = gm->top_fb[1];
        st_wr(0x68, left);
        st_wr(0x6C, left);
        st_wr(0x94, right);
        st_wr(0x98, right);
        /* AB interlace, no line doubling: 3D mode (GSP's stereo format) */
        st_wr(ST_PDC_FMT_OFF, (st_saved_fmt & ~ST_PDC_FMT_MASK) | ST_PDC_FMT_INTER);
    } else if (st_init) {
        for (uint32_t i = 0; i < sizeof st_off / sizeof st_off[0]; i++)
            st_wr(st_off[i], st_saved[i]);
        st_wr(0x68, st_saved_left[0]);
        st_wr(0x6C, st_saved_left[1]);
        st_wr(0x94, st_saved_right[0]);
        st_wr(0x98, st_saved_right[1]);
        st_wr(ST_PDC_FMT_OFF, st_saved_fmt);
    }
    st_ack = seq;
    gm->stereo_ack_seq = seq;
    m_dirty = 1;                        /* rebuild the eyes with the new mode */
    st_cache(&gm->stereo_on, 1);

}
