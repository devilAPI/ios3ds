/*
 * ARM9 side of the GPU display mirror (boot/common/gpu_mirror.h,
 * docs/pica200.md section 9): the ARM11 shim copies the iOS framebuffer to
 * the LCDs with the PICA200; the ARM9 tells it what to copy where, and skips
 * its own CPU copy (display.c) only while GPU frames keep arriving. Anything
 * else - mirror off in boot.cfg, self-test failed, a GPU timeout, GL keeping
 * the GPU busy for a while, an old shim - leaves the CPU copy running.
 * FCRAM is uncached on the ARM9, so the mailbox is plain volatile memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gpumirror.h"
#include "gpu_mirror.h"
#include "display.h"
#include "log.h"
#include "util.h"
#include "blksrv.h"

static volatile gmbox_t *gm;
static u32 hz;

static const char *const st_name[] = { "off", "waiting for the GPU", "self-test", "ACTIVE", "FAILED", "disabled" };
static const char *const why_name[] = { "", "boot.cfg", "GPU bring-up failed", "gpu.bin missing/stale",
    "framebuffer layout", "no arena memory", "self-test mismatch", "GPU timeout", "iOS framebuffer geometry" };

/* watchdog ticks (1047311 Hz) -> microseconds */
static u32 us(u32 ticks) { return ticks / 1047u * 1000u + ticks % 1047u * 1000u / 1047u; }

void gm_init(u32 shim_pa, u32 tick_hz, const u32 cfgv[7], int mode)
{
    u32 fb[4];
    gm = (volatile gmbox_t *)(shim_pa + SHIM_GM_OFF);
    hz = tick_hz;
    for (u32 i = 0; i < sizeof(gmbox_t) / 4; i++)
        ((vu32 *)gm)[i] = 0;
    display_fbs(fb);
    gm->top_fb[0] = fb[0];
    gm->top_fb[1] = fb[1];
    gm->bot_fb[0] = fb[2];
    gm->bot_fb[1] = fb[3];
    gm->mode = (u32)mode;
    gm->gpu_enable = cfgv[0];
    gm->mirror_enable = cfgv[1];
    gm->vbl_div = cfgv[2] ? cfgv[2] : 1;
    gm->spin_us = cfgv[3];
    gm->gpu_clk = cfgv[4];
    gm->gpu_diag = cfgv[5];
    gm->ca_gpu = cfgv[6];
    gm->seq = 1;
    gm->magic = GM_MAGIC;
    logp("[gpu] PICA200: gpu=%u gpu_mirror=%u (every %u. VBlank, spin %u us) gpu_clk=%u gpu_diag=%u ca_gpu=%u; "
         "mailbox at %08x\n", cfgv[0], cfgv[1], (unsigned)gm->vbl_div, cfgv[3], cfgv[4], cfgv[5], cfgv[6],
         shim_pa + SHIM_GM_OFF);
}

void gm_want(bool on)
{
    if (gm && gm->want != (u32)on) {
        gm->want = on;
        gm->seq++;
    }
}

/* Tell the shim about a new screen mode and wait (at most 100 ms, serving
   md0 meanwhile) until no GPU frame built for the old one is still being
   written: the caller then redraws the console on the screen given back. */
void gm_set_mode(int mode, u32 (*now)(void))
{
    if (!gm)
        return;
    gm->mode = (u32)mode;
    u32 seq = ++gm->seq, t0 = now();
    if (gm->state != GM_ST_ACTIVE)
        return;
    while (gm->ack_seq != seq && now() - t0 < hz / 10)
        blksrv_poll();
    if (gm->ack_seq != seq)
        log_warn("[gpu] mirror did not acknowledge the mode change within 100 ms\n");
}

/* Is the GPU copying the screen right now? Logs every change. */
bool gm_live(u32 now, bool display_on)
{
    static u32 last_state = 0xFFFFFFFFu, last_frames, last_idle, last_change, last_busy, busy_since;
    static int was_live = -1;
    static bool was_off;
    if (!gm)
        return false;
    u32 st = gm->state, frames = gm->frames, idle = gm->idle_frames;
    if (!display_on) {
        was_off = true;                 /* the shim skips frames while the CLCD is off */
        return false;
    }
    if (was_off) {
        was_off = false;
        last_frames = frames;           /* grace: frames resume within a few VBlanks */
        last_idle = idle;
        last_change = now;
        last_busy = gm->busy_skips;
        busy_since = 0;
    }
    if (st != last_state) {
        last_state = st;
        const char *s = st < ARRAY_SIZE(st_name) ? st_name[st] : "?";
        u32 why = gm->reason;
        if (st == GM_ST_ACTIVE)
            log_ok("[gpu] display mirror %s: self-test variant %u, %u ms\n", s, (unsigned)gm->variant,
                   us(gm->selftest_ticks) / 1000);
        else if (st == GM_ST_FAILED && why == GM_WHY_SELFTEST)
            log_warn("[gpu] display mirror %s (%s): %u bytes differed, first at %u: %02x expected, %02x seen; "
                     "the ARM9 keeps copying the screen\n", s, why_name[why], (unsigned)gm->selftest_bad,
                     (unsigned)gm->first_bad[0], (unsigned)gm->first_bad[1], (unsigned)gm->first_bad[2]);
        else if (st == GM_ST_FAILED && why == GM_WHY_TIMEOUT)
            log_warn("[gpu] display mirror %s (%s: operation %u of a frame did not finish, register 0x%08x); "
                     "the ARM9 keeps copying the screen\n", s, why_name[why], (unsigned)gm->first_bad[0],
                     (unsigned)gm->first_bad[3]);
        else if (st == GM_ST_FAILED || st == GM_ST_DISABLED)
            log_warn("[gpu] display mirror %s (%s); the ARM9 keeps copying the screen\n", s,
                     why < ARRAY_SIZE(why_name) ? why_name[why] : "?");
        else
            logp("[gpu] display mirror: %s\n", s);
    }
    if (frames != last_frames || idle != last_idle) {
        /* a real frame, or the shim skipping an unchanged frame (idle_frames):
           both mean the LCDs hold the current picture and the mirror is alive */
        last_frames = frames;
        last_idle = idle;
        last_change = now;
        busy_since = 0;
    } else if (gm->busy_skips != last_busy) {
        /* no mirror frame because GL (CoreAnimation) owns the GPU: busy, not
           stalled. After a long lock CA redraws everything and holds the GPU
           for seconds; falling back to the ARM9 copy then only showed a half-
           copied frame (soak cycle 19, re-log 63). Busy counts as progress
           for at most 3 s. */
        if (!busy_since) busy_since = now ? now : 1;
        if (now - busy_since < 3 * hz) last_change = now;
    }
    last_busy = gm->busy_skips;
    bool live = st == GM_ST_ACTIVE && (frames || idle) && now - last_change < hz && gm->ack_seq == gm->seq;
    if ((int)live != was_live) {
        if (live)
            log_ok("[gpu] screen copy: PICA200 (ARM9 CPU copy paused)\n");
        else if (was_live == 1)
            log_warn("[gpu] screen copy: ARM9 CPU again (GPU frames stopped: state %s, %u busy skips, %u timeouts)\n",
                     st < ARRAY_SIZE(st_name) ? st_name[st] : "?", (unsigned)gm->busy_skips, (unsigned)gm->timeouts);
        was_live = live;
    }
    return live;
}

/* Spot check of whatever copied the screen last (GPU or CPU): sample LCD
   pixels, map them back to iOS pixels with gm_map and compare. Pixels whose
   source changed while being looked at are skipped; an animated screen can
   still differ because the copy is older than the source. On a still screen
   both paths must give 0. */
static void spot_check(u32 regs_pa, int mode, bool gpu)
{
    static u32 seed = 12345;
    vu32 *r = (vu32 *)regs_pa;
    const vu32 *src = (const vu32 *)r[0x24 / 4];
    u32 fb[4], n = 0, bad = 0, first = 0, exp1 = 0, got1 = 0;
    if (!regs_pa || r[0x28 / 4] != GM_SRC_W || r[0x30 / 4] != (GM_SRC_W << 16 | GM_SRC_H))
        return;
    display_fbs(fb);
    for (u32 k = 0; k < 512; k++) {
        seed = seed * 1103515245u + 12345u;
        u32 bottom = (seed >> 8) & 1;
        if ((bottom && mode == SCREEN_TOP) || (!bottom && mode == SCREEN_BOTTOM))
            continue;
        u32 rows = bottom ? GM_RT_BOT_H : GM_RT_TOP_H, j = (seed >> 9) % rows, i = (seed >> 20) % GM_RT_W;
        uint32_t x, y;
        u32 v = 0;
        if (gm_map((int)bottom, i, j, &x, &y)) {
            v = src[y * GM_SRC_W + x];
            if (src[y * GM_SRC_W + x] != v)
                continue;
        }
        const vu8 *d = (const vu8 *)(bottom ? fb[2] : fb[0]) + (j * GM_RT_W + i) * 3;
        u32 got = d[0] | d[1] << 8 | d[2] << 16;
        n++;
        if (got != (v & 0xFFFFFF)) {
            if (!bad++) {
                first = (bottom ? 0x80000000u : 0) | j << 16 | i;
                exp1 = v & 0xFFFFFF;
                got1 = got;
            }
        }
    }
    logp("[gpu] spot check (%s copy): %u of %u sampled LCD pixels differ from the iOS framebuffer%s", gpu ? "GPU" : "CPU",
         bad, n, bad ? "" : "\n");
    if (bad)
        logp(" (first: %s fb row %u px %u: %06x expected, %06x seen; only meaningful on a still screen)\n",
             first >> 31 ? "bottom" : "top", (first >> 16) & 0x7FFF, first & 0xFFFF, exp1, got1);
}

/* Which framebuffer sets the LCDs show (bit mask for display_set_shown), as
   the shim publishes them; 0xF (all) until it does. */
u32 gm_shown(void)
{
    static u32 last = 0xF;
    u32 m = 0;
    if (!gm || gm->pdc_magic != GM_PDC_MAGIC)
        return 0xF;
    for (u32 n = 0; n < 2; n++) {
        u32 v = gm->pdc_swap[n], next = v & 1, cur = v >> 4 & 1;
        m |= (next == cur ? 1u << next : 3u) << (2 * n);
    }
    if (m != last) {
        logp("[monitor] LCDs show framebuffer set %s on top, %s on the bottom: the CPU copy writes only those\n",
             (m & 3) == 3 ? "0+1" : (m & 1) ? "0" : "1", (m & 12) == 12 ? "0+1" : (m & 4) ? "0" : "1");
        last = m;
    }
    return m;
}

/* Stereo mode switch. The PDC registers are ARM11-only (3dbrew
   GPU/External Registers), so the GPU part of the shim writes them; the
   parallax barrier is ARM9-side and is enabled by display.c after this ack.
   With the GPU mirror in use the shim also builds both eyes: wait for one
   frame in the requested mode so the barrier never opens over a half-drawn
   pair, and never lets the GPU overwrite the eyes while the ARM9 builds
   them (an old blob, or a mirror that cannot do stereo, is paused here).
   Every wait is bounded: a stalled shim must not hold the monitor loop. */
static bool stereo_paused;

static void gm_pause_mirror(bool pause)
{
    if (!gm || stereo_paused == pause)
        return;
    stereo_paused = pause;
    gm->want = !pause;
    u32 seq = ++gm->seq;
    if (!pause)
        return;
    for (volatile u32 n = 0; n < 200000 && (gm->mirror_busy || gm->ack_seq != seq); n++)
        blksrv_poll();
}

bool gm_set_stereo(bool on)
{
    if (!gm || !gm->gpu_enable || gm->magic != GM_MAGIC)
        return false;
    bool mirror = gm->state == GM_ST_ACTIVE && gm->frames && !stereo_paused;
    bool change = gm->stereo_on != (u32)on || gm->stereo_ack_seq != gm->stereo_seq;
    u32 seq = change ? ++gm->stereo_seq : gm->stereo_seq;
    u32 st0 = gm->st_frames, fl0 = gm->flat_frames;
    bool ok;
    if (change)
        gm->stereo_on = on;
    if (mirror && on && !gm->stereo_ok)
        gm_pause_mirror(true);              /* old blob: the ARM9 builds the eyes */
    if (!on && stereo_paused) {
        /* leave the CPU fallback: the 2D preset goes back first, then the
           mirror resumes and paints both framebuffer sets over the eyes */
        for (volatile u32 n = 0; n < 200000 && gm->stereo_ack_seq != seq; n++)
            blksrv_poll();
        gm_pause_mirror(false);
    }
    for (volatile u32 n = 0; n < 200000 && gm->stereo_ack_seq != seq; n++)
        blksrv_poll();
    ok = gm->stereo_ack_seq == seq;
    if (ok && mirror && !stereo_paused) {
        u32 target = on ? st0 : fl0, now = on ? gm->st_frames : gm->flat_frames;
        for (volatile u32 n = 0; n < 2000000 && now == target; n++) {
            blksrv_poll();
            now = on ? gm->st_frames : gm->flat_frames;
        }
    }
    if (change)
        logp("[stereo] %s: PDC0 %s, %s (ack %u, %u/%u mode frames)\n", ok ? "switch" : "switch FAILED",
             on ? "3D" : "2D", gm_stereo_path(), (unsigned)gm->stereo_ack_seq,
             (unsigned)gm->st_frames, (unsigned)gm->flat_frames);
    return ok;
}

/* The GL engine stamps the same classes whatever the slider says; only the
   eye offset changes, so publish k without restarting the mirror. */
void gm_set_shift(u32 k)
{
    if (gm && gm->stereo_k != k) {
        gm->stereo_k = k;
        gm->seq++;                  /* no frame built with the old k may land late */
    }
}

/* Which side built the last top-screen pair (for the [stereo] log line). */
const char *gm_stereo_path(void)   /* declared in gpumirror.h */
{
    if (!gm || !gm->stereo_on)
        return "flat (GPU mirror)";
    if (stereo_paused || gm->state != GM_ST_ACTIVE)
        return "ARM9 two-eye copy";
    return "PICA200 two-eye mirror";
}

/* every 30 s: what the copy costs, GPU and CPU side by side */
void gm_report(u32 now, u32 cpu_blits, u32 cpu_ticks, u32 regs_pa, int mode, bool shown)
{
    static u32 last_t, last_frames, last_sum, last_irq, last_steps, last_blits, last_cpu, last_idle;
    if (!gm || now - last_t < 30 * hz)
        return;
    u32 dt = now - last_t, frames = gm->frames - last_frames, sum = gm->sum_frame - last_sum;
    u32 idle = gm->idle_frames - last_idle;
    u32 irq = gm->sum_irq - last_irq, steps = gm->steps - last_steps;
    u32 nb = cpu_blits - last_blits, cb = cpu_ticks - last_cpu;
    last_t = now;
    last_frames = gm->frames;
    last_sum = gm->sum_frame;
    last_irq = gm->sum_irq;
    last_steps = gm->steps;
    last_blits = cpu_blits;
    last_cpu = cpu_ticks;
    last_idle = gm->idle_frames;
    u32 secs = dt / hz ? dt / hz : 1;
    if (frames)
        logp("[gpu] mirror: %u frames in %u s (%u.%u fps); GPU per frame: upload %u us, draw %u us, read-out %u us; "
             "frame avg %u us (max %u, incl. IRQ latency); ARM11 IRQ time %u us/frame in %u steps; "
             "%u busy skips, %u timeouts\n",
             frames, secs, frames * 10 / secs / 10, frames * 10 / secs % 10,
             us(gm->t_in), us(gm->t_3d), us(gm->t_out), us(sum / frames), us(gm->m_frame), us(irq / frames), steps,
             (unsigned)gm->busy_skips, (unsigned)gm->timeouts);
    else if (gm->state == GM_ST_ACTIVE)
        /* No CA present for the whole window: damage tracking avoided every
           GPU operation. Say so explicitly, otherwise a hardware log has no
           mirror line at all and looks like a stalled mirror. */
        logp("[gpu] mirror: %u idle VBlanks in %u s (no new CA frame, no GPU work); %u busy skips, %u timeouts\n",
             idle, secs, (unsigned)gm->busy_skips, (unsigned)gm->timeouts);
    if (shown && !gm->stereo_on)
        spot_check(regs_pa, mode, frames != 0 || idle != 0);   /* idle VBlanks keep the last GPU picture */
    else if (shown)
        logp("[gpu] spot check skipped: the two eye images are offset from the source by design\n");
    if (nb) {                   /* ARM9 timer ticks (hz) */
        u32 t = cb / nb;
        logp("[monitor] CPU screen copy: %u copies in %u s, %u.%u ms each (ARM9)\n", nb, secs,
             t * 1000u / hz, t * 10000u / hz % 10);
    }
    static u32 last_rb, last_sk;
    u32 rb, sk;
    display_stereo_stats(&rb, &sk);
    if ((rb | sk) != (last_rb | last_sk)) {
        logp("[monitor] stereo eye columns: %u rebuilt, %u reused in %u s (cached two-eye copy)\n",
             rb - last_rb, sk - last_sk, secs);
        last_rb = rb;
        last_sk = sk;
    }
    {   /* GPU cost per mirror frame, 3D (two-eye list) vs 2D (flat list) */
        static u32 l_sn, l_ss, l_s3, l_fn, l_fs, l_f3;
        u32 sn = gm->st_n - l_sn, fn = gm->fl_n - l_fn;
        if (sn)
            logp("[stereo] mirror GPU cost: 3D %u frames, avg %u us (draw %u us); 2D %u frames, avg %u us (draw %u us) "
                 "in %u s\n", sn, us((gm->st_sum - l_ss) / sn), us((gm->st_sum_3d - l_s3) / sn), fn,
                 fn ? us((gm->fl_sum - l_fs) / fn) : 0, fn ? us((gm->fl_sum_3d - l_f3) / fn) : 0, secs);
        l_sn = gm->st_n; l_ss = gm->st_sum; l_s3 = gm->st_sum_3d;
        l_fn = gm->fl_n; l_fs = gm->fl_sum; l_f3 = gm->fl_sum_3d;
    }
    logp("[stereo] active path: %s (slider k=%u, %u 3D / %u 2D GPU frames, %u ARM9 eye cols reused, "
         "%u transitions held in 3D)\n",
         gm_stereo_path(), (unsigned)gm->stereo_k, (unsigned)gm->st_frames, (unsigned)gm->flat_frames, sk,
         display_stereo_holds());
}
