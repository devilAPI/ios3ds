/*
 * ios3ds audio: definitions shared by the kernel shim (kernel/shim/audio.c), the ARM9
 * monitor (boot/arm9/audio.c), scripts/mkboot.py (patches aud_cfg_t by magic)
 * and the host tests. See docs/audio.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* ---- configuration block inside shim.bin, located by its two magic words.
   mkboot fills dmac_pa / i2s_pa (fake S5L8720 register pages, kernel/dtgen.py);
   dmac_pa == 0 disables everything. ---------------------------------------- */
#define AUD_CFG_MAGIC0  0x30445541u     /* 'AUD0' */
#define AUD_CFG_MAGIC1  0x21676663u     /* 'cfg!' */
#define AUD_F_CSND      (1u << 0)       /* drive the 3DS CSND block */
#define AUD_F_LOG       (1u << 1)       /* trace stream starts/stops and I2C writes into the XNU log ring */

typedef struct {
    uint32_t magic0, magic1;
    uint32_t dmac_pa;       /* fake PL080 page (PA) */
    uint32_t i2s_pa;        /* fake S5L I2S page (PA), informational */
    uint32_t pcm_pa;        /* CSND sample memory (PA, FCRAM): L ring then R ring */
    uint32_t ring_frames;   /* frames per CSND ring (power of two) */
    uint32_t lead_frames;   /* how far ahead of CSND's play position we write */
    uint32_t rate_div;      /* CSND timer divider: Fs = 67027964 / rate_div (1520 -> 44097.3 Hz) */
    uint32_t flags;         /* AUD_F_* */
    uint32_t stat_off;      /* offset of the aud_stat_t that follows, from the first magic (bytes) */
    uint32_t gpio_pa;       /* fake S5L GPIO page (PA): the headphone-detect pin lives there (mkboot) */
} aud_cfg_t;

/* ---- statistics, written by the shim, read by the ARM9 monitor (the shim
   cleans the cache lines after every update that matters) ------------------ */
typedef struct {
    uint32_t services;      /* emulator runs (one per IRQ taken) */
    uint32_t starts;        /* I2S TX DMA channel went idle -> running */
    uint32_t stops;
    uint32_t tx_words;      /* 16-bit samples consumed from iOS's ring */
    uint32_t rx_words;      /* 16-bit samples of silence delivered to the input stream */
    uint32_t tc_irqs;       /* terminal-count interrupts raised */
    uint32_t injected;      /* ... and delivered through the fake VIC */
    uint32_t early_wakes;   /* private-timer IRQs taken only for audio */
    uint32_t csnd_starts;
    uint32_t csnd_resyncs;  /* write pointer re-aligned to CSND's position register */
    uint32_t bad_addr;      /* DMA addresses outside FCRAM (skipped) */
    uint32_t iic_writes, iic_reads;
    uint32_t peak;          /* max |sample| since the last start: nonzero = iOS really plays something */
    uint32_t last_src;      /* last source PA consumed */
    uint32_t state;         /* bit0 TX running, bit1 RX running, bit2 CSND running, bit3 IO window mapped */
    uint32_t amp_step;      /* last LM48821 gain step iOS wrote (0..31), AUD_AMP_OFF, AUD_AMP_NONE */
    uint32_t volume;        /* CSND channel volume derived from it (0..0x8000) */
    uint32_t mic_samples;   /* microphone samples handed to iOS's input stream */
    uint32_t mic_underruns; /* input words that had to repeat the last sample (ring empty) */
    uint32_t mic_resyncs;   /* read position re-aligned to the ARM9's write position */
    uint32_t hp_state;      /* headphones as iOS sees them: 0 out, 1 in, AUD_HP_OFF = route switching off */
    uint32_t hp_changes;    /* jack changes passed to iOS (detect pin + interrupt) */
    uint32_t hp_vol;        /* last CS42L58 headphone volume (reg 0x1a) iOS wrote, AUD_HPVOL_NONE */
} aud_stat_t;

typedef struct {
    aud_cfg_t cfg;
    aud_stat_t stat;
} aud_blk_t;

/* ---- fixed placement (PA) of the CSND sample memory: FCRAM between the XNU
   log ring (ends 0x20890010) and topOfKernelData (0x20900000); like the shim
   and the ring it is mapped by XNU's static map and never handed to the VM. - */
#define AUD_PCM_PA        0x208C0000u
#define AUD_RING_FRAMES   8192u         /* 186 ms at 44.1 kHz; 2 x 16 KiB */
#define AUD_LEAD_FRAMES   2048u         /* 46 ms of output latency */
#define AUD_RATE_DIV      1520u         /* 67027964 / 1520 = 44097.34 Hz */
#define AUD_AMP_NONE      0xFFu         /* aud_stat_t.amp_step: iOS has not set a level yet */
#define AUD_AMP_OFF       0x100u        /* ... the amp was shut down (volume 0) */
#define AUD_AMP_REF_STEP  19u           /* LM48821 step 19 (0 dB) = CSND unity: with the N81 routing (VirtualAudio patch) iOS writes a fixed step 19 for media and sets volume in software; louder steps clamp (audio.c amp_gain) */

/* ---- 3DS CSND (0x10103000), from libn3ds include/arm11/drivers/csnd.h ----- */
#define CSND_PA           0x10103000u
#define CSND_MASTER_VOL   0x000   /* u16, 0..0x8000 */
#define CSND_CNT          0x002   /* u16: bit15 enable, bit14 resample filter, bit0 mute */
#define CSND_CH(n)        (0x400 + (n) * 0x20)
#define CSND_CH_CNT       0x00    /* u16 */
#define CSND_CH_SR        0x02    /* u16: 0x10000 - 67027964 / Fs */
#define CSND_CH_VOL       0x04    /* u32: R (low 16), L (high 16), 0..0x8000 */
#define CSND_CH_CAPVOL    0x08
#define CSND_CH_ADDR      0x0C    /* start address / current play position (PA) */
#define CSND_CH_SIZE      0x10    /* bytes */
#define CSND_CH_LOOP      0x14    /* loop restart address */
#define CSND_CH_ADPCM0    0x18
#define CSND_CH_ADPCM1    0x1C
#define CSND_CNT_EN           (1u << 15)
#define CSND_CNT_RS_FILTER    (1u << 14)
#define CSND_CH_LERP          (1u << 6)
#define CSND_CH_RPT_LOOP      (1u << 10)
#define CSND_CH_FMT_PCM16     (1u << 12)
#define CSND_CH_PLAYING       (1u << 14)
#define CSND_CH_START         (1u << 15)
#define CSND_CLOCK            67027964u     /* = ARM11 clock / 4 */
/* ---- I2S1 output + CODEC master clock (libn3ds i2s.h, pdn.h); the ARM9
   (boot/arm9/audio.c) and the shim (csnd_start) both write them ------------ */
#define I2S1_CNT_PA       0x10145000u   /* u16 */
#define I2S2_CNT_PA       0x10145002u   /* u16 */
#define PDN_I2S_CNT_PA    0x10141220u   /* u8 */
#define PDN_I2S_CLK2_EN   (1u << 1)     /* MCLK2 -> CODEC */
#define I2S1_EN           (1u << 15)
#define I2S1_MCLK_16MHZ   (1u << 14)
#define I2S1_FREQ_47KHZ   (1u << 13)
/* enabled, 16 MHz MCLK, 47605 Hz, legacy (GBA/DS) volume 32, DSP/CSND volume 32 */
#define AUD_I2S1_CNT      (I2S1_EN | I2S1_MCLK_16MHZ | I2S1_FREQ_47KHZ | (32u << 6) | 32u)
#define AUD_I2S2_CNT      (1u << 15 | 1u << 14 | 1u << 13)  /* I2S2 (mic/camera side) as libn3ds */
#define AUD_CH_L          0
#define AUD_CH_R          1

/* ---- microphone: the shim drains the 3DS MIC FIFO itself (ARM11, every
   service: 1 ms while iOS's input stream runs) into a ring in FCRAM and feeds
   iOS's I2S RX emulation from it. The ARM9 only powers the CODEC's ADC while
   "want" is set and logs; it reads the shim's lines from RAM (the shim cleans
   them) and writes only its own line, which the shim invalidates before reading.
   Placement: after the CSND rings, below topOfKernelData. -------------------- */
#define AUD_MIC_PA        0x208C8000u
#define AUD_MIC_SAMPLES   8192u         /* power of two; 0.69 s at 11.9 kHz */
#define AUD_MIC_MAGIC     0x3043494Du   /* 'MIC0' */
#define AUD_MIC_OFF       0             /* aud_mic_t.state */
#define AUD_MIC_RUNNING   1
#define AUD_MIC_FAILED    2             /* the MIC block did not deliver: input stays silent */
#define AUD_MIC_DISABLED  3             /* boot.cfg mic=0 */
typedef struct {
    /* written by the shim (cache line 0) */
    uint32_t magic;         /* AUD_MIC_MAGIC once the shim set it up */
    uint32_t state;         /* AUD_MIC_* */
    uint32_t rate;          /* samples per second while running */
    uint32_t wr;            /* samples written so far (free-running; ring index = wr % AUD_MIC_SAMPLES) */
    uint32_t overruns;      /* MIC FIFO overrun flags seen (samples lost between two services) */
    uint32_t starts;
    uint32_t want;          /* 1 = iOS records: the ARM9 keeps the CODEC's ADC powered */
    uint32_t why;           /* AUD_MIC_FAILED: 1 no data, 2 FIFO never empties, 3 block off, 4 no IO window */
    /* written by the shim (cache lines 1-2): one-second level report, see [mic]
       in the ARM9 log. The ARM9 computes RMS from the sum of squares (u64) so
       the interrupt-time shim needs no 64-bit division or integer sqrt. */
    uint32_t diag_seq;      /* incremented when the report below changes */
    uint32_t diag_raw_peak; /* at the MIC FIFO (before the shim's gain compensation) */
    uint32_t diag_raw_n;    /* raw halfwords accumulated */
    uint32_t diag_raw_sq_lo, diag_raw_sq_hi;
    uint32_t diag_out_peak; /* delivered to iOS's input stream (after compensation) */
    uint32_t diag_out_sq_lo, diag_out_sq_hi;
    uint32_t diag_frames;   /* frames accumulated */
    uint32_t diag_pad[3];
    /* written by the ARM9 (cache line 3) */
    uint32_t arm9_magic;    /* AUD_MIC_MAGIC: the next field is valid */
    uint32_t disabled;      /* boot.cfg mic=0: the shim never touches the MIC block */
    uint32_t adc_on;        /* the ARM9 has powered the ADC */
    uint32_t jack;          /* 1 = headphones plugged (GPIO2 bit 0, debounced by the ARM9) */
    uint32_t hp_disabled;   /* boot.cfg hp_route=0: iOS never hears about the jack */
    uint32_t pad1[3];
    int16_t s[AUD_MIC_SAMPLES];
} aud_mic_t;
/* 3DS MIC block (GBATEK "DSi Microphone", same design on the 3DS) */
#define MIC_CNT_PA        0x10162000u   /* u16 */
#define MIC_DATA_PA       0x10162004u   /* u32: two samples, older one low */
#define MIC_CNT_EN        (1u << 15)
/* MIC_CNT bits 0-1 = data format (GBATEK DSi Microphone): 0 duplicates each
   mono sample ("MakeStereo"), 2 = Normal (two consecutive 16-bit samples).
   The shim reads both halves of MIC_DATA as consecutive samples, so it must
   ask for Normal; MakeStereo would play every sample twice at half rate. */
#define MIC_CNT_FMT(n)    ((n) & 3u)
#define MIC_CNT_FMT_NORMAL 2u
#define MIC_CNT_CLEAR     (1u << 12)
#define MIC_CNT_OVERRUN   (1u << 11)
#define MIC_CNT_EMPTY     (1u << 8)
#define MIC_CNT_DIV(n)    ((n) << 2)    /* rate = I2S2 rate / (1 + n) */
#define AUD_MIC_DIV       3u            /* 47605 / 4 = 11901 Hz: the FIFO (32 samples) lasts 2.7 ms */
#define AUD_MIC_WORDS_MAX 64u           /* per service: a FIFO that never reports empty is broken (it holds 16) */
#define AUD_MIC_NODATA_TICKS 262000u    /* 250 ms without a sample after enabling -> failed */
#define AUD_I2S2_HZ       47605u
/* iOS's input chain (VirtualAudio, route SpeakerAndMicrophone) applies a fixed
   +23.65 dB (x15.23, measured in QEMU: 1000 -> 15232); the shim takes it off
   again so apps see the 3DS mic at the level the CODEC's PGA gives it */
#ifdef IOS3DS_KPI_MIC_GAIN_Q15
/* versions.py mic_gain_q15: 3.1.3's chain (product slot N82) adds no such gain */
#define AUD_MIC_GAIN_Q15  IOS3DS_KPI_MIC_GAIN_Q15
#else
#define AUD_MIC_GAIN_Q15  2151u
#endif
#define AUD_MIC_HOLD_TICKS 1047000u     /* keep the mic on 1 s after the input stream stopped */

/* ---- headphone detection for iOS (tools/ios3ds-audio/dt_audio.py): audio0's
   function-hp_detect is pin 0x0d00 of the fake GPIO page (data word +0x1a4,
   bit 0, 1 = headphones) and its interrupt VIC0 source 12; the shim mirrors
   aud_mic_t.jack into both. ----------------------------------------------- */
#define AUD_HP_GPIO_OFF   0x1A4u        /* port 13 * 0x20 + 4 */
#define AUD_HP_GPIO_BIT   (1u << 0)
#define AUD_HP_VIC_SRC    12u
#define AUD_HP_OFF        0x100u        /* aud_stat_t.hp_state: boot.cfg hp_route=0 */
/* On its headphone route iOS sets the volume on the CS42L58 (reg 0x1a, 1 dB
   steps: bits 0-6 = dB as 7-bit two's complement, -60 = 0x44 at volume 0,
   +2 dB = 0x02 at full volume; bit 7 = mute) and plays the PCM unscaled;
   the speaker amp is shut down (0x04). The shim then takes CSND's volume
   from that register (AUD_HPVOL_REF_DB = unity) instead of the amp. */
#define AUD_HPVOL_REG     0x1Au
#define AUD_HPVOL_REF_DB  2             /* iOS's full headphone volume (dB) = CSND unity */
#define AUD_HPVOL_NONE    0x100u        /* aud_stat_t.hp_vol: iOS has not written it */
