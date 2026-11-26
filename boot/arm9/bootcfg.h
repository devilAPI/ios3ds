/*
 * boot.cfg: the ARM9 monitor's configuration (docs/m1-design.md).
 * Kept in its own translation unit so the key parsing can be host-tested
 * (tests/test_bootcfg.py) without the 3DS hardware that main.c needs.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"

typedef struct {
    char kernel[64], devtree[64], shim[64], rootfs[64];
    u32 shim_pa;
    u32 kernel_pa, entry_pa, devtree_pa, bootargs_pa, ring_pa, ring_size, top_pa;
    u32 phys_base, virt_base, mem_size;
    u32 video_pa, video_display, video_rowbytes, video_width, video_height, video_depth;
    u32 jump, countdown_ms;
    u32 ba_revision, ba_version;
    u32 probes;
    u32 display_regs;
    u32 screen;          /* SCREEN_* start mode; X cycles it at run time */
    u32 gpu, gpu_mirror, gpu_vbl_div, gpu_spin_us, gpu_clk, gpu_diag, ca_gpu;  /* PICA200 (gpumirror.c, docs/pica200.md) */
    u32 mcu;             /* 0: never touch the MCU (docs/mcu.md) */
    u32 mcu_acc;         /* 0: no accelerometer for iOS */
    u32 gyro;            /* 0: no gyroscope for iOS (docs/mcu.md section 11) */
    char gyro_axes[16];  /* iOS x,y,z from the gyro chip's axes, e.g. -y,x,z */
    u32 powersave;       /* 0: no power saving while iOS' screen is off */
    u32 notify_led;      /* 1: notification LED blinks red on low battery */
    u32 wifi_led;        /* 1 (default): MCU Wi-Fi LED follows the link; 0 disables it */
    u32 diag;            /* 1: diagnostics on (boot-arg ios3ds_diag=1 for the test daemons, docs/boot-perf.md) */
    u32 ioprof;          /* md0 profile: 1 = [io] per second, 2 = + [blk] per request */
    u32 log_ts;          /* 1: "[s.ms]" timestamps on forwarded console lines */
    int32_t rtc_offset;  /* seconds added to the MCU RTC to get UTC */
    u32 mic;             /* 0 = never touch the MIC block (docs/audio.md) */
    u32 camera;          /* 0 = never touch the cameras (docs/camera.md) */
    u32 cam_test;        /* 1 = camera self-test matrix before XNU (~6 s; docs/camera.md section 7) */
    u32 hp_route;        /* 0 = iOS stays on its speaker route whatever the jack says */
    u32 hp_codec;        /* 0 = CODEC headphone path as in 0.4.0 (audio.h audio_set_hp_codec) */
    char gps[128];       /* boot.cfg gps=LAT,LON[,ACC]; passed to iOS as ios3ds_gps= */
    u32 n3ds_clock;      /* New 3DS: 1 = ARM11 at 804 MHz (docs/n3ds.md) */
    u32 n3ds_l2;         /* New 3DS: 1 = L2C-310 L2 cache on, write-through (experimental) */
    u32 n3ds_hid;        /* New 3DS: 0 = never read the C-stick / ZL / ZR chip */
    u32 n3ds_ram;        /* New 3DS: 1 = XNU gets the 128 MiB FCRAM extension too (256 MiB) */
    u32 auto_brightness; /* New 3DS: 0 = never sample ambient light / never change the backlight */
    u32 safe_mode;       /* 1 = boot-arg ios3ds_safe=1: no Substrate tweaks (also: hold L while loading) */
    u32 stereo3d;        /* 1 = experimental stereoscopic 3D home screen (docs/stereo3d.md) */
    u32 hwtest;          /* 1 = one-boot hardware test with prompts, 2 = automatic checks only (boot-arg ios3ds_hwtest=, docs/real-hardware.md) */
    u32 stereo_depth;    /* stereo3d=1: depth strength 0..100 % of the slider shift (default 100) */
    u32 nloads;
    char load_path[4][64];
    u32 load_pa[4];
    char cmdline[256];
} bootcfg_t;

extern bootcfg_t cfg;
void parse_bootcfg(char *txt);
