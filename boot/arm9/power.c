/*
 * ios3ds ARM9: opt-in idle power policy (docs/mcu.md section 15).
 *
 * Everything here is off by default: boot.cfg power_opt=1 enables the
 * telemetry and the block gating, and idle_clock=1 additionally asks the
 * optional shim helper to lower the New 3DS ARM11 clock from 804 to 536 MHz
 * while iOS' screen is off. The built-in 4.2.1 boot.cfg does not contain
 * either key; `IOS3DS_IDLE=1 make sdimg` adds them to a developer image and
 * compiles the shim helper (scripts/mkboot.py).
 *
 * The policy only acts on the screen-off state that powersave.c already
 * tracks (lock / auto-lock / lid closed). It never turns off the display or
 * Wi-Fi on its own, and it restores every block before the mirror restarts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "power.h"
#include "bootcfg.h"
#include "log.h"
#include "powersave.h"
#include "gpumirror.h"
#include "gpu_mirror.h"
#include "camera.h"
#include "audio.h"
#include "wifi/wifi.h"
#include "mcu.h"
#include "../../boot/common/mcu_proto.h"
#include "../../boot/common/wifi_proto.h"
#include "../../boot/common/power_model.h"

#define PDN_SOCMODE     (*(vu16 *)0x10141300u)
#define PDN_GPU_CNT     (*(vu32 *)0x10141200u)
#define GPU_CNT_ON      0x1007Fu
#define SHIM_IDLE_REQ_OFF 0x6Cu          /* shim+0x6C: unused stats word ('IDL' request) */
#define IDLE_REQ_MAGIC  0x49440000u      /* 'ID\0\0'; low 12 bits = prescaler | shift<<8 */
#define IDLE_REQ_REFRESH (1u << 12)      /* set after PDN_SOCMODE ack: refresh the timer rate */
#define N3_PRE_804      191u
#define N3_SHIFT_804    1u
#define N3_PRE_536      255u
#define N3_SHIFT_536    0u

static bool enabled, new3ds, has_804, want_clock, l2_on, als_saved;
static u32 shim, hz, loops, t_report, t_prev;
static u32 irq_prev, tmr_prev, vbl_prev, dec_prev, frames_prev;
static u32 gpu_saved, gpu_gated, pending_gpu_gate, clock_low, wifi_rec;

static inline u32 rd(u32 off) { return shim ? *(vu32 *)(shim + off) : 0; }
/* ~1 ms at the ARM9's fixed 134 MHz (~33500 nops), as in audio.c's ms(). */
static void delay_ms(u32 n) { while (n--) __asm__ volatile("nop"); }
#define SPIN_MS() delay_ms(33500u)

/* The shim helper runs on the next ARM11 timer interrupt, which still fires
   while the screen is off. It rewrites cfg_n3ds (+0x0C) after reprogramming
   the MPCore timers; polling that word is our acknowledgement. */
static bool clock_request(u32 pre, u32 shift)
{
    volatile u32 *req = (volatile u32 *)(shim + SHIM_IDLE_REQ_OFF);
    u32 old = rd(SHIM_N3DS_OFF);
    u32 base = old & ~(0xFFu | 0xF00u);
    u32 target = base | (pre & 0xFFu) | ((shift & 0xFu) << 8);
    volatile vu16 *mode = &PDN_SOCMODE;

    if (!want_clock || !has_804 || !shim) return false;
    if (rd(SHIM_N3DS_OFF) == target) return true;
    *req = IDLE_REQ_MAGIC | (pre & 0xFFu) | ((shift & 0xFu) << 8);
    for (u32 i = 0; i < 200 && rd(SHIM_N3DS_OFF) != target; i++) SPIN_MS();
    if (rd(SHIM_N3DS_OFF) != target) {
        *req = 0;
        log_warn("[pwr] no shim idle-clock support (cfg_n3ds still %08x): clock unchanged\n",
                 (unsigned)rd(SHIM_N3DS_OFF));
        want_clock = false;             /* do not ask again every wake */
        return false;
    }
    *req = 0;
    /* PDN_LGR_SOCMODE 5 = 804 MHz, 3 = 536 MHz on LGR2 models (docs/n3ds.md).
       Ack bit 15 is set by the hardware a moment after the write; the helper
       in the GPU part has already rescaled the MPCore timers. */
    *mode = clock_low ? 5u : 3u;
    for (u32 i = 0; i < 200 && !(*mode & 0x8000u); i++) SPIN_MS();
    if (!(*mode & 0x8000u)) {
        log_warn("[pwr] PDN_LGR_SOCMODE %u not acknowledged: restoring the timer setup\n",
                 clock_low ? 5u : 3u);
        *req = (old & 0xFFFu) | IDLE_REQ_MAGIC;
        for (u32 i = 0; i < 200 && rd(SHIM_N3DS_OFF) != old; i++) SPIN_MS();
        *req = 0;
        return false;
    }
    *mode = *mode;                      /* acknowledge */
    clock_low = !clock_low;
    /* PERIPHCLK changed when the mode took effect. The shim's next timer IRQ
       rewrites the MPCore control words so the watchdog's rate follows too
       (the private timer is rewritten by every set_decrementer anyway). */
    *req = IDLE_REQ_MAGIC | (pre & 0xFFu) | ((shift & 0xFu) << 8) | IDLE_REQ_REFRESH;
    for (u32 i = 0; i < 200 && (*req & IDLE_REQ_REFRESH); i++) SPIN_MS();
    if (*req & IDLE_REQ_REFRESH)
        log_warn("[pwr] timer rate refresh not acknowledged (clock changed anyway)\n");
    *req = 0;
    log_ok("[pwr] ARM11 clock %s: PDN_LGR_SOCMODE %u, timer prescaler %u, tick shift %u\n",
           clock_low ? "536 MHz (idle)" : "804 MHz", clock_low ? 3u : 5u, pre, shift);
    return true;
}

static void gpu_gate(bool off)
{
    if (!enabled || (!gpu_saved && !off)) return;
    volatile gmbox_t *gm = (volatile gmbox_t *)(shim + SHIM_GM_OFF);
    if (off) {
        /* ps_update() sets want=0 first; the shim changes state ACTIVE -> WAIT
           on its next thread poll. Both are safe once no frame is in flight. */
        if (gpu_gated || (gm->state != GM_ST_ACTIVE && gm->state != GM_ST_WAIT) || gm->mirror_busy) return;
        gpu_saved = PDN_GPU_CNT;
        PDN_GPU_CNT = 0;                /* clock off, blocks held in reset */
        gm->idle_gate = 1;
        gm->idle_saved = gpu_saved;
        gpu_gated = 1;
        logp("[pwr] GPU clock gated while the screen is off (PDN_GPU_CNT %08x -> 0)\n",
             (unsigned)gpu_saved);
    } else if (gpu_gated) {
        PDN_GPU_CNT = gpu_saved ? gpu_saved : GPU_CNT_ON;
        gm->idle_gate = 0;              /* the shim also clears this on a GL request */
        gm->idle_saved = 0;
        gpu_gated = 0;
        logp("[pwr] GPU clock restored before the display mirror (%08x)\n", (unsigned)PDN_GPU_CNT);
    }
}

void pwr_init(u32 shim_pa, bool enable, bool is_new, bool has804, bool idle, bool l2, u32 hz_)
{
    hz = hz_;
    new3ds = is_new;
    has_804 = has804;
    l2_on = l2;
    want_clock = idle;
    if (!enable) {
        if (idle || cfg.power_opt)
            log_warn("[pwr] needs powersave=1 and power_opt=1; idle policy off\n");
        return;
    }
    if (!shim_pa) {
        log_warn("[pwr] no shim: idle policy off\n");
        return;
    }
    enabled = true;
    shim = shim_pa;
    t_report = t_prev = 0;
    if (want_clock && (!has_804 || l2_on)) {
        log_warn("[pwr] idle_clock needs a New 3DS at 804 MHz with n3ds_l2=0; ignored\n");
        want_clock = false;
    }
    log_color(CON_GREY, "[pwr] idle policy on: telemetry every 10 s; camera/CSND/Wi-Fi/GPU gating"
              "%s; battery %u%% %u mV\n", want_clock ? ", idle ARM11 clock" : "",
              (unsigned)((volatile mcubox_t *)(shim + SHIM_MCU_OFF))->batt_percent,
              (unsigned)((volatile mcubox_t *)(shim + SHIM_MCU_OFF))->batt_volt * 20u);
}

void pwr_display_change(bool off)
{
    if (!enabled) return;
    if (off) {
        pending_gpu_gate = 1;
        if (new3ds && cfg.auto_brightness) {
            mcu_set_als(false);         /* standby the inner camera AE sensor */
            als_saved = true;
        }
        audio_idle_gate(shim, true);    /* CSND/DAC once iOS stops playing */
        wifi_idle(true);                /* WMI REC_POWER on the AR6014 */
        wifi_rec = 1;
        clock_request(N3_PRE_536, N3_SHIFT_536);
    } else {
        if (clock_low) clock_request(N3_PRE_804, N3_SHIFT_804);
        gpu_gate(false);
        audio_idle_gate(shim, false);
        if (als_saved) {
            mcu_set_als(cfg.auto_brightness != 0);
            als_saved = false;
        }
        wifi_idle(false);
        wifi_rec = 0;
        pending_gpu_gate = 0;
    }
}

void pwr_poll(u32 now)
{
    if (!enabled) return;
    loops++;
    if (pending_gpu_gate) gpu_gate(true);       /* wait for a mirror frame to finish */
    if (ps_off()) audio_idle_gate(shim, true);  /* a stream may have stopped after the lock */
    if (!t_prev) { t_prev = now; return; }
    if (now - t_report < 10u * hz) return;

    u32 dt = now - t_prev;
    u32 ni = rd(0x98), nt = rd(0x4C), nv = rd(0x78), nd = rd(0x8C);
    volatile gmbox_t *gm = (volatile gmbox_t *)(shim + SHIM_GM_OFF);
    u32 nf = gm->frames;
    u32 H = hz;
    u32 ips = (ni - irq_prev) * H / dt, tps = (nt - tmr_prev) * H / dt;
    u32 vps = (nv - vbl_prev) * H / dt, dps = (nd - dec_prev) * H / dt;
    u32 fps = (nf - frames_prev) * H / dt;
    irq_prev = ni; tmr_prev = nt; vbl_prev = nv; dec_prev = nd; frames_prev = nf;
    t_prev = now; t_report = now;

    volatile mcubox_t *m = (volatile mcubox_t *)(shim + SHIM_MCU_OFF);
    volatile netmbox_t *n = (volatile netmbox_t *)NET_PA;
    pwr_state_t s;
    bool is_off = ps_off();
    s.backlight = is_off ? 0u : (m->backlight ? m->backlight - 1u : 500u);
    s.screen_on = !is_off;
    s.arm11_mhz = clock_low ? 536u : (has_804 ? 804u : 268u);
    s.arm9_loops_s = loops * H / dt;
    loops = 0;
    s.arm11_irqs_s = ips;
    s.gpu_frames_s = fps;
    s.camera_on = m->als_state == 1u || (gm->state == GM_ST_ACTIVE && camera_als_busy());
    s.csnd_on = audio_csnd_active(shim);
    s.wifi_link = n->link != 0;
    s.wifi_radio = n->backend != NET_BACKEND_NONE;
    s.wifi_rec = wifi_rec;
    u32 ma = pwr_estimate_ma(&s);
    u32 h10_old = pwr_hours_x10(ma, PWR_CAP_OLD_MAH);
    u32 h10_new = pwr_hours_x10(ma, PWR_CAP_NEW_MAH);
    log_color(CON_GREY, "[pwr] %s: ~%u mA modeled (%u.%u h / %u.%u h assumed 1300/1750 mAh): "
              "ARM9 %u loops/s, ARM11 %u irq/s (%u timer, %u VBlank, %u dec/s), GPU %u frames/s, "
              "bl %u/1000, cam %u, CSND %u, wifi link %u ps %u, batt %u%% %u mV%s\n",
              is_off ? "screen off" : "screen on", ma,
              h10_old / 10u, h10_old % 10u, h10_new / 10u, h10_new % 10u,
              (unsigned)s.arm9_loops_s, (unsigned)ips, (unsigned)tps, (unsigned)vps, (unsigned)dps,
              (unsigned)fps, (unsigned)s.backlight, (unsigned)s.camera_on, (unsigned)s.csnd_on,
              (unsigned)s.wifi_link, (unsigned)s.wifi_rec, m->batt_percent, (unsigned)(m->batt_volt * 20u),
              m->flags & MCUF_CHARGING ? ", charging" : "");
}
