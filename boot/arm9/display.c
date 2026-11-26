/*
 * Mirror iOS's framebuffer onto the 3DS top screen.
 * AppleM2CLCD (running against a fake S5L8720 CLCD register page) keeps the
 * scan-out state in that page: +0x00 bit0 = on, +0x24 = window-1 base PA,
 * +0x28 = width in pixels (stride = x4), +0x30 = (w<<16)|h, 32 bpp BGRA.
 * The 320x480 portrait image is rotated and scaled by 3/4 to 360x240 so it
 * fills the top LCD's height (turn the 3DS 90 degrees clockwise, like a book).
 * The bottom screen can show it too, at 2/3 scale: exactly the panel -> iOS
 * mapping of the input daemon (tools/ios3ds-input map_touch, flipx), so a
 * tap lands on what is drawn under the finger. Modes: display.h SCREEN_*.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "display.h"
#include "util.h"
#include "log.h"
#include "blksrv.h"
#include "gpumirror.h"
#include "mcu.h"
#include "stereo.h"

#define SCR_W 400
#define SCR_H 240

static u8 *top_fb[2], *bot_fb[2];
static int mode = SCREEN_BOTH;
static u32 snapshot[0x100];
static bool armed, touched;

/* Experimental stereo copy (docs/stereo3d.md): boot.cfg stereo3d and the
   current 3D slider. With the flag off this code is dormant and the normal
   copy below runs exactly as before. */
static bool stereo_cfg, stereo_hw, stereo_no_shim, stereo_warned;
static bool stereo_want;                    /* a stamped home frame with the slider up */
static u32 stereo_slider;
static u32 stereo_depth = STEREO_DEPTH_DEFAULT; /* boot.cfg stereo_depth, 0..100 % */
/* Transitions (folder, app zoom, switcher, page swipe) mix stamped home
   frames with flat or half-drawn ones. Flipping PDC0/the barrier on every
   probe would flicker, so the decision has hysteresis: 3D starts after
   ST_ON_PROBES home probes in a row and stays on through up to
   ST_HOLD_PROBES non-home probes (~0.5 s at the 15/s copy rate). While held,
   the two-eye list draws the flat frame: no class pass matches alpha 0, so
   both eyes get the same picture (a graceful flat frame, not a stale eye). */
#define ST_ON_PROBES   2
#define ST_HOLD_PROBES 8
static u32 st_home_run, st_flat_run, st_holds;

static int stereo_k(void)
{
    return stereo_depth_k(stereo_slider, stereo_depth);
}

void display_set_stereo_depth(u32 depth)
{
    stereo_depth = depth > 100 ? 100 : depth;
}

/* Rebuilding both eyes is several times the cost of the normal copy: the
   per-pixel class lookup and two shifted samples are done for every output
   pixel. Cache one entry per screen column (the source row it was built
   from) and rebuild a column only when that row changed. A bounded number
   of columns is rebuilt per blit, so a frame that changed everywhere (a
   page swipe, or returning to the home screen) costs at most the same as a
   handful of normal columns instead of one very long ARM9 copy. */
#define ST_ROWS   512                 /* source rows, 0..h-1 (h <= 511) */
#define ST_COL_CAP 120                /* columns rebuilt per blit (360/3) */
static u32 st_hash[ST_ROWS];
static u8 st_hash_ok[ST_ROWS];
static int st_cache_k = -1;
static u32 st_rebuilt, st_skipped;

static void stereo_cache_reset(void)
{
    memset(st_hash_ok, 0, sizeof st_hash_ok);
}

static void stereo_barrier(bool on);

void display_set_stereo(bool on, u32 slider)
{
    bool was_cfg = stereo_cfg;
    stereo_cfg = on && top_fb[0] && top_fb[1] && top_fb[0] != top_fb[1];
    if (was_cfg != stereo_cfg || stereo_slider != slider)
        stereo_cache_reset();               /* a different k shifts every column */
    stereo_slider = slider;
    gm_set_shift((u32)stereo_k());
    if (!on || !stereo_cfg || slider < STEREO_MIN_SLIDER)
        stereo_no_shim = false;                 /* retry a failed switch next time */
    if (!stereo_cfg || stereo_k() <= 0) {
        st_home_run = st_flat_run = 0;
        bool was_want = stereo_want;
        stereo_want = false;
        if (stereo_hw || was_want) {
            gm_set_stereo(false);              /* 2D preset first, barrier second */
            stereo_barrier(false);
            stereo_hw = false;
            logp("[monitor] stereo 3D: back to 2D, parallax barrier off\n");
        }
    }
}

/* 3dbrew "LCD Registers": parallax barrier PWM 0x10202004 = 0x0A390A39 and
   enable 0x10202000 = 0x00010001 (libn3ds LCD_setParallaxBarrier); 0 = off.
   Written only after the ARM11 has acknowledged the PDC0 2D/3D switch. */
static void stereo_barrier(bool on)
{
    vu32 *cnt = (vu32 *)(uintptr_t)ST_BARRIER_PA;
    if (on) {
        *(vu32 *)(uintptr_t)ST_BARRIER_PWM_PA = ST_BARRIER_PWM;
        *cnt = ST_BARRIER_ON;
    } else {
        *cnt = 0;
    }
    /* The MCU firmware normally lights this LED when Horizon enables the
       barrier. We drive the registers directly, so do it here as well. */
    mcu_set_3d_led(on);
}

/* The stamp is an alpha-only GL pass. The iOS surface is 32 bpp BGRA, but
   the PICA present path's byte order has been described both ways (docs/gpu.md
   says B,G,R,A in memory, the original QEMU model used A,B,G,R), so accept
   the class magic in either end of the word. Colour bytes are masked out
   when the eye images are packed; the stamp never reaches the LCD. */
/* A 256-byte table replaces the switch in the hot per-pixel loop: 0 means
   "not a class stamp", otherwise class + 1 (so STC_FLAT is 1). */
static const u8 st_class_lut[256] = {
    [SA_FLAT]  = STC_FLAT + 1,
    [SA_BG]    = STC_BG + 1,
    [SA_ICONS] = STC_ICONS + 1,
    [SA_FRONT] = STC_FRONT + 1,
    [SA_ALERT] = STC_ALERT + 1,
};

static int stereo_px_class(u32 px)
{
    int cls = st_class_lut[(u8)(px >> 24)];
    if (!cls) cls = st_class_lut[(u8)px];
    return cls ? cls - 1 : STC_FLAT;
}

/* Sample the alpha plane of the iOS frame. The GL engine stamps a depth
   class into it on home frames only; every other frame (apps, lock screen,
   software compositor) has no class map and must stay flat. */
static bool stereo_is_home(const u8 *src, u32 w, u32 h, u32 stride)
{
    const u32 *px;
    unsigned seen = 0;
    u32 stamped = 0;
    if (w < 32 || h < 32 || stride < w * 4 || ((u32)(uintptr_t)src & 3))
        return false;
    px = (const u32 *)src;
    for (u32 j = 0; j < 32; j++) {
        u32 y = j * (h - 1) / 31;
        for (u32 i = 0; i < 32; i++) {
            u32 x = i * (w - 1) / 31;
            int cls = stereo_px_class(px[y * (stride / 4) + x]);
            if (cls > STC_FLAT) {
                seen |= 1u << cls;
                stamped++;
            }
        }
    }
    /* CA's wipe leaves alpha 0 everywhere on non-home frames; on a stamped
       home frame at least two depth classes cover a useful part of the grid.
       Do not demand every sample be a class magic: untouched pixels keep
       whatever alpha the compositor produced. */
    return stamped >= 16 && (seen & (seen - 1)) != 0;
}

/* One decision per copy interval. The GL engine stamps classes into the
   display surface's alpha byte whenever stereo3d=1; a stamped home frame
   with the slider up selects the two-eye path. With the mirror running the
   PICA200 builds both eyes (pica_mirror.c); without it the ARM9 copy does.
   Everything else - an app's flat frame, slider down, flag off - leaves the
   mirror drawing the ordinary one-eye picture and the barrier closed. */
bool display_stereo_probe(u32 regs_pa)
{
    bool want = false;
    if (stereo_cfg && stereo_k() > 0 && !stereo_no_shim &&
        mode != SCREEN_BOTTOM && top_fb[1] && top_fb[1] != top_fb[0] && regs_pa) {
        vu32 *r = (vu32 *)regs_pa;
        u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
        u32 stride = r[0x28 / 4] * 4;
        want = w && h && stride && stereo_is_home((const u8 *)(uintptr_t)r[0x24 / 4], w, h, stride);
        if (want) {
            st_flat_run = 0;
            if (st_home_run < ST_ON_PROBES)
                st_home_run++;
            if (!stereo_want && st_home_run < ST_ON_PROBES)
                want = false;               /* not yet: one home probe in a transition */
        } else {
            st_home_run = 0;
            if (stereo_want && ++st_flat_run <= ST_HOLD_PROBES) {
                want = true;                /* hold 3D through a short flat stretch */
                if (st_flat_run == 1)
                    st_holds++;
            }
        }
    } else {
        st_home_run = st_flat_run = 0;
    }
    if (want != stereo_want) {
        if (want) {
            if (!gm_set_stereo(true)) {
                if (!stereo_warned) {
                    log_warn("[monitor] stereo 3D: no shim to switch the top LCD into 3D mode; staying in 2D\n");
                    stereo_warned = true;
                }
                stereo_no_shim = true;
                stereo_cache_reset();       /* the flat copy replaced both eye images */
                want = false;
            }
        } else {
            gm_set_stereo(false);
        }
        stereo_want = want;
    }
    if (stereo_want && !stereo_hw) {
        stereo_barrier(true);
        stereo_hw = true;
        log_ok("[monitor] stereo 3D: both eyes from the stamped home frame, parallax barrier on "
               "(slider %u, depth %u%%, shift %d px)\n", stereo_slider, stereo_depth, stereo_k());
    } else if (!stereo_want && stereo_hw) {
        stereo_barrier(false);
        stereo_hw = false;
        logp("[monitor] stereo 3D: top screen back to 2D, parallax barrier off\n");
    }
    return stereo_want;
}

/* remember the register page as loaded; the mirror starts once the kernel
   driver has written to it (i.e. AppleM2CLCD is really driving "the panel") */
void display_arm(u32 regs_pa)
{
    if (!regs_pa) return;
    for (u32 i = 0; i < 0x100; i++) snapshot[i] = ((vu32 *)regs_pa)[i];
    armed = true;
}

void display_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1)
{
    top_fb[0] = top0;
    top_fb[1] = top1;
    bot_fb[0] = bot0;
    bot_fb[1] = bot1 != bot0 ? bot1 : NULL;
    stereo_cache_reset();
}

/* A screen that starts showing iOS is cleared first: the copy never writes
   the top screen's 20-column side margins, which would otherwise keep
   whatever the console drew there. The GPU mirror (gpu_mirror.h) writes those
   margins black every frame; this keeps the CPU copy pixel-identical to it. */
void display_set_mode(int m)
{
    static int shown = -1;
    bool top_was = shown >= 0 && shown != SCREEN_BOTTOM, bot_was = shown >= 0 && shown != SCREEN_TOP;
    for (int i = 0; i < 2; i++) {
        if (m != SCREEN_BOTTOM && !top_was && top_fb[i]) memset(top_fb[i], 0, SCR_W * SCR_H * 3);
        if (m != SCREEN_TOP && !bot_was && bot_fb[i]) memset(bot_fb[i], 0, 320 * SCR_H * 3);
    }
    shown = mode = m;
    stereo_cache_reset();                   /* the eye frames land on other columns */
}

/* the LCD framebuffers as Luma set them up: top 0/1, bottom 0/1 (0 if absent) */
void display_fbs(u32 out[4])
{
    out[0] = (u32)(uintptr_t)top_fb[0];
    out[1] = (u32)(uintptr_t)(top_fb[1] ? top_fb[1] : top_fb[0]);
    out[2] = (u32)(uintptr_t)bot_fb[0];
    out[3] = (u32)(uintptr_t)(bot_fb[1] ? bot_fb[1] : bot_fb[0]);
}

/* The old copy, kept for geometries the fast one does not take.
   Cost matters: the ARM11 kernel spins on every md0 request until this ARM9
   serves it, so the copy (a) checks the block mailbox every few columns,
   (b) reads each source pixel once as a word (it sits in VRAM), (c) uses
   precomputed column offsets, and (d) fills the second framebuffer of a
   pair with memcpy instead of re-reading the source. */
#define POLL_EVERY 8

/* md0 requests, and (display_set_poll) the buttons and touch screen: a CPU
   copy takes 150-430 ms on hardware and must not starve either (run 17) */
static void (*poll_extra)(void);
void display_set_poll(void (*fn)(void)) { poll_extra = fn; }
static void poll_io(void)
{
    blksrv_poll();
    if (poll_extra) poll_extra();
}

/* bottom LCD (320x240): column c <- iOS row c*3/2, memory row rr (from the
   bottom) <- iOS column (3*rr-40)/2; 13 px black margins top and bottom */
static void blit_bottom_slow(const u8 *src, u32 w, u32 h, u32 stride)
{
    static short ix[SCR_H];
    static u32 ix_w;
    u8 *fb0 = bot_fb[0] ? bot_fb[0] : bot_fb[1], *fb1 = bot_fb[0] ? bot_fb[1] : NULL;
    if (!fb0) return;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 t = rr * 3;
            ix[rr] = (t < 40 || (t - 40) / 2 >= w) ? -1 : (short)((t - 40) / 2);
        }
        ix_w = w;
    }
    for (u32 c = 0; c < 320; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        u32 iy = c * 3 / 2;
        const u32 *row = (const u32 *)(src + (iy < h ? iy : h - 1) * stride);
        u8 *d = fb0 + c * SCR_H * 3;
        for (u32 rr = 0; rr < SCR_H; rr++, d += 3) {
            u32 v = ix[rr] < 0 ? 0 : row[ix[rr]];
            d[0] = v;
            d[1] = v >> 8;
            d[2] = v >> 16;
        }
        if (fb1) memcpy(fb1 + c * SCR_H * 3, fb0 + c * SCR_H * 3, SCR_H * 3);
    }
}

/* Which framebuffers the LCDs show (bit 0/1: top set 0/1, bit 2/3: bottom
   set 0/1). Luma selects set 0 and nothing here ever swaps, so writing the
   other set only costs time; the ARM11 shim reads the PDC select registers
   (the ARM9 cannot reach them) and publishes them (gpu_mirror.h). Default:
   all four, as before. */
static u32 shown = 0xF;
void display_set_shown(u32 mask)
{
    u32 m = mask & 0xF ? mask & 0xF : 0xF;
    if (m != shown)
        stereo_cache_reset();               /* the other framebuffer set needs all columns */
    shown = m;
}

/*
 * The fast copy. On hardware the old byte-per-byte copy took 300-430 ms per
 * frame (run12/13: "[monitor] CPU screen copy: ... 432.8 ms each"): the ARM9
 * reaches VRAM slowly, one transaction per access. Here every VRAM access is
 * a 16-byte LDM/STM burst:
 *  - each needed source row is read whole (320 words) into ARM9 RAM,
 *  - each output column (240 pixels BGR8 = 720 bytes) is packed into 180
 *    words in ARM9 RAM (4 pixels = 3 words) and stored with bursts,
 *  - only to the framebuffers the LCDs show, and without reading VRAM back.
 * Same pixel mapping as before (the host test compares both copies and the
 * GPU mirror byte for byte).
 */
static void wcopy(u32 *d, const u32 *s, u32 n)       /* n: multiple of 4 words */
{
#ifndef __arm__                                     /* host test build (tools/ios3ds-pica/tests) */
    while (n--) *d++ = *s++;
#else
    __asm__ volatile(
        "1: ldmia %1!, {r3, r4, r5, r12}\n"
        "   stmia %0!, {r3, r4, r5, r12}\n"
        "   subs  %2, %2, #4\n"
        "   bne   1b\n"
        : "+r"(d), "+r"(s), "+r"(n) : : "r3", "r4", "r5", "r12", "memory", "cc");
#endif
}

static u32 rowbuf[320 + 4] __attribute__((aligned(32)));    /* +1: rowbuf[w] = black */
static u32 colbuf[SCR_H * 3 / 4] __attribute__((aligned(32)));

/* 240 pixels rowbuf[ix[0..239]] -> 180 words of BGR8 */
static void pack_column(const u16 *ix)
{
    u32 *o = colbuf;
    for (u32 rr = 0; rr < SCR_H; rr += 4, o += 3) {
        u32 p0 = rowbuf[ix[rr]] & 0xFFFFFF, p1 = rowbuf[ix[rr + 1]] & 0xFFFFFF;
        u32 p2 = rowbuf[ix[rr + 2]] & 0xFFFFFF, p3 = rowbuf[ix[rr + 3]] & 0xFFFFFF;
        o[0] = p0 | p1 << 24;
        o[1] = p1 >> 8 | p2 << 16;
        o[2] = p2 >> 16 | p3 << 8;
    }
}

static void store_column(u8 *const fb[2], u32 col, u32 bits)
{
    if (!fb[1]) bits |= bits >> 1;                  /* one buffer for both sets */
    for (u32 k = 0; k < 2; k++)
        if (fb[k] && (bits >> k & 1))
            wcopy((u32 *)(fb[k] + col * SCR_H * 3), colbuf, SCR_H * 3 / 4);
}

static void blit_bottom(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 t = rr * 3;
            ix[rr] = (t < 40 || (t - 40) / 2 >= w) ? (u16)w : (u16)((t - 40) / 2);
        }
        ix_w = w;
    }
    rowbuf[w] = 0;
    for (u32 c = 0; c < 320; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        u32 iy = c * 3 / 2;
        wcopy(rowbuf, (const u32 *)(src + (iy < h ? iy : h - 1) * stride), w);
        pack_column(ix);
        store_column(bot_fb, c, shown >> 2);
    }
}

static void blit_top(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    u32 outw = h * 3 / 4, x0;
    if (outw > SCR_W) outw = SCR_W;
    x0 = (SCR_W - outw) / 2;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    for (u32 c = 0; c < outw; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        wcopy(rowbuf, (const u32 *)(src + (c * 4 / 3) * stride), w);
        pack_column(ix);
        store_column(top_fb, x0 + c, shown);
    }
}

/* ---- stereoscopic top copy ---------------------------------------------
   One source row is read per screen column (as in blit_top). The alpha byte
   of the source pixel selects which horizontal sample each eye sees:
   out(x) = in(x - stereo_eye_delta(cls, k, right)). The alpha byte is masked
   away, so the LCD always gets opaque BGR8. The bottom copy above is shared
   unchanged, including the touch map. */
/* The source row is copied into eye_row+8 with eight pixels of padding on
   each side, filled with the edge pixel. An eye offset is at most 2*k = 8
   iOS pixels, so sampling never needs a clamp in the inner loop. */
#define ST_PAD 8
static u32 eye_row[ST_PAD + 320 + ST_PAD] __attribute__((aligned(32)));
static u32 eye_col[2][SCR_H] __attribute__((aligned(32)));
static int st_dl[STC_ALERT + 1], st_dr[STC_ALERT + 1];

static void stereo_set_deltas(int k)
{
    for (int cls = 0; cls <= STC_ALERT; cls++) {
        st_dl[cls] = stereo_eye_delta(cls, k, 0);
        st_dr[cls] = stereo_eye_delta(cls, k, 1);
    }
}

static void pack_eye_column(const u16 *ix)
{
    const u32 *row = eye_row + ST_PAD;
    for (u32 rr = 0; rr < SCR_H; rr++) {
        int x = ix[rr];
        int cls = stereo_px_class(row[x]);
        eye_col[0][rr] = row[x - st_dl[cls]] & 0xFFFFFF;
        eye_col[1][rr] = row[x - st_dr[cls]] & 0xFFFFFF;
    }
}

/* A cheap signature of one source row: every pixel matters (including the
   alpha stamp), but multiplication is expensive on the ARM9, so use shifts
   and two accumulators. A collision would only leave one screen column
   stale until its row changes again. */
static u32 stereo_row_hash(const u32 *row, u32 n)
{
    u32 a = 0x12345678u, b = 0x87654321u;
    for (u32 i = 0; i < n; i++) {
        u32 v = row[i];
        a = (a << 1 | a >> 31) + v;
        b = (b << 3 | b >> 29) ^ v;
    }
    return a ^ b;
}

static void pack_bgr_column(const u32 *src)
{
    u32 *o = colbuf;
    for (u32 rr = 0; rr < SCR_H; rr += 4, o += 3) {
        u32 p0 = src[rr] & 0xFFFFFF, p1 = src[rr + 1] & 0xFFFFFF;
        u32 p2 = src[rr + 2] & 0xFFFFFF, p3 = src[rr + 3] & 0xFFFFFF;
        o[0] = p0 | p1 << 24;
        o[1] = p1 >> 8 | p2 << 16;
        o[2] = p2 >> 16 | p3 << 8;
    }
}

static void blit_top_stereo(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    u32 outw = h * 3 / 4, x0, rebuilt = 0;
    int k = stereo_k();
    if (outw > SCR_W) outw = SCR_W;
    x0 = (SCR_W - outw) / 2;
    if (k != st_cache_k || ix_w != w) {
        stereo_cache_reset();
        st_cache_k = k;
    }
    stereo_set_deltas(k);
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    for (u32 c = 0; c < outw; c++) {
        u32 sy = c * 4 / 3, hash;
        if (c % POLL_EVERY == 0) poll_io();
        wcopy(eye_row + ST_PAD, (const u32 *)(src + sy * stride), w);
        hash = stereo_row_hash(eye_row + ST_PAD, w);
        if (st_hash_ok[sy] && st_hash[sy] == hash) {
            st_skipped++;
            continue;                       /* this eye column is already current */
        }
        if (rebuilt >= ST_COL_CAP)
            continue;                       /* finish it on a later blit */
        for (u32 i = 0; i < ST_PAD; i++)
            eye_row[i] = eye_row[ST_PAD];
        for (u32 i = 0; i < ST_PAD; i++)
            eye_row[ST_PAD + w + i] = eye_row[ST_PAD + w - 1];
        pack_eye_column(ix);
        for (u32 eye = 0; eye < 2; eye++) {
            if (!top_fb[eye])
                continue;
            pack_bgr_column(eye_col[eye]);
            wcopy((u32 *)(top_fb[eye] + (x0 + c) * SCR_H * 3), colbuf, SCR_H * 3 / 4);
        }
        st_hash[sy] = hash;
        st_hash_ok[sy] = 1;
        rebuilt++;
    }
    st_rebuilt += rebuilt;
}

void display_stereo_stats(u32 *rebuilt, u32 *skipped)
{
    *rebuilt = st_rebuilt;
    *skipped = st_skipped;
}

u32 display_stereo_holds(void)
{
    return st_holds;
}

/* The CLCD scan-out buffer is ordinary /vram. Until iOS draws its first
   frame it still holds whatever the loader or the GPU left there. Mirroring
   it as soon as AppleM2CLCD programs the register page showed a barcode of
   stale VRAM for the first one to two minutes of a hardware boot (issue #10:
   the log has no present yet, and the copy's spot check reports 0/512 wrong:
   the buffer itself is the stale picture). Keep the boot log on screen until
   the buffer changes after the CLCD is on. */
#define FB_PROBE_WORDS 64
static u32 fb_probe[FB_PROBE_WORDS];
static u32 fb_probe_pa, fb_probe_stride, fb_probe_h;
static bool fb_probe_waiting, fb_ready;

/* one word every 1/8 of the width and 1/8 of the height: the first frame
   (or the driver clearing the buffer) changes at least one of them for any
   real picture, while a static stale pattern does not. */
static u32 fb_probe_word(u32 base, u32 stride, u32 h, u32 w, int i)
{
    u32 y = (2 * (i >> 3) + 1) * h / 16u, x = (2 * (i & 7) + 1) * w / 16u;
    if (y >= h) y = h - 1;
    if (x >= w) x = w - 1;
    return *(vu32 *)(base + y * stride + x * 4);
}

static void fb_probe_take(u32 base, u32 stride, u32 h, u32 w)
{
    for (int i = 0; i < FB_PROBE_WORDS; i++)
        fb_probe[i] = fb_probe_word(base, stride, h, w, i);
    fb_probe_pa = base;
    fb_probe_stride = stride;
    fb_probe_h = h;
}

static bool fb_probe_changed(u32 base, u32 stride, u32 h, u32 w)
{
    for (int i = 0; i < FB_PROBE_WORDS; i++)
        if (fb_probe[i] != fb_probe_word(base, stride, h, w, i))
            return true;
    return false;
}

bool display_active(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    u32 base = r[0x24 / 4];
    if (!armed) return false;
    if (!touched) {
        for (u32 i = 0; i < 0x100 && !touched; i++) touched = r[i] != snapshot[i];
        if (!touched) return false;
    }
    if (!regs_pa || !(r[0] & 1) || base < VRAM_BASE || base >= VRAM_BASE + 0x600000 || !top_fb[0])
        return false;
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF, stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride || stride < w * 4 ||
        (u64)base + (u64)h * stride > (u64)VRAM_BASE + 0x600000)
        return false;
    if (!fb_ready) {
        if (!fb_probe_waiting) {
            fb_probe_take(base, stride, h, w);
            fb_probe_waiting = true;
            logp("[monitor] CLCD is on; waiting for the first iOS frame before mirroring its buffer\n");
            return false;
        }
        if (base == fb_probe_pa && stride == fb_probe_stride && h == fb_probe_h &&
            !fb_probe_changed(base, stride, h, w))
            return false;               /* still the loader's stale picture: keep the console */
        fb_ready = true;
        logp("[monitor] first iOS frame in the CLCD buffer: mirroring it now\n");
    }
    return true;
}

static void display_blit_slow(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    const u8 *src = (const u8 *)r[0x24 / 4];
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
    u32 stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride) return;
    if (mode != SCREEN_TOP) blit_bottom_slow(src, w, h, stride);
    if (mode == SCREEN_BOTTOM) return;
    /* screen column sx (0..399) <- source row iy; screen row from top <- source column ix */
    u32 outw = h * 3 / 4;                     /* 360 for h = 480 */
    if (outw > SCR_W) outw = SCR_W;
    u32 x0 = (SCR_W - outw) / 2;
    static u16 ix[SCR_H];
    static u32 ix_w;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    u8 *fb0 = top_fb[0] ? top_fb[0] : top_fb[1], *fb1 = top_fb[0] ? top_fb[1] : NULL;
    if (!fb0) return;
    for (u32 c = 0; c < outw; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        /* memory goes bottom-to-top: source column 0 at the bottom
           (rotate the 3DS 90 deg clockwise to read it upright) */
        const u32 *row = (const u32 *)(src + (c * 4 / 3) * stride);
        u8 *d = fb0 + (x0 + c) * SCR_H * 3;
        for (u32 rr = 0; rr < SCR_H; rr++, d += 3) {
            u32 v = row[ix[rr]];
            d[0] = v;
            d[1] = v >> 8;
            d[2] = v >> 16;
        }
        if (fb1) memcpy(fb1 + (x0 + c) * SCR_H * 3, fb0 + (x0 + c) * SCR_H * 3, SCR_H * 3);
    }
}

void display_blit(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    const u8 *src = (const u8 *)r[0x24 / 4];
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
    u32 stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride) return;
    if (w <= 320 && !(w & 3) && stride >= w * 4 && !(stride & 3) && !((u32)(uintptr_t)src & 3) &&
        top_fb[0] && !((u32)(uintptr_t)top_fb[0] & 3) && (!top_fb[1] || !((u32)(uintptr_t)top_fb[1] & 3)) &&
        (!bot_fb[0] || !((u32)(uintptr_t)bot_fb[0] & 3)) && (!bot_fb[1] || !((u32)(uintptr_t)bot_fb[1] & 3))) {
        /* Only called when the GPU mirror is not feeding the LCDs. The mode
           decision (stamp + slider) lives in display_stereo_probe(); here the
           cached ARM9 two-eye copy is the fallback for a mirror that stopped
           or cannot do stereo. With the flag off this is the original copy. */
        if (mode != SCREEN_TOP) blit_bottom(src, w, h, stride);
        if (mode != SCREEN_BOTTOM) {
            if (stereo_want)
                blit_top_stereo(src, w, h, stride);
            else
                blit_top(src, w, h, stride);
        }
        return;
    }
    display_blit_slow(regs_pa);
}
