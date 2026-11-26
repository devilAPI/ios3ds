/*
 * boot.cfg parsing. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bootcfg.h"
#include "display.h"
#include "util.h"
#include "log.h"
#include "stereo.h"

bootcfg_t cfg;

void parse_bootcfg(char *txt)
{
    memset(&cfg, 0, sizeof(cfg));
    strlcpy_(cfg.kernel, "0:/ios3ds/kernel.bin", sizeof(cfg.kernel));
    strlcpy_(cfg.devtree, "0:/ios3ds/devtree.bin", sizeof(cfg.devtree));
    cfg.jump = 1;
    cfg.countdown_ms = 3000;
    cfg.phys_base = FCRAM_BASE;
    cfg.virt_base = 0x80000000u;
    cfg.mem_size = FCRAM_SIZE;
    cfg.ba_revision = 1;
    cfg.probes = 1;
    cfg.ba_version = 5;
    cfg.screen = SCREEN_BOTH;   /* pe_identify_machine panics 'Epoch Mismatch' unless == 5 */
    cfg.gpu = 1;
    cfg.gpu_mirror = 1;
    cfg.gpu_vbl_div = 2;
    cfg.gpu_clk = 1;
    cfg.gpu_diag = 1;
    cfg.ca_gpu = 1;
    cfg.mcu = 1;
    cfg.mcu_acc = 1;
    cfg.gyro = 1;
    cfg.powersave = 1;
    cfg.mic = 1;
    cfg.camera = 1;
    cfg.hp_route = 1;
    cfg.hp_codec = 1;
    cfg.n3ds_clock = 1;
    cfg.n3ds_hid = 1;
    cfg.n3ds_ram = 1;
    cfg.auto_brightness = 1;
    cfg.wifi_led = 1;
    cfg.stereo_depth = STEREO_DEPTH_DEFAULT;

    static const struct { const char *k; u32 *v; } nums[] = {
        { "kernel_pa", &cfg.kernel_pa }, { "entry_pa", &cfg.entry_pa },
        { "devtree_pa", &cfg.devtree_pa }, { "bootargs_pa", &cfg.bootargs_pa },
        { "shim_pa", &cfg.shim_pa }, { "ring_pa", &cfg.ring_pa }, { "ring_size", &cfg.ring_size }, { "top_pa", &cfg.top_pa },
        { "phys_base", &cfg.phys_base }, { "virt_base", &cfg.virt_base }, { "mem_size", &cfg.mem_size },
        { "video_pa", &cfg.video_pa }, { "video_display", &cfg.video_display },
        { "video_rowbytes", &cfg.video_rowbytes }, { "video_width", &cfg.video_width },
        { "video_height", &cfg.video_height }, { "video_depth", &cfg.video_depth },
        { "jump", &cfg.jump },
        /* boot_delay is the user-facing name (issue #1); countdown_ms stays for
           old cards. If both are present, the last line wins. */
        { "countdown_ms", &cfg.countdown_ms }, { "boot_delay", &cfg.countdown_ms },
        { "probes", &cfg.probes }, { "display_regs", &cfg.display_regs }, { "ba_revision", &cfg.ba_revision }, { "ba_version", &cfg.ba_version },
        { "screen", &cfg.screen },
        { "gpu", &cfg.gpu }, { "gpu_mirror", &cfg.gpu_mirror }, { "gpu_vbl_div", &cfg.gpu_vbl_div },
        { "gpu_spin_us", &cfg.gpu_spin_us }, { "gpu_clk", &cfg.gpu_clk }, { "gpu_diag", &cfg.gpu_diag },
        { "ca_gpu", &cfg.ca_gpu },
        { "mcu", &cfg.mcu }, { "mcu_acc", &cfg.mcu_acc }, { "gyro", &cfg.gyro }, { "powersave", &cfg.powersave }, { "notify_led", &cfg.notify_led }, { "wifi_led", &cfg.wifi_led },
        { "mic", &cfg.mic }, { "hp_route", &cfg.hp_route }, { "hp_codec", &cfg.hp_codec }, { "camera", &cfg.camera }, { "cam_test", &cfg.cam_test },
        { "n3ds_clock", &cfg.n3ds_clock }, { "n3ds_l2", &cfg.n3ds_l2 }, { "n3ds_hid", &cfg.n3ds_hid },
        { "n3ds_ram", &cfg.n3ds_ram }, { "auto_brightness", &cfg.auto_brightness },
        { "stereo3d", &cfg.stereo3d }, { "stereo_depth", &cfg.stereo_depth }, { "safe_mode", &cfg.safe_mode },
        { "diag", &cfg.diag }, { "ioprof", &cfg.ioprof }, { "log_ts", &cfg.log_ts },
        { "hwtest", &cfg.hwtest },
    };
    int lineno = 0;
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *eq = strchr(line, '=');
            if (!eq) {
                log_warn("boot.cfg:%d: no '=' - ignored\n", lineno);
            } else {
                *eq = 0;
                const char *key = line, *val = eq + 1;
                bool known = false;
                if (!strncmp(key, "cmdline", 8)) {
                    strlcpy_(cfg.cmdline, val, sizeof(cfg.cmdline));
                    known = true;
                } else if (!strncmp(key, "kernel", 7)) {
                    strlcpy_(cfg.kernel, val, sizeof(cfg.kernel));
                    known = true;
                } else if (!strncmp(key, "load", 5)) {
                    const char *comma = strchr(val, ',');
                    if (!comma || cfg.nloads >= 4) {
                        log_warn("boot.cfg:%d: load=path,pa (max 4)\n", lineno);
                    } else {
                        u32 n = comma - val < 63 ? comma - val : 63;
                        memcpy(cfg.load_path[cfg.nloads], val, n);
                        cfg.load_path[cfg.nloads][n] = 0;
                        cfg.load_pa[cfg.nloads] = parse_u32(comma + 1, NULL);
                        cfg.nloads++;
                    }
                    known = true;
                } else if (!strncmp(key, "gyro_axes", 10)) {
                    strlcpy_(cfg.gyro_axes, val, sizeof(cfg.gyro_axes));
                    known = true;
                } else if (!strncmp(key, "rootfs", 7)) {
                    strlcpy_(cfg.rootfs, val, sizeof(cfg.rootfs));
                    known = true;
                } else if (!strncmp(key, "shim", 5)) {
                    strlcpy_(cfg.shim, val, sizeof(cfg.shim));
                    known = true;
                } else if (!strncmp(key, "gps", 4)) {
                    strlcpy_(cfg.gps, val, sizeof(cfg.gps));
                    known = true;
                } else if (!strncmp(key, "rtc_offset", 11)) {
                    cfg.rtc_offset = *val == '-' ? -(int32_t)parse_u32(val + 1, NULL)
                                                 : (int32_t)parse_u32(val + (*val == '+'), NULL);
                    known = true;
                } else if (!strncmp(key, "devtree", 8)) {
                    strlcpy_(cfg.devtree, val, sizeof(cfg.devtree));
                    known = true;
                } else {
                    for (u32 i = 0; i < ARRAY_SIZE(nums); i++) {
                        if (!strncmp(key, nums[i].k, strlen(nums[i].k) + 1)) {
                            *nums[i].v = parse_u32(val, NULL);
                            known = true;
                        }
                    }
                }
                if (!known) log_warn("boot.cfg:%d: unknown key '%s'\n", lineno, key);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}
