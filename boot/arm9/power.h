/* ios3ds ARM9: opt-in idle power policy and telemetry.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* Call after the model check. Enables nothing unless boot.cfg power_opt=1
 * (and powersave=1); the boot.cfg keys default to 0. */
void pwr_init(u32 shim_pa, bool enable, bool new3ds, bool has_804, bool idle_clock, bool l2, u32 hz);
/* Every monitor loop; prints the [pwr] estimate every 10 s when enabled. */
void pwr_poll(u32 now);
/* Called from powersave.c at the screen-off / wake transitions. */
void pwr_display_change(bool off);
