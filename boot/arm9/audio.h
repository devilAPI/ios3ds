/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* CODEC sound path (DAC, speaker + headphone drivers, I2S1/I2S2, MCLK) as
   libn3ds CODEC_init/soundInit leaves it. Call once before XNU starts and
   before tsc_init (it soft-resets the CODEC). false = no CODEC answered. */
bool audio_init(void);

/* Monitor loop: report stream starts/stops seen by the shim's audio emulation
   (kernel/shim/audio.c) in the SD log. shim_pa = cfg.shim_pa (0 = no shim). */
void audio_poll(u32 shim_pa);

/* Opt-in idle policy (boot.cfg power_opt=1): clear CSND_CNT once the shim has
   stopped its channels and the screen is off; restore it on wake or on the
   next iOS output stream. True = the shim currently has CSND running. */
void audio_idle_gate(u32 shim_pa, bool off);
bool audio_csnd_active(u32 shim_pa);

/* boot.cfg mic=0: never touch the MIC block (iOS's input stays silent) */
void audio_set_mic(bool enabled);

/* boot.cfg hp_route=0: iOS is never told about the headphone jack and stays on
   its speaker route (the CODEC still switches the analogue output) */
void audio_set_hp_route(bool enabled);

/* boot.cfg hp_codec (call before audio_init): 1 (default) = libn3ds' headphone
   filters at init, the headphone driver re-powered on every plug-in, the Old
   3DS de-pop line driven low; 0 = only switch the CODEC's output select (0.4.0) */
void audio_set_hp_codec(u32 mode);
