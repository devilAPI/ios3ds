/*
 * A deliberately small, integer-only power model used by the ARM9's opt-in
 * [pwr] telemetry line (boot/arm9/power.c, docs/mcu.md section 15).
 *
 * IMPORTANT: these are ORDER-OF-MAGNITUDE ASSUMPTIONS, not measurements. The
 * emulator has no current sensor and the MCU register 0x0D reads voltage, not
 * current; nothing here is calibrated against a console. The counters that
 * feed the model (screen/backlight, ARM9 loops, ARM11 IRQs, mirror frames,
 * camera/CSND/Wi-Fi state, MCU battery) are real and are logged with the
 * estimate, so a hardware run can replace the coefficients with measured ones.
 *
 * Shared with a host test (tests/test_power_model.py); no floats, no libc.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

#define PWR_CAP_OLD_MAH   1300u    /* Old 3DS / 3DS XL / 2DS battery, assumed */
#define PWR_CAP_NEW_MAH   1750u    /* New models, assumed */

typedef struct {
    uint32_t backlight;     /* 0..1000, 0 = screen off */
    uint32_t screen_on;
    uint32_t arm11_mhz;     /* 268 / 536 / 804 */
    uint32_t arm9_loops_s;  /* monitor loop iterations per second (WFI loops included) */
    uint32_t arm11_irqs_s;  /* shim IRQ entries per second */
    uint32_t gpu_frames_s;  /* completed PICA200 mirror frames per second */
    uint32_t camera_on;     /* inner camera AE for auto-brightness, or a preview */
    uint32_t csnd_on;       /* CSND channels playing */
    uint32_t wifi_link;     /* associated (frames may arrive at any time) */
    uint32_t wifi_radio;    /* chip powered, no link */
    uint32_t wifi_rec;      /* WMI REC_POWER requested while linked */
} pwr_state_t;

/* Current draw in mA. Every term is a guess; the *shape* is what the telemetry
   line is for: which block dominates idle, and what switching it off should
   save. Keep the arithmetic integer so the ARM9 build needs no floating point. */
static inline uint32_t pwr_estimate_ma(const pwr_state_t *s)
{
    uint32_t ma = 30u;                                   /* ARM9 board, MCU, SD, regulators */
    ma += 8u + (s->arm9_loops_s + 250u) / 500u;           /* ~10 mA busy, ~9 mA with WFI */
    ma += 10u + (s->arm11_mhz + 10u) / 20u;               /* 23 / 37 / 50 mA */
    ma += s->arm11_irqs_s / 32u;                          /* interrupt handling */
    if (s->screen_on)
        ma += 30u + (s->backlight * 120u + 500u) / 1000u; /* two LCD panels + PWM */
    if (s->gpu_frames_s) ma += 25u;                       /* PICA200 + VRAM traffic */
    if (s->camera_on)    ma += 25u;                       /* sensor + CAM block clock */
    if (s->csnd_on)      ma += 10u;                       /* CODEC + CSND DMA */
    if (s->wifi_link)    ma += s->wifi_rec ? 30u : 70u;   /* 802.11 PS vs max performance */
    else if (s->wifi_radio) ma += 15u;                    /* powered, idle */
    return ma;
}

/* Tenths of an hour from a battery capacity in mAh. */
static inline uint32_t pwr_hours_x10(uint32_t ma, uint32_t capacity_mah)
{
    return capacity_mah * 10u / (ma ? ma : 1u);
}
