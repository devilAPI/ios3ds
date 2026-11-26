/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
#define SCREEN_TOP    0   /* iOS on the top screen, monitor log on the bottom */
#define SCREEN_BOTH   1   /* iOS on both; the bottom copy is the touch map (tap what you see) */
#define SCREEN_BOTTOM 2   /* iOS on the bottom (touch) screen only, log on the top */
#define SCREEN_MODES  3
void display_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1);
void display_set_mode(int mode);
bool display_active(u32 regs_pa);
void display_blit(u32 regs_pa);
void display_arm(u32 regs_pa);
void display_fbs(u32 out[4]);
void display_set_shown(u32 mask);   /* bit 0/1 top set 0/1, bit 2/3 bottom set 0/1 */
/* stereo3d=1 (boot.cfg) + the current MCU 3D slider, called every monitor
   loop: with a stamped home frame the ARM9 copy builds the two eyes and the
   top LCD goes into 3D mode; otherwise the normal copy stays untouched. */
void display_set_stereo(bool on, u32 slider);
/* Sample the display source once per copy interval and switch the top screen
   between the flat GPU mirror and 3D. Returns true while a stamped home
   frame is being shown in stereo. */
bool display_stereo_probe(u32 regs_pa);
/* cumulative work of the two-eye path: columns rebuilt vs reused unchanged */
void display_stereo_stats(u32 *rebuilt, u32 *skipped);
/* boot.cfg stereo_depth (0..100): scales the slider's shift, 0 = flat */
void display_set_stereo_depth(u32 depth);
/* transitions where 3D was held through a short flat stretch (hysteresis) */
u32 display_stereo_holds(void);
/* called with blksrv_poll() every few columns of a CPU copy */
void display_set_poll(void (*fn)(void));
