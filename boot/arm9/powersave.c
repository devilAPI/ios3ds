/*
 * ios3ds ARM9: power saving while iOS' screen is off (docs/mcu.md section 9).
 *
 * "Off" = iOS had its display on and AppleM2CLCD has switched the (fake)
 * CLCD controller off: lock, auto-lock, shell closed (which locks). Then:
 *   - the screen copy stops on both paths: the ARM9 CPU copy follows
 *     display_active() already; the shim's GPU mirror gets want = 0 through
 *     its mailbox (gpu_mirror.h), the one flag it checks before every frame;
 *   - the touch ADC is not read (buttons, HOME/POWER and the shell still are);
 *   - once md0 has been quiet for 200 ms the monitor loop sleeps in
 *     wait-for-interrupt between iterations, woken by ARM9 timer 1 every 2 ms
 *     (IRQs stay masked in CPSR: the pending timer IRQ only ends the wait).
 *     A block request is served within 2 ms; the loop is busy again for the
 *     next 200 ms;
 *   - the shim masks the VBlank interrupt (mcubox ps_on, tools/ios3ds-mcu/
 *     shim/mcu.c) while the CLCD is off.
 * On wake everything is restored and the next loop copies a fresh frame.
 * boot.cfg powersave=0 turns all of it off.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "powersave.h"
#include "log.h"
#include "tsc.h"
#include "mcu.h"
#include "gpumirror.h"
#include "power.h"

#define T9_VAL(n)   (*(vu16 *)(0x10003000u + (n) * 4))
#define T9_CNT(n)   (*(vu16 *)(0x10003002u + (n) * 4))
#define IRQ_IE      (*(vu32 *)0x10001000u)
#define IRQ_IF      (*(vu32 *)0x10001004u)
#define IRQ_TIMER1  (1u << 9)
#define TICK_US     2000u       /* WFI wake-up period */

static bool enabled, off;          /* off is tracked (and logged) even with powersave=0, for comparison */
static u32 hz, shim;
static u32 loops, sleeps;
static u32 t_mark, loops_mark, sleeps_mark, irq_mark, vbl_mark, tmr_mark;
static u32 on_loops_per_s, on_irqs_per_s;

static u32 rd(u32 off_) { return shim ? *(vu32 *)(shim + off_) : 0; }     /* shim stats, entry.S */

void ps_init(bool enable, u32 shim_pa, u32 ticks_per_s)
{
    enabled = enable && shim_pa;
    if (!shim_pa) return;
    shim = shim_pa;
    hz = ticks_per_s;
    mcu_set_powersave(enabled);
    if (!enable) log_warn("[power] power saving off (boot.cfg powersave=0)\n");
}

static void mark(u32 now)
{
    t_mark = now;
    loops_mark = loops;
    sleeps_mark = sleeps;
    irq_mark = rd(0x98);
    vbl_mark = rd(0x78);
    tmr_mark = rd(0x4C);
}

/* rates since mark(), per second */
static void rates(u32 now, u32 *lps, u32 *sps, u32 *ips, u32 *vps, u32 *tps)
{
    u32 dt = now - t_mark;
    if (!dt) dt = 1;
#define R(x) (u32)((u64)(x) * hz / dt)
    *lps = R(loops - loops_mark);
    *sps = R(sleeps - sleeps_mark);
    *ips = R(rd(0x98) - irq_mark);
    *vps = R(rd(0x78) - vbl_mark);
    *tps = R(rd(0x4C) - tmr_mark);
#undef R
}

static void timer_on(bool on)
{
    if (on) {
        T9_CNT(1) = 0;
        T9_VAL(1) = (u16)(0x10000u - TICK_US * 1047u / 1000u);  /* 67027964 / 64 = 1.047 MHz */
        IRQ_IF = IRQ_TIMER1;
        IRQ_IE |= IRQ_TIMER1;
        T9_CNT(1) = 0x80 | 0x40 | 1;                            /* start, IRQ, prescaler 64 */
    } else {
        T9_CNT(1) = 0;
        IRQ_IE &= ~IRQ_TIMER1;
        IRQ_IF = IRQ_TIMER1;
    }
}

bool ps_update(bool display_on, bool ios_display, u32 now)
{
    loops++;
    if (!shim || !ios_display) return false;
    if (!display_on && !off) {
        u32 l, s, i, v, t;
        rates(now, &l, &s, &i, &v, &t);
        on_loops_per_s = l;
        on_irqs_per_s = i;
        off = true;
        if (enabled) {
            gm_want(false);
            pwr_display_change(true);   /* opt-in: gate unused blocks / lower the clock */
            input_set_idle(true);
            mcu_screen_off(true);
            timer_on(true);
        }
        mark(now);
        log_color(CON_GREY, "[power] iOS screen off: %s (screen on: %u loops/s, ARM11 %u IRQs/s, %u VBlanks/s)\n",
                  enabled ? "copies paused, touch idle, ARM9 sleeps between polls" : "powersave=0, nothing changed",
                  l, i, v);
        return false;
    }
    if (display_on && off) {
        u32 l, s, i, v, t;
        rates(now, &l, &s, &i, &v, &t);
        off = false;
        if (enabled) {
            timer_on(false);
            mcu_screen_off(false);
            pwr_display_change(false); /* opt-in: restore blocks before the mirror restarts */
            input_set_idle(false);
            gm_want(true);
        }
        log_color(CON_GREY, "[power] iOS screen on again after %u s off: ARM9 %u loops/s (%u in WFI), "
                  "ARM11 %u IRQs/s (%u VBlank, %u timer); screen on was %u loops/s, %u IRQs/s\n",
                  (now - t_mark) / hz, l, s, i, v, t, on_loops_per_s, on_irqs_per_s);
        mark(now);
        return true;                    /* the caller copies a fresh frame now */
    }
    if (!off && now - t_mark >= 30 * hz) mark(now);    /* keep the "screen on" rate recent */
    return false;
}

bool ps_sleep(bool busy, bool io_recent)
{
    if (!enabled || !off || busy || io_recent) return false;
    IRQ_IF = IRQ_TIMER1;
    __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0) : "memory");    /* wait for interrupt */
    IRQ_IF = IRQ_TIMER1;
    sleeps++;
    return true;
}

bool ps_off(void) { return off; }
