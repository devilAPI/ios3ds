/*
 * ios3ds GPU display mirror: the contract between the ARM9 monitor
 * (boot/arm9/display.c, main.c), the ARM11 kernel shim (kernel/shim/pica.c)
 * and the build-time command-list generator (tools/ios3ds-pica/mirror).
 * See docs/pica200.md section 9.
 *
 * What it does: the ARM9 used to copy iOS's 320x480 BGRA framebuffer into
 * the 3DS LCD framebuffers itself (display.c). With the GPU mirror the ARM11
 * shim does it on the PICA200 instead, started from the VBlank interrupt:
 *
 *   1. PPF display transfer  iOS framebuffer (VRAM, linear) -> GM_SCR_PA (tiled RGBA8, 320 wide)
 *      PPF texture copy      -> a 512x512 texture in the FCRAM arena (tile rows re-strided:
 *                            the PICA200 samples non-power-of-two textures wrongly, run13)
 *   2. P3D command list      two textured quads: GM_RT_TOP_PA (240x400) and
 *                            GM_RT_BOT_PA (240x320), nearest sampling at
 *                            exactly the CPU copy's pixel mapping (gm_map)
 *   3. PPF display transfer  render targets -> LCD framebuffers (RGBA8 -> RGB8)
 *
 * Only fixed-width C types: included by ARM9, ARM11 shim and host code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* ---- the mailbox: inside the shim region, PA 0x20840000 + 0x7F00 ----
   (shim region: code < +0x5F00, probes +0x5F00, PC ring +0x5F80, blkmbox
   +0x6000, inmbox +0x6200..+0x63FF, gmbox +0x7F00..+0x7FFF, bounce +0x8000) */
#define SHIM_GM_OFF       0x7F00u
#define GM_MAGIC          0x524D4D47u   /* 'GMMR' */
#define GM_PDC_MAGIC      0x53434450u   /* 'PDCS' */

enum {                                  /* gmbox_t.state */
    GM_ST_OFF = 0,          /* nothing tried yet (or the shim predates the mirror) */
    GM_ST_WAIT = 1,         /* waiting for a thread context to bring the GPU up */
    GM_ST_TEST = 2,         /* self-test running */
    GM_ST_ACTIVE = 3,       /* mirroring: the ARM9 may skip its CPU copy while `frames` moves */
    GM_ST_FAILED = 4,       /* gave up: see `reason`; the ARM9 copies with the CPU */
    GM_ST_DISABLED = 5,     /* boot.cfg gpu=0 or gpu_mirror=0 */
};

enum {                                  /* gmbox_t.reason */
    GM_WHY_NONE = 0,
    GM_WHY_CFG = 1,         /* switched off in boot.cfg */
    GM_WHY_NO_GPU = 2,      /* GPU bring-up failed (see the [pica] lines) */
    GM_WHY_NO_BLOB = 3,     /* gpumirror.bin missing or for another layout */
    GM_WHY_LAYOUT = 4,      /* LCD framebuffers overlap the mirror's VRAM, or bad addresses */
    GM_WHY_NO_MEM = 5,      /* no arena memory for the self-test */
    GM_WHY_SELFTEST = 6,    /* no variant reproduced the CPU copy exactly */
    GM_WHY_TIMEOUT = 7,     /* a live GPU operation did not finish (200 ms) */
    GM_WHY_GEOMETRY = 8,    /* iOS framebuffer is not 320x480 BGRA in VRAM */
};

typedef struct {
    /* line 0: ARM9 (FCRAM is uncached on the ARM9; the ARM11 invalidates before reading) */
    uint32_t magic;         /* GM_MAGIC once the ARM9 runs this protocol */
    uint32_t seq;           /* bumped after every change of line 0 */
    uint32_t want;          /* 1 = iOS display is on, mirror it */
    uint32_t mode;          /* SCREEN_TOP / SCREEN_BOTH / SCREEN_BOTTOM (display.h) */
    uint32_t top_fb[2];     /* LCD framebuffers as Luma set them up (BGR8, 240x400) */
    uint32_t bot_fb[2];     /* (BGR8, 240x320) */
    /* line 1: ARM9, boot.cfg; written once before XNU starts */
    uint32_t gpu_enable;    /* boot.cfg gpu=     0: the shim never touches the GPU (GL too) */
    uint32_t mirror_enable; /* boot.cfg gpu_mirror= 0: ARM9 CPU copy only */
    uint32_t vbl_div;       /* start a frame at most every Nth VBlank (>= 1) */
    uint32_t spin_us;       /* IRQ-time busy-wait budget per step (0 = never wait) */
    uint32_t gpu_clk;       /* boot.cfg gpu_clk=  1: set GX_GPU_CLK = 0x70100 at bring-up if it reads 0 */
    uint32_t gpu_diag;      /* boot.cfg gpu_diag= 1: staged GPU diagnostics at bring-up ([gpu-diag] lines) */
    uint32_t ca_gpu;        /* boot.cfg ca_gpu=   1: CoreAnimation may composite on the GPU (docs/gpu.md 7) */
    uint32_t _r1[1];
    /* line 2: ARM11 status (cleaned to RAM after every update) */
    uint32_t state;         /* GM_ST_* */
    uint32_t reason;        /* GM_WHY_* when FAILED / DISABLED */
    uint32_t ack_seq;       /* no frame built from a line 0 older than this is in flight */
    uint32_t frames;        /* frames completed */
    uint32_t busy_skips;    /* VBlanks skipped because GL owned the GPU */
    uint32_t timeouts;
    uint32_t variant;       /* self-test variant in use (GM_VAR_*) */
    uint32_t selftest_bad;  /* bytes that differed in the default variant's self-test */
    /* line 3: ARM11 timing, MPCore watchdog ticks (1047311 Hz, ~0.955 us) */
    uint32_t t_in, t_3d, t_out, t_frame;    /* last frame: per stage, start to end */
    uint32_t m_in, m_3d, m_out, m_frame;    /* maxima */
    /* line 4: ARM11 */
    uint32_t sum_frame;     /* sum of t_frame (wraps; the ARM9 averages deltas) */
    uint32_t sum_irq;       /* ARM11 ticks spent in the IRQ-time stepping code */
    uint32_t steps;         /* IRQ-time steps that touched the GPU */
    uint32_t selftest_ticks;/* duration of the whole self-test */
    uint32_t first_bad[4];  /* default variant: first mismatch {offset, expected, got, ops} */
    /* line 5: ARM11, every VBlank: which framebuffer set each LCD shows
       (PDC0/PDC1 +0x78 "swap": bit 0 next, bit 4 current; the ARM9 cannot
       read the GPU block itself). The CPU copy writes only those. */
    uint32_t pdc_magic;     /* GM_PDC_MAGIC once published */
    uint32_t pdc_swap[2];   /* top, bottom */
    /* stereo 3D mode switch (docs/stereo3d.md): ARM9 sets on + bumps seq,
       ARM11 applies the PDC0 2D/3D preset and acknowledges here. The ARM9
       enables the parallax barrier only after the ack. */
    uint32_t stereo_on;
    uint32_t stereo_seq;
    uint32_t stereo_ack_seq;
    /* stereo_k: eye shift (1..4) used by the GPU mirror's stereo list.
       st_frames / flat_frames: frames finished in each mode after the latest
       PDC ack, so the ARM9 only opens the barrier once both eyes are there. */
    uint32_t stereo_k;
    uint32_t st_frames;
    uint32_t flat_frames;
    /* lines 6-7: ARM11 mirror state for the ARM9 */
    uint32_t mirror_busy;   /* 1 while a mirror frame is in flight */
    uint32_t stereo_ok;     /* 1: gpumirror.bin has the stereo lists (v4) */
    uint32_t idle_frames;   /* VBlanks skipped because nothing changed: the ARM9
                               counts these as liveness too (the LCDs already
                               hold the current picture) */
    /* per-mode GPU cost (re-log 127): completed frames and the sums of their
       whole-frame and draw-stage times (watchdog ticks, wrapping), so the
       ARM9 can print 3D and 2D frame costs side by side */
    uint32_t st_n, st_sum, st_sum_3d;
    uint32_t fl_n, fl_sum, fl_sum_3d;
#ifdef IOS3DS_IDLE
    /* Opt-in idle policy only (scripts/mkboot.py IOS3DS_IDLE=1). Spare words
       in the ARM9-owned line: the shim's GPU part restores PDN_GPU_CNT if a
       GL request arrives while the ARM9 had gated the clock for a dark screen. */
    uint32_t idle_gate;     /* 1 = clock gated by the ARM9 */
    uint32_t idle_saved;    /* PDN_GPU_CNT value to restore */
    uint32_t _r6[4];
#else
    uint32_t _r6[6];
#endif
} gmbox_t;
_Static_assert(sizeof(gmbox_t) == 256, "gmbox_t layout");

/* self-test variants (bits): hardware facts the model cannot settle */
#define GM_VAR_SAMPLE_HALF  1u      /* nearest sampling rounds instead of truncating: texcoords shifted by -1/2 texel */
#define GM_VAR_IN_FLIP      2u      /* texture rows are stored the other way round: upload with PPF flip */
#define GM_VAR_OUT_FLIP     4u      /* render target rows the other way round: read back with PPF flip */
#define GM_VARIANTS         8u

/* ---- GPU memory (VRAM bank B, above Luma's second framebuffer set) ----
   0x18000000-0x182FFFFF  /vram: iOS framebuffer (AppleM2CLCD window 1)
   0x18300000 / 0x18346500  Luma: top / bottom framebuffer, set 0
   0x18400000 / 0x18446500  Luma: top / bottom framebuffer, set 1 (ends 0x1847EA00)
   0x18480000  upload    320x480 RGBA8, 8x8 tiled (0x96000); copied on into
               the 512x512 texture, which lives in the FCRAM arena (1 MiB)
   0x18516000  RT top    240x400 RGBA8 tiled     (0x5DC00)
   0x18573C00  RT bottom 240x320 RGBA8 tiled     (0x4B000), ends 0x185BEC00
   The shim checks the framebuffer addresses the ARM9 reports against this. */
#define GM_SRC_W          320u
#define GM_SRC_H          480u
#define GM_SCR_PA         0x18480000u   /* upload target (tiled, 320 wide) */
#define GM_TEX_W          512u          /* the texture the quads sample: power of two */
#define GM_TEX_H          512u
#define GM_TEX_BYTES      (GM_TEX_W * GM_TEX_H * 4u)
#define GM_TEX_PLACEHOLDER 0x2FF00000u  /* genmirror's texture address, patched at run time */
#define GM_RT_W           240u
#define GM_RT_TOP_H       400u
#define GM_RT_BOT_H       320u
#define GM_RT_TOP_PA      (GM_SCR_PA + GM_SRC_W * GM_SRC_H * 4u)
#define GM_RT_BOT_PA      (GM_RT_TOP_PA + GM_RT_W * GM_RT_TOP_H * 4u)
#define GM_VRAM_LO        GM_SCR_PA
#define GM_VRAM_HI        (GM_RT_BOT_PA + GM_RT_W * GM_RT_BOT_H * 4u)
#define GM_IOS_VRAM_HI    0x18300000u   /* end of /vram */
#define GM_TOP_FB_BYTES   (GM_RT_W * GM_RT_TOP_H * 3u)
#define GM_BOT_FB_BYTES   (GM_RT_W * GM_RT_BOT_H * 3u)

/* ---- gpu.bin: the shim's GPU part and the mirror's command lists --------
   One file, loaded by the ARM9 (boot.cfg load=) at a fixed FCRAM address
   between the XNU log ring and topOfKernelData, in the same 1 MiB kernel
   section as the shim (XNU never uses it; the network mailbox is at
   0x208A0000, audio PCM at 0x208C0000):
     0x208B0000  GPU part of the shim (kernel/shim/pica.c, pica_drv.c,
                 pica_mirror.c; code, data and zeroed bss), at most 32 KiB.
                 It starts with gpu_part_hdr; the main shim only calls into
                 it when that magic is there (kernel/shim/gpu_glue.c).
     0x208B8000  gpumirror blob (tools/ios3ds-pica/mirror/genmirror.c): the
                 command lists and vertex arrays; the GPU reads them in place. */
#define GPU_PART_PA       0x208B0000u
#ifndef IOS3DS_GPU_PART_VA
#define IOS3DS_GPU_PART_VA 0x808B0000u
#endif
#define GPU_PART_VA       IOS3DS_GPU_PART_VA
#define GPU_PART_SIZE     0x8000u
#define GPU_PART_MAGIC    0x30555047u   /* 'GPU0' */
#define GM_BLOB_PA        0x208B8000u
#ifndef IOS3DS_GM_BLOB_VA
#define IOS3DS_GM_BLOB_VA 0x808B8000u
#endif
#define GM_BLOB_VA        IOS3DS_GM_BLOB_VA
#define GM_BLOB_MAX       0x8000u
#define GM_BLOB_MAGIC     0x424D4D47u   /* 'GMMB' */
#define GM_BLOB_VERSION   4u    /* v4: stereo lists + per-class shift patches */

/* GPU diagnostics (tools/ios3ds-pica/src/pica_diag.c): small VRAM scratch
   areas above the mirror's buffers, in the window the shim maps uncached */
#define GD_VRAM_PA        0x185C0000u   /* .. 0x185D4000 */
#define GD_FILL_PA        0x185C0000u   /* 4 KiB fill target */
#define GD_RT_PA          0x185C8000u   /* 64x64 RGBA8 render target */
#define GD_TEX_PA         0x185CC000u   /* up to 64x64 RGBA8 texture */
#define GD_PPF_PA         0x185D0000u   /* 64x64 RGBA8 tiled */
#define GD_RT_W           64u
#define GD_NPOT_W         48u           /* NPOT texture test: 48x40 */
#define GD_NPOT_H         40u
#define GD_SOLID_TEV      0x87654321u   /* TEV constant R=0x21 G=0x43 B=0x65 A=0x87 */
#define GD_SOLID_WORD     0x21436587u   /* ... as an RGBA8 colour-buffer word */

typedef struct {
    uint32_t magic, version, blob_pa, size;
    uint32_t scr_pa, rt_top_pa, rt_bot_pa, src_wh;      /* what the lists were built for */
    uint32_t list_off[2], list_bytes[2];                /* [0] exact, [1] GM_VAR_SAMPLE_HALF */
    uint32_t diag_off[3], diag_bytes[3];                /* diagnostics: solid, texture 64x64, texture 48x40 */
    uint32_t diag_patch;                                /* blob offset of the solid list's colour-buffer
                                                           address word (PA >> 3), to render elsewhere */
    uint32_t tex_patch[2];                              /* blob offsets of list[n]'s texture address word */
    /* v4: two-eye mirror lists, one per sampling variant. Each list draws a
       baseline pass (delta 0) and three class passes per eye (BG, FRONT,
       ALERT) selected with the PICA200's alpha test on the *unshifted* stamp.
       st_shift_off[v][eye*3+class] points at the uniform word holding the
       shifted texture coordinate; the shim patches it from st_shift_tab for
       the current slider k (no floating point in the kernel). */
    uint32_t st_list_off[2], st_list_bytes[2];
    uint32_t st_shift_off[2][6];
    uint32_t st_shift_tab;                              /* blob offset: [k-1][6] IEEE float words,
                                                           in st_shift_off order */
    uint32_t st_tex_patch[2][4];                        /* texture-address words in each stereo list
                                                           (two units x two program emissions) */
    uint32_t st_pad_off;                                /* blob offset: [60][16] uint32 tiled word
                                                           offsets: 8 (source edge pixel, dest tile
                                                           base) pairs for the stereo texture padding */
    uint32_t _r[1];
} gm_blob_t;
_Static_assert(sizeof(gm_blob_t) == 192, "gm_blob_t layout");

/*
 * The pixel mapping of the CPU copy (boot/arm9/display.c), as one function:
 * LCD pixel -> iOS pixel. The 3DS framebuffers are column-major: framebuffer
 * "row" j (= screen column from the left) holds 240 pixels, i = 0 at the
 * bottom of the screen. In those terms both copies are plain scalings:
 *   top    (j 0..399): j 20..379 <- iOS row (j-20)*4/3, i <- iOS column i*4/3
 *   bottom (j 0..319): iOS row j*3/2, i 14..226 <- iOS column (3i-40)/2
 * Returns 0 for black margin pixels.
 */
static inline int gm_map(int bottom, uint32_t i, uint32_t j, uint32_t *x, uint32_t *y)
{
    if (!bottom) {
        if (j < 20 || j >= 380)
            return 0;
        *x = i * 4 / 3;
        *y = (j - 20) * 4 / 3;
        return 1;
    }
    if (i * 3 < 40 || (i * 3 - 40) / 2 >= GM_SRC_W)
        return 0;
    *x = (i * 3 - 40) / 2;
    *y = j * 3 / 2;
    return 1;
}
