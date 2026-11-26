/*
 * ios3ds ARM9 monitor (M1).
 *
 *  1. text consoles on both screens (framebuffers from Luma's screen init)
 *  2. sd:/ios3ds/log.txt (previous boot kept as log_prev.txt)
 *  3. handshake with the ARM11 stub, print its CPU ID registers + probe results
 *  4. read sd:/ios3ds/boot.cfg, load kernel.bin + devtree.bin into FCRAM
 *  5. build XNU boot_args, tell the ARM11 to jump into XNU
 *  6. stay alive as the monitor: forward ARM11/XNU log rings to screen + SD
 *
 * Every step logs before it starts, so a hang shows where it happened.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "util.h"
#include "console.h"
#include "log.h"
#include "blksrv.h"
#include "display.h"
#include "stereo.h"
#include "tsc.h"
#include "swap.h"
#include "wifi/wifi.h"
#include "audio.h"
#include "gpumirror.h"
#include "gpu_mirror.h"
#include "mcu.h"
#include "gyro.h"
#include "camera.h"
#include "powersave.h"
#include "exthid.h"
#include "bootcfg.h"

#define CFG9_SDMMCCTL (*(vu16 *)0x10000020u)

extern void waitcycles(u32 n);
extern u32 a9_get_cpsr(void);

/* ~1 ms of ARM9 busy-wait at 134 MHz (4 cycles per waitcycles iteration) */
static void delay_ms(u32 ms)
{
    while (ms--) waitcycles(33500);
}

static const char *stage_name(u32 s)
{
    static const char *n[] = { "none", "entry", "id-regs", "probes", "vfp", "ready", "jumping", "n3ds" };
    return s < ARRAY_SIZE(n) ? n[s] : "?";
}

/* ------------------------------------------------------------------------ */
/* bottom screen status                                                     */

static void status(int row, u32 color, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_line(CON_BOTTOM, row, color, buf);
}

/* ------------------------------------------------------------------------ */
/* ARM11 log ring                                                           */

static u32 a11_log_tail;
static u32 clk_ticks(void);
static bool log_ts;         /* boot.cfg log_ts=1 */
static void ts_prefix(char *buf, u32 *n);

/* copy ring bytes [*tail, head) to the log in chunks: one f_write per chunk,
   not per byte (per-byte writes + syncs made real-SD console output crawl
   and starved the md0 block server running on the same ARM9) */
/* console bytes other than whitespace: "activity" for the stall dump and
   the heartbeat. In QEMU the kernel console carries a steady stream of empty
   lines (70% of all lines in a soak, none on hardware), which kept the
   heartbeat from ever seeing a quiet console. */
static u32 text_bytes;

static void drain_ring(const volatile char *buf, u32 size, u32 *tail, u32 head, u32 color, bool *bol)
{
    char chunk[512];
    while (*tail != head) {
        u32 n = 0;
        while (*tail != head && n < sizeof(chunk) - 16) {
            char c = buf[*tail % size];
            /* the stamp goes before a line's first visible character (the
               console sends "\r" and blank lines long before the text) */
            bool vis = c != '\n' && c != '\r' && c != ' ' && c != '\t';
            if (log_ts && *bol && vis) ts_prefix(chunk, &n);
            if (c == '\n') *bol = true;
            else if (vis) *bol = false;
            chunk[n++] = c;
            if (c != '\n' && c != '\r' && c != ' ' && c != '\t') text_bytes++;
            (*tail)++;
        }
        log_raw(chunk, n, color);
    }
}

static void drain_a11_log(void)
{
    volatile shared_t *sh = SHARED;
    u32 head = sh->log_head;
    if (head - a11_log_tail > SHARED_LOG_SIZE) {
        log_warn("[a11] (log overrun, %u bytes lost)\n", head - a11_log_tail - SHARED_LOG_SIZE);
        a11_log_tail = head - SHARED_LOG_SIZE;
    }
    static bool bol = true;
    drain_ring((const volatile char *)sh->log_buf, SHARED_LOG_SIZE, &a11_log_tail, head, CON_CYAN, &bol);
}

/* boot.cfg parsing and defaults: bootcfg.c / bootcfg.h */

static const char *const screen_names[SCREEN_MODES] = { "top", "both", "bottom" };

/* where iOS and the monitor log go (display.h SCREEN_*) */
static void apply_screen_mode(int m)
{
    display_set_mode(m);
    con_show(CON_TOP, m == SCREEN_BOTTOM);
    con_show(CON_BOTTOM, m == SCREEN_TOP);
    log_target(m == SCREEN_TOP ? CON_BOTTOM : m == SCREEN_BOTTOM ? CON_TOP : -1);
    log_color(CON_CYAN, "[monitor] screen mode: %s (X = next)\n", screen_names[m]);
}

static bool in_fcram(u32 pa, u32 len)
{
    return pa >= FCRAM_BASE && len <= FCRAM_SIZE && pa - FCRAM_BASE <= FCRAM_SIZE - len;
}

static bool validate_cfg(void)
{
    bool ok = true;
#define CHECK(cond, ...) do { if (!(cond)) { log_err(__VA_ARGS__); ok = false; } } while (0)
    CHECK(cfg.phys_base == FCRAM_BASE, "phys_base must be 0x%08x\n", FCRAM_BASE);
    CHECK((cfg.phys_base & 0xFFFFFF) == 0, "phys_base not 16 MiB aligned\n");
    CHECK(cfg.mem_size && (cfg.mem_size & 0xFFFFF) == 0 && cfg.mem_size <= FCRAM_SIZE,
          "mem_size must be a non-zero multiple of 1 MiB <= 128 MiB (XNU loops forever otherwise)\n");
    CHECK(in_fcram(cfg.kernel_pa, 0x1000), "kernel_pa outside FCRAM\n");
    CHECK(cfg.entry_pa > cfg.kernel_pa && cfg.entry_pa < cfg.devtree_pa, "entry_pa not inside kernel\n");
    CHECK(cfg.devtree_pa > cfg.kernel_pa && cfg.bootargs_pa > cfg.devtree_pa, "region order wrong\n");
    CHECK(cfg.ring_pa >= cfg.bootargs_pa + sizeof(boot_args_t), "ring overlaps boot_args\n");
    CHECK(!cfg.shim[0] || (cfg.shim_pa >= cfg.bootargs_pa + sizeof(boot_args_t) && cfg.shim_pa < cfg.ring_pa),
          "shim_pa must lie between boot_args and the ring\n");
    CHECK(cfg.top_pa >= cfg.ring_pa + cfg.ring_size, "ring above topOfKernelData\n");
    CHECK((cfg.top_pa & 0x3FFF) == 0, "top_pa must be 16 KiB aligned (it becomes TTBR)\n");
    CHECK(cfg.top_pa + 0x9000 <= cfg.phys_base + cfg.mem_size, "page tables beyond mem_size\n");
#undef CHECK
    return ok;
}

/* ------------------------------------------------------------------------ */
/* ARM11 handshake                                                          */

static bool wait_arm11_ready(u32 timeout_ms)
{
    volatile shared_t *sh = SHARED;
    u32 token = sh->a9_token + 1;
    if (!token) token = 1;
    for (u32 t = 0; t < timeout_ms; t++) {
        sh->a9_token = token;
        if (sh->a11_echo == token && sh->magic == SHARED_MAGIC && sh->a11_stage == A11_STAGE_READY) {
            return true;
        }
        drain_a11_log();
        delay_ms(1);
    }
    return false;
}

static void report_cpu(void)
{
    volatile shared_t *sh = SHARED;
    log_color(CON_CYAN, "ARM11 core0 CP15 ID registers:\n");
    logp(" MIDR  %08x  CTR   %08x  TLBTR %08x\n", sh->midr, sh->ctr, sh->tlbtr);
    logp(" MPIDR %08x  PFR0  %08x  PFR1  %08x\n", sh->mpidr, sh->id_pfr0, sh->id_pfr1);
    logp(" DFR0  %08x  AFR0  %08x\n", sh->id_dfr0, sh->id_afr0);
    logp(" MMFR0-3 %08x %08x %08x %08x\n", sh->id_mmfr[0], sh->id_mmfr[1], sh->id_mmfr[2], sh->id_mmfr[3]);
    logp(" ISAR0-2 %08x %08x %08x\n", sh->id_isar[0], sh->id_isar[1], sh->id_isar[2]);
    logp(" ISAR3-5 %08x %08x %08x\n", sh->id_isar[3], sh->id_isar[4], sh->id_isar[5]);
    logp(" SCTLR %08x  ACTLR %08x  CPACR %08x  CPSR %08x\n", sh->sctlr, sh->actlr, sh->cpacr, sh->cpsr);
    u32 midr = sh->midr;
    logp(" -> part %03x rev r%up%u (%s)\n", (midr >> 4) & 0xFFF, (midr >> 20) & 0xF, midr & 0xF,
         ((midr >> 4) & 0xFFF) == 0xB02 ? "ARM11 MPCore" : "unexpected!");
}

static void report_probes(void)
{
    volatile shared_t *sh = SHARED;
    log_color(CON_CYAN, "Instruction probes (XNU uses these):\n");
    for (u32 i = 0; i < sh->nprobes && i < PROBE_MAX; i++) {
        volatile probe_t *p = &sh->probe[i];
        if (p->result == PROBE_OK) {
            log_ok(" %08x ok      -> %08x\n", p->encoding, p->value);
        } else if (p->result == PROBE_EXCEPTION) {
            log_warn(" %08x TRAPPED (slot %u mode %02x)\n", p->encoding, p->exc_slot, p->exc_mode);
        } else {
            log_err(" %08x not run\n", p->encoding);
        }
    }
    if (sh->unexpected_count) {
        log_err(" %u unexpected ARM11 exception(s): slot %u mode %02x lr %08x\n",
                sh->unexpected_count, sh->unexpected_slot, sh->unexpected_mode, sh->unexpected_lr);
    }
}

/* ------------------------------------------------------------------------ */
/* hang probes: everything the shim recorded, written to log.txt once the     */
/* ARM11 stops reading the clock, so no photo transcription is needed.       */

static u32 fc_rd(u32 pa) { return in_fcram(pa, 4) ? *(vu32 *)pa : 0xBAD0BAD0u; }

/* wall clock: ARM9 timer 2 at 67027964/1024 Hz cascading into timer 3 (32 bit,
   wraps after ~18 h). Timer 0 is the Wi-Fi code's entropy source; 1 is free. */
#define T9_VAL(n) (*(vu16 *)(0x10003000u + (n) * 4))
#define T9_CNT(n) (*(vu16 *)(0x10003002u + (n) * 4))
#define T9_HZ     65457u

static void clk_start(void)
{
    static bool started;        /* once, at ARM9 start: all times count from there */
    if (started) return;
    started = true;
    T9_CNT(2) = 0;
    T9_CNT(3) = 0;
    T9_VAL(2) = 0;
    T9_VAL(3) = 0;
    T9_CNT(3) = 0x80 | 0x04;            /* enable, count-up (cascade) */
    T9_CNT(2) = 0x80 | 0x03;            /* enable, prescaler 1024 */
}

static u32 clk_ticks(void)
{
    u16 hi, lo, hi2;
    do {
        hi = T9_VAL(3);
        lo = T9_VAL(2);
        hi2 = T9_VAL(3);
    } while (hi != hi2);
    return (u32)hi << 16 | lo;
}

static u32 clk_secs(void) { return clk_ticks() / T9_HZ; }
static u32 clk_ms(void) { return (u32)((u64)clk_ticks() * 1000 / T9_HZ); }

static void ts_prefix(char *buf, u32 *n)
{
    u32 t = clk_ms();
    *n += snprintf(buf + *n, 16, "[%u.%03u] ", t / 1000, t % 1000);
}

/* ARM9 boot phases (docs/boot-perf.md): one line each, ms since the ARM9 started */
static void stamp(const char *what)
{
    u32 t = clk_ms();
    logp("[boot] %u.%03u s: %s\n", t / 1000, t % 1000, what);
}

/* heartbeat while the console is quiet: is the kernel alive (clock reads,
   ticks), is md0 busy, and where does the ARM11 spend its time (IRQ-time PC
   samples, shim+0x5F80, 16 x {pc, spsr}, newest = st_pc_idx - 1) */
static void heartbeat(u32 quiet_s)
{
    vu32 *st = (vu32 *)cfg.shim_pa;
    vu32 *ring = (vu32 *)(cfg.shim_pa + 0x5F80);
    u32 bs, be, n = st[0x9C / 4];
    blksrv_stats(&bs, &be);
    logp("[hb] t=%us quiet %us | abs %u dec %u timer %u irq %u vbl %u | md0 %u req %u KiB %u err\n",
         clk_secs(), quiet_s, (unsigned)st[0x80 / 4], (unsigned)st[0x8C / 4], (unsigned)st[0x4C / 4],
         (unsigned)st[0x98 / 4], (unsigned)st[0x78 / 4], bs, blksrv_sectors() / 2, be);
    /* clock-rate check (hardware run12/13: gettimeofday and mach_absolute_time
       disagree by 2-2.6x). The ARM11 time base is the MPCore watchdog as of the
       last clock read (shim +0x44 high word, +0x48 low word, 1047311 Hz
       nominal); the ARM9 timer is independent. In ms since the first heartbeat. */
    {
        static u64 a11_0;
        static u32 a9_0;
        static bool have0;
        u64 a11 = ((u64)st[0x44 / 4] << 32 | st[0x48 / 4]) >> N3RES_TBSHIFT(st[SHIM_N3DS_OFF / 4]);
        u32 a9 = clk_ticks();
        if (!have0) {
            a11_0 = a11;
            a9_0 = a9;
            have0 = true;
        }
        u32 a11_ms = (u32)((a11 - a11_0) * 1000 / 1047311u);
        u32 a9_ms = (u32)((u64)(a9 - a9_0) * 1000 / T9_HZ);
        logp("[hb]  clock: arm11 watchdog +%u ms vs arm9 timer +%u ms (ratio %u/1000)\n",
             a11_ms, a9_ms, a9_ms ? (u32)((u64)a11_ms * 1000 / a9_ms) : 0);
    }
    char line[200];
    int len = snprintf(line, sizeof(line), "[hb]  pc");
    for (u32 i = 1; i <= 8 && i <= n; i++) {
        u32 k = (n - i) & 15, pc = ring[k * 2], psr = ring[k * 2 + 1];
        const char *m = (psr & 0x1F) == 0x10 ? "u" : (psr & 0x1F) == 0x13 ? "k" : "x";
        len += snprintf(line + len, sizeof(line) - len, " %s%08x%s", m, pc, (psr & 0x20) ? "t" : "");
        if (len >= (int)sizeof(line)) break;
    }
    logp("%s\n", line);
}

static void dump_probes(const char *why)
{
    vu32 *st = (vu32 *)cfg.shim_pa;
    vu32 *g = (vu32 *)(cfg.shim_pa + 0x5F00);
    u32 v[8];

    log_warn("[probe] %s\n", why);
    logp("[probe] shim init%u timer%u other%u last%u def%u vbl%u inj%u irq-entries%u\n",
         (unsigned)st[0x40 / 4], (unsigned)st[0x4C / 4], (unsigned)st[0x50 / 4], (unsigned)st[0x54 / 4],
         (unsigned)st[0x58 / 4], (unsigned)st[0x78 / 4], (unsigned)st[0x7C / 4], (unsigned)st[0x98 / 4]);
    logp("[probe] abs %u raw %08x cpsr %08x | dec %u last %u caller %08x\n",
         (unsigned)st[0x80 / 4], (unsigned)st[0x84 / 4], (unsigned)st[0x88 / 4],
         (unsigned)st[0x8C / 4], (unsigned)st[0x90 / 4], (unsigned)st[0x94 / 4]);
    logp("[probe] at the last clock read: ptimer count %08x ctrl %08x isr %x\n",
         (unsigned)g[0], (unsigned)g[1], (unsigned)g[2]);
    logp("[probe]  gicc ctrl %x pmr %x hppir %x | gicd ctrl %x en %08x/%08x pend %08x/%08x act %08x\n",
         (unsigned)g[3], (unsigned)g[4], (unsigned)g[5], (unsigned)g[6], (unsigned)g[7],
         (unsigned)g[10], (unsigned)g[8], (unsigned)g[9], (unsigned)g[11]);
    logp("[probe]  sctlr %08x ttbr0 %08x ttbr1 %08x ttbcr %08x dacr %08x mpidr %08x\n",
         (unsigned)g[12], (unsigned)g[13], (unsigned)g[14], (unsigned)g[15],
         (unsigned)g[0x60 / 4], (unsigned)g[0x64 / 4]);
    logp("[probe]  a11 walk stage %u: L1[fff] %08x L2[f0] %08x irq-literal %08x handler %08x %08x\n",
         (unsigned)g[0x7C / 4], (unsigned)g[0x68 / 4], (unsigned)g[0x6C / 4],
         (unsigned)g[0x70 / 4], (unsigned)g[0x74 / 4], (unsigned)g[0x78 / 4]);
    for (int i = 0; i < 8; i++) v[i] = g[0x40 / 4 + i];
    logp("[probe]  a11 vectors %08x %08x %08x %08x %08x %08x %08x %08x\n",
         v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);

    /* the same walk through physical memory (what the table walker and an
       uncached instruction fetch see if the ARM11 did not clean its D-cache) */
    u32 ttbr1 = g[14];
    if (!ttbr1) {
        log_warn("[probe] no TTBR1 recorded (snapshot never ran)\n");
        return;
    }
    u32 l1 = fc_rd((ttbr1 & ~0x3FFFu) + 0xFFF * 4);
    u32 l2 = (l1 & 3) == 1 ? fc_rd((l1 & ~0x3FFu) + 0xF0 * 4) : 0;
    u32 vpa = (l2 & 2) ? (l2 & ~0xFFFu) : 0;
    u32 w[8], lit = 0, h0 = 0, h1 = 0;
    for (int i = 0; i < 8; i++) w[i] = vpa ? fc_rd(vpa + i * 4) : 0;
    if ((w[6] >> 12) == 0xE59FFu) {
        lit = fc_rd(vpa + 0x20 + (w[6] & 0xFFF));
        u32 hpa = lit - cfg.virt_base + cfg.phys_base;
        h0 = fc_rd(hpa);
        h1 = fc_rd(hpa + 4);
    }
    logp("[probe]  a9  walk: L1[fff] %08x L2[f0] %08x vector page PA %08x irq-literal %08x handler %08x %08x\n",
         l1, l2, vpa, lit, h0, h1);
    logp("[probe]  a9  vectors %08x %08x %08x %08x %08x %08x %08x %08x\n",
         w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    bool same = l1 == g[0x68 / 4] && l2 == g[0x6C / 4];
    for (int i = 0; i < 8; i++) same = same && w[i] == v[i];
    if (g[0x7C / 4] >= 3) same = same && lit == g[0x70 / 4];
    if (g[0x7C / 4] >= 4) same = same && h0 == g[0x74 / 4] && h1 == g[0x78 / 4];
    if (same) log_ok("[probe]  ARM11 view == memory (vector page and tables are clean)\n");
    else log_err("[probe]  ARM11 view != memory: stale D-cache or a different mapping\n");
}

/* ------------------------------------------------------------------------ */
/* monitor                                                                  */

/* buttons and touch while a CPU screen copy runs (display_set_poll): at most
   every 10 ms */
static void poll_buttons(void)
{
    static u32 last;
    u32 t = clk_ticks();
    if (t - last < T9_HZ / 100)
        return;
    last = t;
    input_poll(cfg.shim_pa);
    mcu_poll(t);
    gyro_poll(t);
    exthid_poll(t);
}

static void __attribute__((noreturn)) monitor(const char *phase)
{
    volatile xnu_ring_t *ring = cfg.ring_pa ? (volatile xnu_ring_t *)cfg.ring_pa : NULL;
    u32 ring_tail = 0;
    u32 tick = 0;
    bool l1_seen = false;
    u32 *l1 = cfg.top_pa ? (u32 *)cfg.top_pa : NULL;

    log_color(CON_GREY, "[monitor] %s - ARM9 stays here forwarding logs.\n", phase);
    clk_start();
    /* from here on the consoles are painted a few rows per loop pass, never
       inside a log call (run 17: a visible log console starved the loop) */
    con_defer(true);
    display_set_poll(poll_buttons);
    /* sync the log every 0.5 s, not after every write: a power cut loses at
       most 0.5 s, a hang loses nothing (the ARM9 keeps syncing) */
    log_sync_each(false);
    u32 last_sync = 0;
    for (;;) {
        bool busy = false, io_recent = true;
        /* md0 first: the ARM11 kernel spins (IRQs off) until its request is
           served and usually posts the next one within microseconds while it
           streams a file, so after serving one, linger up to 2 ms for the next
           before running the rest of the loop */
        if (blksrv_poll()) {
            busy = true;
            u32 start = clk_ticks(), t0 = start;     /* at most 100 ms: input, */
            while (clk_ticks() - t0 < T9_HZ / 500 + 1 &&  /* log and screen run too */
                   clk_ticks() - start < T9_HZ / 10) {
                if (blksrv_poll()) t0 = clk_ticks();
            }
        }
        drain_a11_log();
        blksrv_tick(clk_ticks());   /* idle write-back of dirty md0 sectors */
        blksrv_prof_tick();
        if (ring && ring->magic == XNU_RING_MAGIC && ring->size) {
            u32 head = ring->head;
            if (head - ring_tail > ring->size) {
                log_warn("[xnu] (ring overrun, %u bytes lost)\n", head - ring_tail - ring->size);
                ring_tail = head - ring->size;
            }
            static bool bol = true;
            drain_ring((const volatile char *)ring->buf, ring->size, &ring_tail, head, CON_WHITE, &bol);
        }
        /* The entry stub zeroes 36 KiB at topOfKernelData and writes the L1
           entries for virtBase. We filled it with a pattern before the jump. */
        if (l1 && !l1_seen) {
            u32 idx = cfg.virt_base >> 20;
            u32 e = l1[idx];
            if (e != 0xA5A5A5A5u && e != 0) {
                l1_seen = true;
                log_ok("[monitor] XNU entry stub ran: L1[0x%x] = %08x (TTBR region %08x)\n", idx, e, cfg.top_pa);
            }
        }
        /* 3DS pad + touch -> shim input ring (read by /usr/libexec/ios3ds-input) */
        input_poll(cfg.shim_pa);
        /* Wi-Fi driver step + frames <-> network mailbox (kern.ios3ds_net) */
        if (wifi_poll()) busy = true;
        /* docs/mcu.md: the MCU Wi-Fi LED follows the link (netmbox_t.link) */
        mcu_set_wifi_led(((volatile netmbox_t *)NET_PA)->link);
        audio_poll(cfg.shim_pa);    /* docs/audio.md */
        mcu_poll(clk_ticks());      /* RTC, battery, sliders, HOME/POWER (docs/mcu.md) */
        gyro_poll(clk_ticks());     /* gyroscope while iOS wants it (docs/mcu.md 11) */
        exthid_poll(clk_ticks());   /* New 3DS C-stick, ZL/ZR (docs/n3ds.md) */
        camera_poll();              /* sensor power + I2C set-up for the shim (docs/camera.md) */
        static bool ios_display, x_latched;
        static int screen_mode;
        /* screen copy pacing: <= 15 fps; while md0 is busy (a request in the
           last 100 ms) only every 500 ms, because the ARM11 kernel stalls on
           each block request until this loop serves it */
        static u32 last_blit, last_io, last_served, cpu_blits, cpu_ticks;
        u32 nserved, nerr, tnow = clk_ticks();
        blksrv_stats(&nserved, &nerr);
        if (nserved != last_served) {
            last_served = nserved;
            last_io = tnow;
        }
        bool io_busy = tnow - last_io < T9_HZ / 10;
        u32 blit_gap = io_busy ? T9_HZ / 2 : T9_HZ / 15;
        /* the PICA200 copies the screen while its frames keep coming (gpumirror.c) */
        bool disp_on = cfg.display_regs && display_active(cfg.display_regs);
        bool gpu_copy = gm_live(tnow, disp_on);
        /* stereo3d=1 + slider up: only home frames get the two-eye copy and
           the barrier (docs/stereo3d.md). The slider comes from the MCU box. */
        display_set_stereo(cfg.stereo3d, mcu_slider3d());
        if (cfg.display_regs && tnow - last_blit >= blit_gap && display_active(cfg.display_regs)) {
            if (!ios_display) {
                ios_display = true;
                log_color(CON_GREEN, "[monitor] iOS display is on: top screen now mirrors the CLCD framebuffer\n");
                screen_mode = cfg.screen < SCREEN_MODES ? (int)cfg.screen : SCREEN_BOTH;
                apply_screen_mode(screen_mode);
                gm_set_mode(screen_mode, clk_ticks);
                gm_want(true);
            }
            /* sample the class map at copy rate (not every monitor loop) and
               let the mirror / barrier follow the slider and the frame */
            display_stereo_probe(cfg.display_regs);
            gpu_copy = gm_live(tnow, disp_on);
            if (!gpu_copy) {
                u32 t0 = clk_ticks();
                display_set_shown(gm_shown());
                display_blit(cfg.display_regs);
                cpu_blits++;
                cpu_ticks += clk_ticks() - t0;
            }
            last_blit = clk_ticks();
        }
        /* power saving while iOS' screen is off (powersave.c, docs/mcu.md section 9) */
        if (ps_update(cfg.display_regs && display_active(cfg.display_regs), ios_display, clk_ticks()))
            last_blit = clk_ticks() - blit_gap;     /* woken: copy a fresh frame on the next loop */
        io_recent = clk_ticks() - last_io < T9_HZ / 5;
        /* X: next screen mode (X is not forwarded to iOS as a button) */
        bool xdown = !(HID_PAD & HID_X);
        if (ios_display && xdown && !x_latched) {
            screen_mode = (screen_mode + 1) % SCREEN_MODES;
            gm_set_mode(screen_mode, clk_ticks);     /* GPU frames for the old mode finish first */
            apply_screen_mode(screen_mode);
        }
        x_latched = xdown;
        static bool combo_latched;
        bool combo = !(HID_PAD & HID_SELECT) && !(HID_PAD & HID_START);
        if (cfg.shim_pa && combo && !combo_latched) {
            log_warn("[monitor] SELECT+START: asking XNU to panic (thread dump)\n");
            *(vu32 *)(cfg.shim_pa + SHIM_PANIC_REQ) = 1;
        }
        combo_latched = combo;
        if ((tick & 63) == 0) {
            u32 bs, be;
            gm_report(clk_ticks(), cpu_blits, cpu_ticks, cfg.display_regs, screen_mode, ios_display);
            blksrv_stats(&bs, &be);
            status(27, CON_GREY, "monitor alive #%u a11=%s", tick / 64, stage_name(SHARED->a11_stage));
            status(28, CON_GREY, "xnu ring: %u bytes", ring ? (unsigned)ring->head : 0);
            status(29, CON_GREY, "md0: %u req, %u err", bs, be);
            if (cfg.shim_pa) {          /* shim stats (kernel/shim/entry.S +0x40..) */
                vu32 *st = (vu32 *)cfg.shim_pa;
                status(26, CON_GREY, "shim i%u t%u o%u L%u d%u v%u j%u", (unsigned)st[0x40 / 4],
                       (unsigned)st[0x4C / 4], (unsigned)st[0x50 / 4], (unsigned)st[0x54 / 4] & 0x3FF,
                       (unsigned)st[0x58 / 4], (unsigned)st[0x78 / 4], (unsigned)st[0x7C / 4]);
                /* clock probes: reads/raw watchdog/CPSR, deadline sets/last/caller */
                status(24, CON_GREY, "abs %u w%08x c%03x", (unsigned)st[0x80 / 4],
                       (unsigned)st[0x84 / 4], (unsigned)st[0x88 / 4] & 0x1FF);
                status(25, CON_GREY, "dec %u l%u @%08x", (unsigned)st[0x8C / 4],
                       (unsigned)st[0x90 / 4], (unsigned)st[0x94 / 4]);
                /* timer + GIC snapshot from the last clock read (shim+0x5F00) */
                vu32 *g = (vu32 *)(cfg.shim_pa + 0x5F00);
                status(21, CON_GREY, "irq %u pt %x c%x s%x", (unsigned)st[0x98 / 4],
                       (unsigned)g[0], (unsigned)g[1], (unsigned)g[2]);
                status(22, CON_GREY, "cc%x pm%x hp%x dc%x act%x", (unsigned)g[3],
                       (unsigned)g[4], (unsigned)g[5], (unsigned)g[6], (unsigned)g[11]);
                status(23, CON_GREY, "en%08x/%08x pd%08x/%08x", (unsigned)g[7],
                       (unsigned)g[10], (unsigned)g[8], (unsigned)g[9]);
                status(20, CON_GREY, "sctlr %08x stage %u", (unsigned)g[12], (unsigned)g[0x7C / 4]);
                /* 5 s without a clock read or console text: dump once.
                   10 s without console text: heartbeat every 10 s; otherwise
                   one every 30 s. */
                static u32 last_abs, last_head, abs_since, head_since, next_hb;
                static bool dumped;
                u32 now = clk_secs();
                u32 abs = st[0x80 / 4], hd = text_bytes;
                if (abs != last_abs || hd != last_head) {
                    abs_since = now;
                    dumped = false;
                }
                if (hd != last_head) {
                    head_since = now;
                    next_hb = now + 10;
                }
                last_abs = abs;
                last_head = hd;
                if (st[0x40 / 4] && !dumped && now - abs_since >= 5) {
                    dumped = true;
                    dump_probes("ARM11 stalled: no clock read or console output for 5 s");
                }
                /* also every 30 s regardless: the daemons now log real text
                   every few seconds, so the console is rarely quiet for 10 s */
                static u32 last_hb;
                if (st[0x40 / 4] && ((int)(now - next_hb) >= 0 || now - last_hb >= 30)) {
                    next_hb = now + 10;
                    last_hb = now;
                    heartbeat(now - head_since);
                }
            }
        }
        con_paint(2);                   /* ~1 ms per row on hardware */
        u32 now_t = clk_ticks();
        if (now_t - last_sync >= T9_HZ / 2) {
            log_flush();
            last_sync = now_t;
        }
        tick++;
        /* ~2 ms of mailbox polling instead of a blind delay: block requests
           are latency-bound (the ARM11 spins while it waits) */
        bool slept = ps_sleep(busy, io_recent);     /* screen off and idle: WFI ~2 ms */
        for (u32 i = 0; !slept && !busy && i < 200; i++) {
            if (blksrv_poll()) busy = true;
            else waitcycles(335);
        }
    }
}

/* ------------------------------------------------------------------------ */

/* New 3DS: ask the ARM11 stub for the model, the 804 MHz clock and the L2
   cache (boot.cfg n3ds_clock / n3ds_l2), and take part in its timer
   calibration (cal_state 1 -> 2 now -> 3 after 1309 ticks = 20.0 ms of the
   ARM9 timer). The result word goes into the shim (SHIM_N3DS_OFF): the shim
   programs its timer prescaler and tick shift from it. docs/n3ds.md */
static u32 n3ds_res;

/* Apple device tree (flattened): node = nprops, nchildren, props, children;
   prop = name[32], length (bit 31: placeholder), value padded to 4 bytes.
   Returns the end of the node; sets *reg to the value of /memory's "reg". */
static const u8 *dt_walk(const u8 *n, const u8 *end, int depth, u32 **reg)
{
    u32 np, nc, i, len;
    const u8 *p = n + 8, *name = NULL, *r = NULL;
    if (n + 8 > end) return end;
    np = ((const u32 *)n)[0];
    nc = ((const u32 *)n)[1];
    for (i = 0; i < np && p + 36 <= end; i++) {
        len = ((const u32 *)p)[8] & 0x7FFFFFFFu;
        if (!strncmp((const char *)p, "name", 5)) name = p + 36;
        if (!strncmp((const char *)p, "reg", 4) && len >= 8) r = p + 36;
        p += 36 + ((len + 3) & ~3u);
    }
    if (depth == 1 && name && r && !strncmp((const char *)name, "memory", 7)) *reg = (u32 *)r;
    for (i = 0; i < nc && p < end; i++) p = dt_walk(p, end, depth + 1, reg);
    return p;
}

/* New 3DS with n3ds_ram=1: the ARM11 stub found RAM of its own at
   0x28000000-0x2FFFFFFF (PDN_LGR_CNT bit 0): XNU's memSize and the device
   tree's /memory become 256 MiB. Everything ios3ds reserves stays in the
   first 9 MiB; the GPU arena is kept below 0x28000000 by the shim
   (pica.c). docs/n3ds.md section 6. */
static void n3ds_ram(void)
{
    boot_args_t *ba = (boot_args_t *)cfg.bootargs_pa;
    u32 *reg = NULL;
    if (!(n3ds_res & N3RES_RAM)) {
        log_warn("[n3ds] FCRAM extension not usable (no RAM of its own at 0x28000000): iOS keeps 128 MiB\n");
        return;
    }
    dt_walk((const u8 *)cfg.devtree_pa, (const u8 *)cfg.bootargs_pa, 0, &reg);
    if (!reg || reg[0] != cfg.phys_base || reg[1] != FCRAM_SIZE) {
        log_warn("[n3ds] device tree /memory not found: iOS keeps 128 MiB\n");
        return;
    }
    reg[1] = FCRAM_N3DS_SIZE;
    ba->memSize = FCRAM_N3DS_SIZE;
    cfg.mem_size = FCRAM_N3DS_SIZE;
    logp("[n3ds] FCRAM extension: iOS gets 256 MiB (boot_args memSize, device tree /memory reg)\n");
}

static void n3ds_setup(void)
{
    volatile shared_t *sh = SHARED;
    sh->n3ds_res = 0;
    sh->cal_state = 0;
    sh->n3ds_req = (cfg.n3ds_clock ? N3REQ_CLOCK : 0) | (cfg.n3ds_l2 ? N3REQ_L2 : 0) |
                   (cfg.n3ds_ram && cfg.mem_size == FCRAM_SIZE ? N3REQ_RAM : 0);
    sh->a9_cmd = A9CMD_N3DS;
    for (u32 t0 = clk_ticks(); !(sh->n3ds_res & N3RES_DONE) && clk_ticks() - t0 < 3 * T9_HZ;) {
        if (sh->cal_state == 1) {
            u32 c = clk_ticks(), c0;
            while ((c0 = clk_ticks()) == c) ;          /* start on a tick edge */
            sh->cal_state = 2;
            while (clk_ticks() - c0 < 1309u) ;
            sh->cal_state = 3;
        }
        drain_a11_log();
    }
    sh->a9_cmd = A9CMD_NONE;
    drain_a11_log();
    n3ds_res = sh->n3ds_res;
    if (!(n3ds_res & N3RES_DONE)) {
        log_err("[n3ds] ARM11 stub did not answer (old stub?): Old 3DS timings assumed\n");
        n3ds_res = 0;
        return;
    }
    if (n3ds_res & N3RES_NEW) {
        u32 hz = sh->n3ds_periph_hz;
        logp("[n3ds] model: New 3DS (CFG11_SOCINFO %x), ARM11 %s MHz, PDN_LGR_SOCMODE %x, LGR_CNT %x\n",
             sh->n3ds_socinfo, (n3ds_res & N3RES_804) ? "804" : "268", sh->n3ds_socmode, sh->n3ds_lgr_cnt);
        logp("[n3ds] MPCore timers: PERIPHCLK %u.%02u MHz measured, prescaler /%u, tick shift %u -> %u Hz\n",
             hz / 1000000u, hz / 10000u % 100u, N3RES_PRESCALE(n3ds_res) + 1, N3RES_TBSHIFT(n3ds_res),
             hz / ((N3RES_PRESCALE(n3ds_res) + 1) << N3RES_TBSHIFT(n3ds_res)));
        if (n3ds_res & N3RES_TIMEOUT)
            log_err("[n3ds] clock switch not acknowledged (GIC 88): set n3ds_clock=0 in boot.cfg\n");
        if (cfg.n3ds_l2)
            logp("[n3ds] L2 cache: %s (L2C-310 id %08x aux %08x ctrl %x)\n",
                 (n3ds_res & N3RES_L2) ? "on, write-through" : "OFF", sh->n3ds_l2_id, sh->n3ds_l2_aux, sh->n3ds_l2_ctrl);
        else
            logp("[n3ds] L2 cache: off (boot.cfg n3ds_l2=0)\n");
    } else {
        logp("[n3ds] model: Old 3DS (CFG11_SOCINFO %x), PERIPHCLK %u Hz\n", sh->n3ds_socinfo, sh->n3ds_periph_hz);
    }
    if ((n3ds_res & N3RES_NEW) && cfg.n3ds_ram && cfg.mem_size == FCRAM_SIZE)
        n3ds_ram();
    if (cfg.shim[0] && cfg.shim_pa)
        *(vu32 *)(cfg.shim_pa + SHIM_N3DS_OFF) = n3ds_res;
}

static char cfgbuf[16384];     /* boot.cfg passed 4 KiB with the New 3DS switches: the cmdline at the end was cut off */

void a9_main(int argc, char **argv)
{
    u8 *top0 = NULL, *top1 = NULL, *bot0 = NULL, *bot1 = NULL;
    char why[96];

    /* argv lives on Luma's ITCM stack: copy what we need first */
    static char path0[64];              /* argv[0]: which .firm was booted (issue #9) */
    if (argc >= 1 && argv && argv[0]) {
        const char *a = argv[0];
        unsigned i = 0;
        while (i < sizeof path0 - 1 && a[i] >= 0x20 && a[i] < 0x7f) { path0[i] = a[i]; i++; }
        path0[i] = 0;
    }
    if (argc >= 2 && argv && argv[1]) {
        u32 *fbs = (u32 *)argv[1];
        top0 = (u8 *)fbs[0];
        bot0 = (u8 *)fbs[2];
        top1 = (u8 *)fbs[3];
        bot1 = (u8 *)fbs[5];
    }
    clk_start();
    con_init(top0, top1, bot0, bot1);
    display_init(top0, top1, bot0, bot1);

    status(0, CON_GREEN, "ios3ds %s", IOS3DS_VERSION);
    status(1, CON_GREY, "M1 loader - ARM9 monitor");
    status(3, CON_WHITE, "log: sd:/ios3ds/log.txt");
    status(4, CON_WHITE, "A during countdown = jump now; B = cancel");
    status(5, CON_WHITE, "hold POWER to switch off");

    log_color(CON_GREEN, "ios3ds %s  (M1 loader)\n", IOS3DS_VERSION);
    logp("ARM9: argc=%d cpsr=%08x argv0=\"%s\"\n", argc, a9_get_cpsr(), path0);
    if (top0) {
        logp("framebuffers: top %p/%p bottom %p/%p\n", top0, top1, bot0, bot1);
    } else {
        log_warn("no framebuffers from loader (screen-init flag ignored?) - SD log only\n");
    }

    /* SD slot -> TMIO1 (ARM9), card power on; log the before/after values */
    u16 sdctl = CFG9_SDMMCCTL;
    CFG9_SDMMCCTL = (sdctl & ~1u) | 0x200u;
    logp("CFG9_SDMMCCTL %04x -> %04x\n", sdctl, CFG9_SDMMCCTL);

    logp("mounting SD...\n");
    if (log_mount_sd(why, sizeof(why)) != 0) {
        log_err("SD: %s\n", why);
        status(7, CON_RED, "SD MOUNT FAILED - see top screen");
    } else {
        log_ok("SD mounted, logging to sd:/ios3ds/log.txt\n");
        status(7, CON_GREEN, "SD ok");
        firm_swap_restore();            /* launcher (.3dsx) boot: Luma back first */
        if (firm_is_stale()) {          /* issue #9: old chainload copy vs new shim.bin */
            status(8, CON_RED, "OUTDATED ios3ds.firm (Luma chainloader)");
            status(9, CON_RED, "see log: power off, boot again");
            monitor("outdated payload");
        }
    }

    logp("waiting for ARM11 (stage %s)...\n", stage_name(SHARED->a11_stage));
    if (!wait_arm11_ready(3000)) {
        volatile shared_t *sh = SHARED;
        log_err("ARM11 not ready after 3 s: stage=%s probe=%u magic=%08x echo=%08x\n",
                stage_name(sh->a11_stage), sh->a11_current_probe, sh->magic, sh->a11_echo);
        status(8, CON_RED, "ARM11 STUCK at stage %s", stage_name(sh->a11_stage));
        report_cpu();
        monitor("ARM11 did not come up");
    }
    log_ok("ARM11 ready\n");
    stamp("SD mounted, ARM11 ready");
    status(8, CON_GREEN, "ARM11 ok, MIDR %08x", SHARED->midr);
    report_cpu();
    drain_a11_log();

    if (!log_sd_ok()) {
        log_err("no SD card: cannot load the kernel\n");
        monitor("no SD");
    }

    logp("reading sd:/ios3ds/boot.cfg\n");
    int n = sd_read_small("0:/ios3ds/boot.cfg", cfgbuf, sizeof(cfgbuf));
    if (n < 0) {
        log_err("boot.cfg missing (FRESULT %d). Run 'make sd' on the PC.\n", -n);
        status(9, CON_RED, "NO boot.cfg");
        monitor("nothing to boot");
    }
    if (n >= (int)sizeof(cfgbuf) - 1)
        log_err("boot.cfg is larger than %u bytes: the rest is ignored\n", (unsigned)sizeof(cfgbuf) - 1);
    parse_bootcfg(cfgbuf);
    log_ts = cfg.log_ts != 0;
    blksrv_profile(cfg.ioprof, clk_ticks, T9_HZ);
    stamp("boot.cfg read");

    if (cfg.probes || cfg.diag) {   /* diagnostics only (0.25 s): off unless asked for */
        logp("running ARM11 instruction probes (boot.cfg probes=0 diag=0 skips them)\n");
        log_flush();
        volatile shared_t *sh = SHARED;
        sh->a9_cmd = A9CMD_PROBE;
        u32 t = 0;
        for (; t < 2000 && !sh->probes_done; t++) {
            drain_a11_log();
            delay_ms(1);
        }
        drain_a11_log();
        if (!sh->probes_done) {
            log_err("ARM11 hung in probe %u (encoding %08x). Set probes=0 in boot.cfg.\n",
                    sh->a11_current_probe, sh->probe[sh->a11_current_probe % PROBE_MAX].encoding);
            status(9, CON_RED, "ARM11 HUNG IN PROBE %u", sh->a11_current_probe);
            monitor("ARM11 hung in a probe");
        }
        sh->a9_cmd = A9CMD_NONE;
        report_probes();
        stamp("ARM11 probes done");
    }
    logp("kernel %s @%08x entry %08x\n", cfg.kernel, cfg.kernel_pa, cfg.entry_pa);
    logp("devtree %s @%08x, boot_args @%08x\n", cfg.devtree, cfg.devtree_pa, cfg.bootargs_pa);
    logp("log ring @%08x+%x, topOfKernelData %08x\n", cfg.ring_pa, cfg.ring_size, cfg.top_pa);
    logp("phys %08x virt %08x memSize %08x\n", cfg.phys_base, cfg.virt_base, cfg.mem_size);
    logp("cmdline: \"%s\"\n", cfg.cmdline);
    if (!validate_cfg()) {
        status(9, CON_RED, "boot.cfg INVALID");
        monitor("bad boot.cfg");
    }

    u32 ksize = 0, dsize = 0;
    logp("loading kernel...\n");
    int r = sd_load(cfg.kernel, (void *)cfg.kernel_pa, cfg.devtree_pa - cfg.kernel_pa, &ksize);
    if (r) {
        log_err("kernel load failed (%d)\n", r);
        status(9, CON_RED, "KERNEL LOAD FAILED");
        monitor("kernel load failed");
    }
    log_ok("kernel: %u bytes at %08x\n", ksize, cfg.kernel_pa);
    r = sd_load(cfg.devtree, (void *)cfg.devtree_pa, cfg.bootargs_pa - cfg.devtree_pa, &dsize);
    if (r) {
        log_err("devtree load failed (%d)\n", r);
        monitor("devtree load failed");
    }
    log_ok("devtree: %u bytes at %08x\n", dsize, cfg.devtree_pa);
    stamp("kernel + devtree loaded");

    if (cfg.shim[0]) {
        u32 ssize = 0;
        r = sd_load(cfg.shim, (void *)cfg.shim_pa, cfg.ring_pa - cfg.shim_pa, &ssize);
        if (r) {
            log_err("shim load failed (%d)\n", r);
            monitor("shim load failed");
        }
        u32 magic = *(vu32 *)(cfg.shim_pa + 4);
        log_ok("shim: %u bytes at %08x (magic %08x)\n", ssize, cfg.shim_pa, magic);
        /* The kernel's patches branch into these fixed addresses. A shim.bin
           that is not ours (corrupt first cluster, a foreign file, or a
           different program) used to be accepted silently: the ARM11 then
           jumped into whatever is at 0x80840000 on its first console write
           and stalled with no output at all (issue #9: magic e7bfe619). Catch
           it here, before the jump, and leave the reason on the screen and in
           log.txt. */
        if (ssize < 0x10 || magic != SHIM_MAGIC) {
            log_err("shim: bad header (magic %08x, want %08x 'SHIM'): sd:/ios3ds/shim.bin is corrupt or "
                    "from another build\n", magic, SHIM_MAGIC);
            log_err("shim: re-run install.py from the same zip as ios3ds.firm (or delete the card's "
                    "ios3ds/shim.bin and reinstall)\n");
            monitor("shim header invalid");
        }
    }

    if (cfg.rootfs[0]) {
        u32 secs = 0;
        int br = blksrv_open(cfg.rootfs, &secs);
        if (br) {
            log_err("rootfs %s: open failed (%d) - md0 will return I/O errors\n", cfg.rootfs, br);
        } else {
            log_ok("rootfs %s: %u MiB served as md0\n", cfg.rootfs, secs / 2048);
        }
        stamp("rootfs opened (fast-seek map)");
    }

    /* the shim calls its GPU part only if gpu.bin put the part's magic there
       (kernel/shim/gpu_glue.c): drop whatever a previous boot left in FCRAM */
    *(vu32 *)GPU_PART_PA = 0;
    for (u32 i = 0; i < cfg.nloads; i++) {
        u32 sz = 0, pa = cfg.load_pa[i];
        bool ok_dst = (pa >= 0x1FF80000 && pa < 0x1FFF0000) || in_fcram(pa, 0x1000) ||
                      (pa >= VRAM_BASE && pa < VRAM_BASE + 0x600000);
        if (!ok_dst) {
            log_err("load %s: destination %08x not allowed\n", cfg.load_path[i], pa);
            monitor("bad load= address");
        }
        u32 max = pa < 0x1FFF0000 && pa >= 0x1FF80000 ? 0x1FFF0000 - pa : 0x100000;
        r = -1;
        if (cfg.stereo3d) {
            /* stereo3d=1: the GPU part whose PDC0 3D switch writes PDC0
               (gpu3d.bin, kernel/shim/pica_stereo.c ST_PDC_BASE); gpu.bin's
               writes miss PDC0. Falls back to gpu.bin on an old card. */
            u32 n = strlen(cfg.load_path[i]);
            if (n >= 7 && n + 2 < sizeof cfg.load_path[i] && !memcmp(cfg.load_path[i] + n - 7, "gpu.bin", 7)) {
                char p3[sizeof cfg.load_path[i]];
                memcpy(p3, cfg.load_path[i], n - 7);
                strlcpy_(p3 + n - 7, "gpu3d.bin", sizeof p3 - (n - 7));
                r = sd_load(p3, (void *)pa, max, &sz);
                if (!r)
                    log_ok("[stereo] stereo3d=1: GPU part %s instead of gpu.bin (PDC0 3D switch)\n", p3);
            }
        }
        if (r)
            r = sd_load(cfg.load_path[i], (void *)pa, max, &sz);
        if (r) {
            log_err("load %s failed (%d)\n", cfg.load_path[i], r);
            monitor("load= failed");
        }
        log_ok("loaded %s: %u bytes at %08x\n", cfg.load_path[i], sz, pa);
    }

    stamp("load= files loaded");
    display_arm(cfg.display_regs);

    u32 entry_word = *(vu32 *)cfg.entry_pa;
    logp("entry word %08x (expect e3a01000 'mov r1,#0')\n", entry_word);

    /* boot_args */
    boot_args_t *ba = (boot_args_t *)cfg.bootargs_pa;
    memset(ba, 0, sizeof(*ba));
    ba->Revision = cfg.ba_revision;
    ba->Version = cfg.ba_version;
    ba->virtBase = cfg.virt_base;
    ba->physBase = cfg.phys_base;
    ba->memSize = cfg.mem_size;
    ba->topOfKernelData = cfg.top_pa;
    ba->Video.v_baseAddr = cfg.video_pa;
    ba->Video.v_display = cfg.video_display;
    ba->Video.v_rowBytes = cfg.video_rowbytes;
    ba->Video.v_width = cfg.video_width;
    ba->Video.v_height = cfg.video_height;
    ba->Video.v_depth = cfg.video_depth;
    ba->machineType = 0;
    ba->deviceTreeP = cfg.devtree_pa - cfg.phys_base + cfg.virt_base;
    ba->deviceTreeLength = dsize;
    strlcpy_(ba->CommandLine, cfg.cmdline, sizeof(ba->CommandLine));
    /* diag=1: the test/diagnostic launchd jobs (gltest, boot tone, [smp] sampler)
       look for this boot-arg in kern.bootargs and exit at once without it */
    if (cfg.diag && strlen(ba->CommandLine) + 14 < sizeof(ba->CommandLine))
        strlcpy_(ba->CommandLine + strlen(ba->CommandLine), " ios3ds_diag=1", 15);
    /* gps=LAT,LON[,ACC] -> boot-arg ios3ds_gps=... for CoreLocation hooks */
    if (cfg.gps[0]) {
        char gpstmp[160];
        snprintf(gpstmp, sizeof(gpstmp), " ios3ds_gps=%s", cfg.gps);
        if (strlen(ba->CommandLine) + strlen(gpstmp) < sizeof(ba->CommandLine))
            strlcpy_(ba->CommandLine + strlen(ba->CommandLine), gpstmp,
                     sizeof(ba->CommandLine) - strlen(ba->CommandLine));
    }
    /* stereo3d=1 -> boot-arg ios3ds_stereo=1 so the CA render server stamps
       depth classes into the frame's alpha (docs/stereo3d.md) */
    /* Substrate safe mode (issue 1): boot.cfg safe_mode=1, or L held now ->
       ios3ds-applog loads no tweaks in SpringBoard or the apps this boot */
    if ((cfg.safe_mode || !(HID_PAD & HID_L)) && strlen(ba->CommandLine) + 14 < sizeof(ba->CommandLine)) {
        strlcpy_(ba->CommandLine + strlen(ba->CommandLine), " ios3ds_safe=1", 15);
        log_warn("safe mode (%s): Substrate tweaks stay off this boot\n", cfg.safe_mode ? "boot.cfg safe_mode=1" : "L held");
        status(12, CON_YELLOW, "SAFE MODE: no Substrate tweaks");
    }
    if (cfg.stereo3d && strlen(ba->CommandLine) + 20 < sizeof(ba->CommandLine))
        strlcpy_(ba->CommandLine + strlen(ba->CommandLine), " ios3ds_stereo=1", 17);
    /* hwtest=1|2 -> boot-arg ios3ds_hwtest=N: launchd starts ios3ds-hwtest
       (tools/ios3ds-diag/hwtest.c), which checks every driver once the home
       screen is up and writes a [hwtest] SUMMARY block (docs/real-hardware.md) */
    if (cfg.hwtest && strlen(ba->CommandLine) + 18 < sizeof(ba->CommandLine)) {
        char hw[20];
        snprintf(hw, sizeof(hw), " ios3ds_hwtest=%u", cfg.hwtest > 2 ? 1u : cfg.hwtest);
        strlcpy_(ba->CommandLine + strlen(ba->CommandLine), hw, sizeof(ba->CommandLine) - strlen(ba->CommandLine));
        log_color(CON_YELLOW, "[hwtest] hwtest=%u: the hardware test runs after the home screen; results in this log\n", cfg.hwtest);
    }
    log_ok("boot_args at %08x: dt va %08x len %u\n", cfg.bootargs_pa, ba->deviceTreeP, dsize);

    /* XNU log ring (filled by the M2 console hook) */
    if (cfg.ring_size > sizeof(xnu_ring_t)) {
        xnu_ring_t *ring = (xnu_ring_t *)cfg.ring_pa;
        memset(ring, 0, cfg.ring_size);
        ring->size = cfg.ring_size - sizeof(xnu_ring_t);
        ring->magic = XNU_RING_MAGIC;
    }
    /* marker pattern in the page-table area: lets the monitor see the entry stub run */
    memset((void *)cfg.top_pa, 0xA5, 0x9000);
    /* Wi-Fi: wifi.cfg, WPA2 key derivation, network mailbox (docs/wifi.md) */
    {   /* scratch for unpacking sd:/ios3ds/nwm.code: the top 4 MiB of XNU's
           memory, unused until XNU starts (after this), never a load= target */
        u32 len = 0x400000, pa = cfg.phys_base + cfg.mem_size - len;
        bool ok = pa >= cfg.top_pa + 0x100000;
        for (u32 i = 0; i < cfg.nloads; i++)
            if (cfg.load_pa[i] >= pa - 0x100000 && cfg.load_pa[i] < pa + len) ok = false;
        wifi_init(ok ? pa : 0, ok ? len : 0);
    }

    audio_set_hp_codec(cfg.hp_codec);
    audio_init();                   /* CODEC sound path, before XNU (docs/audio.md) */
    {   /* time set in iOS Settings, saved by mcu.c, wins over boot.cfg rtc_offset */
        char t[24];
        int n = sd_read_small("0:/ios3ds/rtc_offset.txt", t, sizeof(t));
        if (n > 0) {
            int v = 0, neg = t[0] == '-';
            for (int i = neg; i < n && t[i] >= '0' && t[i] <= '9'; i++) v = v * 10 + (t[i] - '0');
            cfg.rtc_offset = neg ? -v : v;
            logp("[mcu] rtc_offset %d from sd:/ios3ds/rtc_offset.txt (time set in iOS)\n", (int)cfg.rtc_offset);
        }
    }
    mcu_init(cfg.shim_pa, cfg.mcu != 0, cfg.rtc_offset, T9_HZ);   /* RTC before the kernel's calendar (docs/mcu.md) */
    mcu_config_acc(cfg.mcu_acc != 0);
    gyro_init(cfg.shim_pa, cfg.gyro != 0, cfg.gyro_axes, T9_HZ);
    ps_init(cfg.powersave != 0, cfg.shim_pa, T9_HZ);              /* screen-off power saving */
    mcu_set_led(cfg.notify_led != 0);
    mcu_config_wifi_led(cfg.wifi_led != 0);
    audio_set_mic(cfg.mic != 0);
    audio_set_hp_route(cfg.hp_route != 0);
    camera_init(cfg.shim_pa, cfg.camera != 0, cfg.cam_test != 0);   /* docs/camera.md */
    if (cfg.stereo3d) {
        /* The GPU mirror stays on: with the slider down it copies the screen
           exactly as with stereo3d=0, and with the slider up its two-eye lists
           build both eyes on the PICA200 (docs/stereo3d.md). The ARM9 two-eye
           copy is only a fallback when the mirror stops. */
        log_color(CON_YELLOW, "[stereo] stereo3d=1: GPU display mirror stays on; the PICA200 builds both eyes with the slider up\n");
        display_set_stereo_depth(cfg.stereo_depth);
        logp("[stereo] depth strength %u%% (boot.cfg stereo_depth, 0..100): shift %d px at slider max\n",
             cfg.stereo_depth > 100 ? 100u : cfg.stereo_depth, stereo_depth_k(255, cfg.stereo_depth));
    }
    if (cfg.shim_pa) {              /* GPU display mirror mailbox (gpu_mirror.h), before XNU */
        u32 g[7] = { cfg.gpu, cfg.gpu_mirror, cfg.gpu_vbl_div, cfg.gpu_spin_us, cfg.gpu_clk, cfg.gpu_diag, cfg.ca_gpu };
        gm_init(cfg.shim_pa, T9_HZ, g, cfg.screen < SCREEN_MODES ? (int)cfg.screen : SCREEN_BOTH);
    }
    stamp("Wi-Fi, audio, MCU, GPU set up");
    n3ds_setup();                   /* model, 804 MHz, L2, timer calibration (docs/n3ds.md) */
    if (cfg.shim[0] && cfg.shim_pa)     /* SELECT+START word: outside shim.bin, whatever RAM held */
        *(vu32 *)(cfg.shim_pa + SHIM_PANIC_REQ) = 0;
    /* Ambient light only exists on New models; on an Old 3DS we do not even
       call this: nothing touches the camera bus and the mailbox stays zero
       (docs/mcu.md section 12). */
    if (n3ds_res & N3RES_NEW) mcu_set_als(cfg.auto_brightness != 0);
    exthid_init(cfg.n3ds_hid != 0 && (n3ds_res & N3RES_NEW), cfg.diag != 0, T9_HZ);
    stamp("New 3DS check");
    if (!cfg.jump) {
        log_warn("boot.cfg says jump=0: not starting XNU\n");
        monitor("jump disabled");
    }
    /* The wait is only a grace period (1 s by default): it gives a user time
       to hold B and stay in the monitor, and leaves the bottom screen readable.
       boot_delay=0 in boot.cfg or A during the countdown jumps at once. */
    if (!cfg.countdown_ms)
        log_ok("boot.cfg boot_delay=0: no countdown\n");
    for (u32 ms = cfg.countdown_ms; ms; ms = ms > 100 ? ms - 100 : 0) {
        status(10, CON_YELLOW, "jump to XNU in %u.%u s (A = skip, B = cancel)", ms / 1000, (ms % 1000) / 100);
        if (!(HID_PAD & HID_A)) {
            log_ok("A pressed: skipping the rest of the countdown\n");
            break;
        }
        if (!(HID_PAD & HID_B)) {
            log_warn("B held: jump cancelled\n");
            status(10, CON_YELLOW, "jump cancelled (B)");
            monitor("jump cancelled");
        }
        delay_ms(100);
    }

    stamp("countdown over");
    log_color(CON_GREEN, "jumping: ARM11 -> %08x, r0=%08x (boot_args)\n", cfg.entry_pa, cfg.bootargs_pa);
    log_flush();
    volatile shared_t *sh = SHARED;
    sh->jump_pc = cfg.entry_pa;
    sh->jump_r0 = cfg.bootargs_pa;
    sh->a9_cmd = A9CMD_JUMP;
    for (u32 t = 0; t < 1000 && sh->a11_ack != A9CMD_JUMP; t++) delay_ms(1);
    if (sh->a11_ack != A9CMD_JUMP) {
        log_err("ARM11 did not acknowledge the jump command\n");
    } else {
        log_ok("ARM11 acknowledged, now in XNU\n");
        stamp("XNU started");
    }
    status(10, CON_GREEN, "XNU started");
    monitor("XNU running");
}
