/*
 * ios3ds ARM9: the 3DS cameras' control side (docs/camera.md).
 *
 * The ARM11 kernel shim (kernel/shim/cam.c) asks for a camera through the
 * camera mailbox (cammbox_t at shim + SHIM_CAM_OFF, boot/common/cam_proto.h);
 * this file, polled from the monitor loop, does what needs the I2C buses:
 *   - CFG11_CAMERA_CNT bit 0 (the cameras' 16 MHz clock, needed for their I2C),
 *   - the CAM blocks' reset and set-up in the order the 3DS CAMERA sysmodule
 *     uses (re-log 71: CAM0 bit 4 low, clock on, bit 4 high, both blocks'
 *     CAM_CNT bits 8-10/14, transfer unit, trimming off),
 *   - the Aptina MT9V113 bring-up: reset, wakeup, PLL, YUV422 output at 320x240
 *     (GBATEK "DSi Aptina Camera Initialization", with output size A set to
 *     320x240), parallel output on,
 *   - standby of the camera that is not wanted (the inner camera and the outer
 *     right camera share the CAM0 parallel bus: only one may drive it).
 * The shim then owns the CAM block (CAM_CNT, overrun recovery) and drains its
 * FIFO, which only the ARM11 can read.
 *
 * Which camera sits where (GBATEK "3DS I2C", "3DS Video CAM Registers"):
 *   inner camera      I2C bus 0x10161000, device 0x7A, CAM0 (0x10120000)
 *   outer right       I2C bus 0x10161000, device 0x78, CAM0 (unused by iOS: kept in standby)
 *   outer left        I2C bus 0x10144000, device 0x78, CAM1 (0x10121000)
 * Nothing here has run on a console yet (docs/camera.md section 6).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "camera.h"
#include "i2c.h"
#include "log.h"
#include "util.h"
#include "cam_proto.h"
#include "fatfs/ff.h"

#define BUS_DSI         0           /* i2c.c bus numbers: 0 = 0x10161000, 1 = 0x10144000 */
#define BUS_3DS         1
/* BlocksDS libnds camera.h MCUREG_APT_AE_LAST_LUMA: the sensor's own measured
   scene luminance (8-bit, gamma encoded). New 3DS' QTM service turns camera
   data into lux; this raw byte is the ARM9-readable part of that path. */
#define ALS_LUMA_VAR    0x2218u
#define REG32(a)        (*(vu32 *)(a))
#define REG8(a)         (*(vu8 *)(a))

enum { CAMERR_NONE, CAMERR_NO_ID, CAMERR_INIT, CAMERR_WAKE, CAMERR_PLL, CAMERR_SEQ };

static volatile cammbox_t *box;
static bool enabled;
static u32 handled_seq;
static int active = -1;             /* camera whose parallel output is on */
static bool powered;
static bool als_want, als_ready;    /* ambient light sampling (New models, docs/mcu.md section 13) */
static u32 t_started, diag_step;    /* camera_poll's diagnostic dumps after a start */
static u32 t_stat;                  /* last 10 s statistics line */
static u32 t_watch;                 /* last independent CAM_STAT sample */
static u32 watch_n, watch_max, watch_full;   /* ARM9's own FIFO observer, one sample per monitor loop */

#define T9_VAL(n)   (*(vu16 *)(0x10003000u + (n) * 4))
static u32 t9(void)                 /* main.c's free-running 65457 Hz clock */
{
    u16 hi, lo, hi2;
    do {
        hi = T9_VAL(3);
        lo = T9_VAL(2);
        hi2 = T9_VAL(3);
    } while (hi != hi2);
    return (u32)hi << 16 | lo;
}
static void wait_ms(u32 ms)
{
    u32 t0 = t9();
    while (t9() - t0 < ms * 66u) { }
}

typedef struct { u8 bus, dev, port; const char *name; } camdev_t;
static const camdev_t cams[3] = {
    [CAM_OUTER] = { BUS_3DS, CAM_I2C_OUTER_L, 1, "outer left" },
    [CAM_INNER] = { BUS_DSI, CAM_I2C_INNER, 0, "inner" },
    [2]         = { BUS_DSI, CAM_I2C_OUTER_R, 0, "outer right" },
};

/* ---- Aptina register access (16-bit index, 16-bit big-endian data) ---- */
static bool apt_wr(const camdev_t *c, u16 idx, u16 v)
{
    u8 b[2] = { (u8)(v >> 8), (u8)v };
    return i2c_write16(c->bus, c->dev, idx, b, 2);
}
static bool apt_rd(const camdev_t *c, u16 idx, u16 *v)
{
    u8 b[2];
    if (!i2c_read16(c->bus, c->dev, idx, b, 2)) return false;
    *v = (u16)(b[0] << 8 | b[1]);
    return true;
}
static bool apt_mask(const camdev_t *c, u16 idx, u16 clr, u16 set)
{
    u16 v;
    return apt_rd(c, idx, &v) && apt_wr(c, idx, (u16)((v & ~clr) | set));
}
/* MCU variables through XDMA: MCU_ADDRESS 0x098C, MCU_DATA_0 0x0990 */
static bool mcu_wr(const camdev_t *c, u16 var, u16 v)
{
    return apt_wr(c, 0x098C, var) && apt_wr(c, 0x0990, v);
}
static bool mcu_rd(const camdev_t *c, u16 var, u16 *v)
{
    return apt_wr(c, 0x098C, var) && apt_rd(c, 0x0990, v);
}
/* wait until (reg & mask) == want, at most 500 ms */
static bool apt_wait(const camdev_t *c, u16 idx, u16 mask, u16 want, bool mcu)
{
    u32 t0 = t9();
    do {
        u16 v;
        if ((mcu ? mcu_rd(c, idx, &v) : apt_rd(c, idx, &v)) && (v & mask) == want) return true;
    } while (t9() - t0 < 32730u);       /* 500 ms (run 23: a third init timed out at 100 ms) */
    return false;
}

/* GBATEK aptina_code_list_init, output size A = 320x240 */
static u32 apt_init(const camdev_t *c, bool inner)
{
    static const u16 seq1[][2] = {          /* MCU variable writes */
        { 0x02F0, 0x0000 }, { 0x02F2, 0x0210 }, { 0x02F4, 0x001A }, { 0x2145, 0x02F4 }, { 0xA134, 0x0001 },
    };
    static const u16 flicker[][2] = {       /* the 3DS cam module's flicker-detection set-up (re-log 71) */
        { 0x222D, 0x0058 }, { 0xA408, 0x0015 }, { 0xA409, 0x0018 }, { 0xA40A, 0x0019 }, { 0xA40B, 0x001C },
        { 0x2411, 0x0058 }, { 0x2413, 0x006A }, { 0x2415, 0x0058 }, { 0x2417, 0x006A },
        { 0xA402, 0x001D }, { 0xA403, 0x0004 }, { 0xA404, 0x0010 }, { 0xA40D, 0x0002 }, { 0xA40E, 0x0003 },
        { 0xA410, 0x000A }, { 0xA40C, 0x0000 },
    };
    static const u16 modes[][2] = {
        { 0x2755, 0x0002 }, { 0x2757, 0x0002 },                         /* output format A/B: YUV */
    };
    static const u16 sizes[][2] = {
        { 0x2703, CAM_W }, { 0x2705, CAM_H },                           /* output size A */
        { 0x2707, 0x0280 }, { 0x2709, 0x01E0 },                         /* output size B 640x480 */
        { 0x2715, 0x0001 }, { 0x2719, 0x001A }, { 0x271B, 0x006B }, { 0x271D, 0x006B },
        { 0x271F, 0x02C0 }, { 0x2721, 0x034B },                         /* sensor A timing */
        { 0xA20B, 0x0000 }, { 0xA20C, 0x0006 },                         /* AE index range */
        { 0x272B, 0x0001 }, { 0x272F, 0x001A }, { 0x2731, 0x006B }, { 0x2733, 0x006B },
        { 0x2735, 0x02C0 }, { 0x2737, 0x034B },                         /* sensor B timing */
    };
    u16 v;
    apt_wr(c, 0x001A, 0x0003);              /* reset */
    wait_ms(1);                             /* the cam module waits 1 ms after each (re-log 71) */
    apt_wr(c, 0x001A, 0x0000);
    wait_ms(1);
    apt_wr(c, 0x0018, 0x4028);              /* wakeup */
    apt_wr(c, 0x001E, 0x0201);
    apt_wr(c, 0x0016, 0x42DF);
    if (!apt_wait(c, 0x0018, 0x4000, 0, false) || !apt_wait(c, 0x301A, 0x0004, 0x0004, false))
        return CAMERR_WAKE;
    for (u32 i = 0; i < sizeof seq1 / sizeof seq1[0]; i++) mcu_wr(c, seq1[i][0], seq1[i][1]);
    if (mcu_rd(c, 0xA115, &v)) mcu_wr(c, 0xA115, v | 2);               /* SEQ_CAP_MODE: video */
    wait_ms(10);
    for (u32 i = 0; i < sizeof modes / sizeof modes[0]; i++) mcu_wr(c, modes[i][0], modes[i][1]);
    mcu_wr(c, 0xA111, 0x000A);              /* written by the 3DS cam module's base script */
    apt_wr(c, 0x0014, 0x2145);              /* PLL as on the DSi (and the 3DS cam module) */
    apt_wr(c, 0x0010, 0x0111);
    apt_wr(c, 0x0012, 0x0000);
    apt_wr(c, 0x0014, 0x244B);
    wait_ms(1);
    apt_wr(c, 0x0014, 0x304B);
    if (!apt_wait(c, 0x0014, 0x8000, 0x8000, false)) return CAMERR_PLL;
    apt_mask(c, 0x0014, 0x0001, 0);         /* PLL bypass off */
    for (u32 i = 0; i < sizeof flicker / sizeof flicker[0]; i++) mcu_wr(c, flicker[i][0], flicker[i][1]);
    for (u32 i = 0; i < sizeof sizes / sizeof sizes[0]; i++) mcu_wr(c, sizes[i][0], sizes[i][1]);
    apt_mask(c, 0x3210, 0, 0x0008);
    mcu_wr(c, 0xA208, 0x0000);
    mcu_wr(c, 0xA24C, 0x0020);
    mcu_wr(c, 0xA24F, 0x0070);
    /* read mode as in GBATEK's DSi list (0x24 for device 0x7A, 0x25 = x-flip for the others);
       which one gives a true picture on the 3DS is unverified (docs/camera.md section 7) */
    mcu_wr(c, 0x2717, inner ? 0x0024 : 0x0025);
    mcu_wr(c, 0x272D, inner ? 0x0024 : 0x0025);
    mcu_wr(c, 0xA202, inner ? 0x0022 : 0x0000);
    mcu_wr(c, 0xA203, inner ? 0x00BB : 0x00FF);
    apt_mask(c, 0x0016, 0, 0x0020);
    mcu_wr(c, 0xA115, 0x0072);
    mcu_wr(c, 0xA11F, 0x0001);
    apt_wr(c, 0x326C, inner ? 0x0900 : 0x1000);
    mcu_wr(c, 0xAB22, inner ? 0x0001 : 0x0002);
    mcu_wr(c, 0xA103, 0x0006);              /* refresh mode */
    if (!apt_wait(c, 0xA103, 0x000F, 0, true)) return CAMERR_SEQ;
    mcu_wr(c, 0xA103, 0x0005);              /* refresh */
    if (!apt_wait(c, 0xA103, 0x000F, 0, true)) return CAMERR_SEQ;
    return CAMERR_NONE;
}

static bool apt_activate(const camdev_t *c)
{
    if (!apt_mask(c, 0x0018, 0x0001, 0)) return false;                  /* out of standby */
    apt_wait(c, 0x0018, 0x4000, 0, false);
    apt_wait(c, 0x301A, 0x0004, 0x0004, false);
    return apt_mask(c, 0x001A, 0, 0x0200);                              /* parallel output on */
}

static void apt_standby(const camdev_t *c)
{
    apt_mask(c, 0x001A, 0x0200, 0);                                     /* parallel output off */
    apt_mask(c, 0x0018, 0, 0x0001);                                     /* standby */
}

/* ---- hardware diagnostics (docs/camera.md section 7, re-log 68) ---- */
#define CAMREG32(p, o)  (*(vu32 *)(CAM_REG_PA(p) + (o)))
#define CAMREG16(p, o)  (*(vu16 *)(CAM_REG_PA(p) + (o)))

static void diag_cam_block(const char *when)
{
    log_ok("[cam-diag] %s: CFG11_CAMERA_CNT %02x; CAM0 cnt %08x stat %04x +6 %04x; CAM1 cnt %08x stat %04x +6 %04x\n",
           when, (unsigned)REG8(CFG11_CAMERA_CNT_PA),
           (unsigned)CAMREG32(0, CAM_CNT), (unsigned)CAMREG16(0, CAM_STAT), (unsigned)CAMREG16(0, CAM_CLR),
           (unsigned)CAMREG32(1, CAM_CNT), (unsigned)CAMREG16(1, CAM_STAT), (unsigned)CAMREG16(1, CAM_CLR));
}

/* CAM_CNT bits 0/1 are status bits that toggle during a transfer (GBATEK):
   count their changes and FIFO fill changes over ~50 ms per port */
static void diag_toggles(void)
{
    for (u32 p = 0; p < 2; p++) {
        u32 prev = CAMREG32(p, CAM_CNT) & 3, ch0 = 0, ch1 = 0, fillch = 0, maxfill = 0;
        u32 pf = CAMREG16(p, CAM_STAT) & 0x7FF, t0 = t9(), n = 0;
        while (t9() - t0 < 3273u) {      /* 50 ms */
            u32 v = CAMREG32(p, CAM_CNT) & 3, f = CAMREG16(p, CAM_STAT) & 0x7FF;
            ch0 += ((v ^ prev) & 1) != 0;
            ch1 += ((v ^ prev) & 2) != 0;
            fillch += f != pf;
            if (f > maxfill) maxfill = f;
            prev = v;
            pf = f;
            n++;
        }
        log_ok("[cam-diag] CAM%u over 50 ms (%u reads): CNT bit0 changed %u times, bit1 %u times; FIFO fill changed %u times, max %u\n",
               (unsigned)p, (unsigned)n, (unsigned)ch0, (unsigned)ch1, (unsigned)fillch, (unsigned)maxfill);
    }
}

static void diag_sensor(const camdev_t *c)
{
    static const u16 regs[] = { 0x0010, 0x0014, 0x0016, 0x0018, 0x001A, 0x001E, 0x301A };
    static const u16 vars[] = { 0xA103, 0xA104, 0xA115, 0x2703, 0x2705, 0x2755, 0x2717 };
    u16 v[7], m[7];
    for (u32 i = 0; i < 7; i++) {
        if (!apt_rd(c, regs[i], &v[i])) v[i] = 0xDEAD;
        if (!mcu_rd(c, vars[i], &m[i])) m[i] = 0xDEAD;
    }
    log_ok("[cam-diag] %s sensor: PLL_DIV %04x PLL %04x CLOCKS %04x STANDBY %04x RESET_MISC %04x PAD_SLEW %04x CORE_301A %04x\n",
           c->name, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    log_ok("[cam-diag] %s sensor: SEQ_CMD %04x SEQ_STATE %04x SEQ_CAP_MODE %04x OUT_W_A %04x OUT_H_A %04x OUT_FMT_A %04x READ_MODE_A %04x\n",
           c->name, m[0], m[1], m[2], m[3], m[4], m[5], m[6]);
}

static void diag_shim(const char *name, u32 nframes, u32 novr, u32 gap)
{
    log_ok("[cam] %s camera: %u frames, %u FIFO overruns in the last 10 s; shim: %u services, %u with data, "
           "stat %08x (max fill %u), drain gap max %u.%u ms, CAM_CNT %08x, variant %u, CAM%u%s, %u words, %u blank drops\n", name,
           (unsigned)nframes, (unsigned)novr, (unsigned)box->services, (unsigned)box->data_seen,
           (unsigned)box->stat_last, (unsigned)box->stat_max, (unsigned)(gap / 1047u), (unsigned)((gap % 1047u) * 10u / 1047u),
           (unsigned)box->cnt_rb, (unsigned)box->variant,
           (unsigned)box->port_used, box->probing ? ", still probing: no data yet" : "", (unsigned)box->words,
           (unsigned)box->resyncs);
}

/* The word the shim never says: run 29 showed a full FIFO (fill stuck at
   0x500) with the overrun bit still clear - when the drain is starved for
   longer than the FIFO holds (~5.5 ms of the stream, e.g. masked interrupts),
   words are lost without any counter moving. The ARM9 is never starved, so
   it polls CAM_STAT independently while a camera runs. */
static void watch_sample(void)
{
    if (t9() - t_watch < 66u) return;            /* 1 ms */
    t_watch = t9();
    for (u32 p = 0; p < 2; p++) {
        u32 f = CAMREG16(p, CAM_STAT) & 0x7FFu;
        if (f > watch_max) watch_max = f;
        if (f >= CAM_FIFO_UNITS - 40u) watch_full++;
    }
    watch_n++;
}

static void watch_report(const char *when)
{
    log_ok("[cam] monitor %s: %u CAM_STAT samples, max FIFO fill %u units, near-full %u\n",
           when, (unsigned)watch_n, (unsigned)watch_max, (unsigned)watch_full);
    watch_n = watch_max = watch_full = 0;
}

/* The shim's two newest complete frames to sd:/ios3ds/CAM<seq>.RAW, straight
   out of its buffers (docs/camera.md section 4.1). Called at stop: the shim
   has turned the capture off and cleaned its buffers before `want` went to 0,
   so nothing changes under the copy (a fast switch that already started again
   is caught by the seq check, the scene is dumped on the next stop). */
static u32 dumped_seq;
static void dump_frames(void)
{
    FIL f;
    UINT bw;
    u32 seq0 = box->newest_seq, seq1 = box->snew_seq;
    char path[32];
    u32 hdr[8];
    log_ok("[cam] dump check: newest seq %u, second %u (last saved %u), idx %u/%u, pa %08x/%08x\n",
           (unsigned)seq0, (unsigned)seq1, (unsigned)dumped_seq,
           (unsigned)box->newest, (unsigned)box->snew,
           (unsigned)box->buf_pa[box->newest % CAM_NBUF], (unsigned)box->buf_pa[box->snew % CAM_NBUF]);
    if (!seq1) return;                           /* fewer than two frames */
    if (seq0 == dumped_seq) return;               /* already saved */
    wait_ms(2);
    if (box->newest_seq != seq0) {               /* a new start is running */
        log_warn("[cam] dump skipped: newest seq moved %u -> %u\n", (unsigned)seq0, (unsigned)box->newest_seq);
        return;
    }
    if (box->newest >= CAM_NBUF || box->snew >= CAM_NBUF ||
        !box->buf_pa[box->newest] || !box->buf_pa[box->snew]) {
        log_warn("[cam] dump skipped: idx %u/%u, pa %08x/%08x\n",
                 (unsigned)box->newest, (unsigned)box->snew,
                 (unsigned)box->buf_pa[box->newest % CAM_NBUF], (unsigned)box->buf_pa[box->snew % CAM_NBUF]);
        return;
    }
    snprintf(path, sizeof(path), "0:/ios3ds/CAM%08u.RAW", (unsigned)seq0);
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        log_warn("[cam] frame dump: cannot create %s\n", path);
        return;
    }
    hdr[0] = 0x44554D43u;                        /* 'CAMD' */
    hdr[1] = 1;
    hdr[2] = CAM_W;
    hdr[3] = CAM_H;
    hdr[4] = box->cam;
    hdr[5] = box->port;
    hdr[6] = seq0;
    hdr[7] = seq1;
    FRESULT r = f_write(&f, hdr, sizeof(hdr), &bw);
    if (r == FR_OK) r = f_write(&f, (const void *)box->buf_pa[box->newest], CAM_FRAME_BYTES, &bw);
    if (r == FR_OK) r = f_write(&f, (const void *)box->buf_pa[box->snew], CAM_FRAME_BYTES, &bw);
    f_close(&f);
    if (r == FR_OK) {
        dumped_seq = seq0;
        log_ok("[cam] dumped frames seq %u (newest) and %u to %s\n",
               (unsigned)seq0, (unsigned)seq1, path);
    } else {
        log_warn("[cam] frame dump write failed (FRESULT %d)\n", (int)r);
    }
}



/* Boot self-test (boot.cfg cam_test=1; before XNU, the shim is not running yet):
   for each sensor, bring it up and try every CAM block set-up variant on
   both blocks, 200 ms each, reading CAM_STAT's fill/overrun (the ARM9 can
   read the registers, only the FIFO window is ARM11-only). Then two sensor
   variants with the most likely block set-ups: RESET_MISC 0x0200 (bit 4
   clear: outputs not gated by the OE pin) and PAD_SLEW 0x0777 (the Linux
   MT9V113 driver's value; the DSi list writes 0x0201). One log line each:
   [cam-test] <sensor> <set-up>: CAM0 stat .. cnt .., CAM1 stat .. cnt .. DATA */
static u32 selftest_one(const char *sname, const char *what, u32 cnt, u32 clr)
{
    u32 got = 0, st[2], rb[2], t0, maxf[2] = { 0, 0 };
    for (u32 p = 0; p < 2; p++) {
        CAMREG32(p, CAM_CNT) = cnt & ~CAM_CNT_EN;
        CAMREG16(p, CAM_CLR) = (u16)(0x8000u | clr);
        CAMREG32(p, CAM_CNT) = cnt;
    }
    t0 = t9();
    while (t9() - t0 < 13091u) {            /* 200 ms: 3 frames at 15 fps */
        for (u32 p = 0; p < 2; p++) {
            u32 v = CAMREG16(p, CAM_STAT);
            if ((v & 0x7FF) > maxf[p]) maxf[p] = v & 0x7FF;
            if (v & 0x8000) maxf[p] |= 0x8000;
        }
    }
    for (u32 p = 0; p < 2; p++) {
        st[p] = CAMREG16(p, CAM_STAT);
        rb[p] = CAMREG32(p, CAM_CNT);
        if (maxf[p]) got |= 1u << p;
        CAMREG32(p, CAM_CNT) = cnt & ~CAM_CNT_EN;      /* off again */
    }
    log_ok("[cam-test] %s, %s: CAM0 cnt %04x stat %04x max %04x | CAM1 cnt %04x stat %04x max %04x%s\n",
           sname, what, (unsigned)rb[0], (unsigned)st[0], (unsigned)maxf[0], (unsigned)rb[1], (unsigned)st[1],
           (unsigned)maxf[1], got ? "  <-- DATA" : "");
    return got;
}

static void selftest(void)
{
    static const u32 vcnt[CAM_NVARIANTS] = CAM_VAR_CNT_LIST;
    static const u32 vclr[CAM_NVARIANTS] = CAM_VAR_CLR_LIST;
    static const u8 order[3] = { CAM_OUTER, CAM_INNER, 2 };
    char what[40];
    u32 hits = 0;
    log_ok("[cam-test] boot self-test (cam_test=1): each sensor x CAM set-up, both blocks, 200 ms each\n");
    for (u32 k = 0; k < 3; k++) {
        const camdev_t *c = &cams[order[k]];
        u32 err;
        u16 id = 0;
        if (!apt_rd(c, 0x0000, &id) || id != APTINA_CHIP_ID) {
            log_warn("[cam-test] %s: no sensor\n", c->name);
            continue;
        }
        for (u32 j = 0; j < 3; j++)                     /* only this one may drive its bus */
            if (j != order[k]) apt_standby(&cams[j]);
        err = apt_init(c, order[k] == CAM_INNER);
        if (!err && !apt_activate(c)) err = CAMERR_INIT;
        if (err) {
            log_warn("[cam-test] %s: init failed (step %u)\n", c->name, (unsigned)err);
            diag_sensor(c);
            continue;
        }
        wait_ms(100);
        for (u32 v = 0; v < CAM_NVARIANTS; v++) {
            snprintf(what, sizeof what, "cnt %04x +6 %02x", (unsigned)vcnt[v], (unsigned)vclr[v]);
            hits += selftest_one(c->name, what, vcnt[v], vclr[v]);
        }
        apt_wr(c, 0x001A, 0x0200);                       /* RESET_MISC: parallel on, OE pin gating off */
        hits += selftest_one(c->name, "RESET_MISC 0200, cnt 8010", 0x8010, 0);
        hits += selftest_one(c->name, "RESET_MISC 0200, cnt 8610", 0x8610, 0);
        apt_wr(c, 0x001E, 0x0777);                       /* PAD_SLEW as the Linux MT9V113 driver */
        hits += selftest_one(c->name, "+ PAD_SLEW 0777, cnt 8610", 0x8610, 0);
        diag_sensor(c);
        apt_standby(c);
    }
    for (u32 p = 0; p < 2; p++) {
        CAMREG32(p, CAM_CNT) = p ? 0 : CAM_CNT_B4;      /* CAM0 bit 4 = reset released */
        CAMREG16(p, CAM_CLR) = 0x8000;
    }
    log_ok("[cam-test] done: %u set-ups saw data%s\n", (unsigned)hits,
           hits ? "" : " (none: the CAM blocks never received anything)");
}

static void publish(u32 state, u32 err)
{
    box->err = err;
    box->state = state;
    box->ack_seq = box->req_seq;
}

/* The CAMERA sysmodule's block reset (re-log 71): CAM0's CAM_CNT bit 4 is
   held low while the camera clock (pdn:c, here CFG11_CAMERA_CNT bit 0) is
   switched on, released with the clock running, then the clock goes off
   again and both CAM_CNTs are written back. Its delays are busy loops of
   well under a millisecond; these are longer. */
static void cam_reset(void)
{
    CAMREG16(0, CAM_CNT) &= (u16)~CAM_CNT_B4;
    wait_ms(1);
    REG8(CFG11_CAMERA_CNT_PA) |= 1u;
    wait_ms(1);
    CAMREG16(0, CAM_CNT) |= CAM_CNT_B4;
    wait_ms(2);
    REG8(CFG11_CAMERA_CNT_PA) &= (u8)~1u;
    for (u32 p = 0; p < 2; p++) CAMREG16(p, CAM_CNT) = CAMREG16(p, CAM_CNT);
}

/* The rest of the cam module's driver initialisation, for both blocks:
   transfer off, overrun cleared, 256-byte transfer unit, trimming off,
   bits 8-9 = 2, bit 10 on, bit 11 off; then bit 14 on both. The shim
   writes its set-up variant on top (variant 0 = this one, re-log 71). */
static void cam_block_init(void)
{
    cam_reset();
    for (u32 p = 0; p < 2; p++) {
        CAMREG16(p, CAM_CNT) &= (u16)~CAM_CNT_EN;
        CAMREG16(p, CAM_CLR) = (u16)((CAMREG16(p, CAM_CLR) & ~0x3Fu) | 0x8000u | CAM_XFER_UNITS(256u));
        CAMREG16(p, CAM_CNT) &= (u16)~CAM_CNT_TRIM;
        CAMREG32(p, CAM_SOFS) = 0x00010000u;
        CAMREG32(p, CAM_EOFS) = 0x00010000u;
        CAMREG16(p, CAM_CNT) = (u16)((CAMREG16(p, CAM_CNT) & ~(CAM_CNT_MODE_MASK | CAM_CNT_B11)) |
                                     CAM_CNT_MODE_2 | CAM_CNT_B10);
    }
    for (u32 p = 0; p < 2; p++) CAMREG16(p, CAM_CNT) |= CAM_CNT_DRQ;
}

static void power(bool on)
{
    if (on == powered) return;
    powered = on;
    if (on) {
        cam_block_init();
        u32 before = REG8(CFG11_CAMERA_CNT_PA);
        /* bit 0: the cameras' 16 MHz clock (their I2C needs it; the cam
           module's pdn:c call); bit 1 does not stick (run 25), set anyway */
        REG8(CFG11_CAMERA_CNT_PA) = (u8)(before | 3);
        wait_ms(5);
        log_ok("[cam-diag] CFG11_CAMERA_CNT %02x -> %02x\n", (unsigned)before, (unsigned)REG8(CFG11_CAMERA_CNT_PA));
        diag_cam_block("after the cam module's block reset/init");
        i2c_init(BUS_DSI);
        i2c_init(BUS_3DS);
        u32 present = 0;
        for (u32 i = 0; i < 3; i++) {
            u16 id = 0;
            bool ok = apt_rd(&cams[i], 0x0000, &id);
            if (ok && id == APTINA_CHIP_ID && i < CAM_COUNT) present |= 1u << i;
            log_ok("[cam] %s camera (I2C %s 0x%02X): %s%04x\n", cams[i].name,
                   cams[i].bus == BUS_DSI ? "0x10161000" : "0x10144000", cams[i].dev,
                   ok ? "chip id " : "no answer, ", ok ? id : 0);
            if (ok && id == APTINA_CHIP_ID && i == 2) apt_standby(&cams[2]);   /* keep CAM0's bus free */
        }
        box->present = present;
    } else {
        /* the cam module's shutdown: block reset, clock off, bits 8-10 cleared */
        for (u32 p = 0; p < 2; p++) CAMREG16(p, CAM_CNT) &= (u16)~CAM_CNT_EN;
        cam_reset();
        REG8(CFG11_CAMERA_CNT_PA) &= (u8)~3u;
        for (u32 p = 0; p < 2; p++)
            CAMREG16(p, CAM_CNT) &= (u16)~(CAM_CNT_MODE_MASK | CAM_CNT_B10 | CAM_CNT_DRQ);
    }
}

static void start(u32 cam)
{
    const camdev_t *c = &cams[cam];
    u32 err, t0 = t9();
    publish(CAM_ST_STARTING, 0);
    power(true);
    if (active >= 0 && active != (int)cam) {
        apt_standby(&cams[active]);
        active = -1;
    }
    if (!(box->present & (1u << cam))) {
        log_warn("[cam] %s camera did not answer on I2C: iOS gets no picture\n", c->name);
        publish(CAM_ST_FAILED, CAMERR_NO_ID);
        return;
    }
    diag_cam_block("before the sensor set-up");
    err = apt_init(c, cam == CAM_INNER);
    if (!err && !apt_activate(c)) err = CAMERR_INIT;
    if (err) {
        log_warn("[cam] %s camera: init failed (step %u)\n", c->name, (unsigned)err);
        diag_sensor(c);
        apt_standby(c);
        publish(CAM_ST_FAILED, err);
        return;
    }
    active = (int)cam;
    box->cam = cam;
    box->port = c->port;
    publish(CAM_ST_RUNNING, 0);
    log_ok("[cam] %s camera streaming %ux%u YUV422 into CAM%u (init %u ms)\n", c->name, CAM_W, CAM_H,
           (unsigned)c->port, (unsigned)((t9() - t0) / 65u));
    diag_sensor(c);
    t_started = t_stat = t9();
    diag_step = 0;
}

static void stop(void)
{
    int was = active;
    if (active >= 0) apt_standby(&cams[active]);
    active = -1;
    dump_frames();                       /* the buffers are clean and quiescent now */
    watch_report("at stop");
    /* Camera.app put the sensor we were sharing into standby: its AE is no
       longer running, so the next ambient sample must bring it up again. */
    if (was == CAM_INNER && als_ready) als_ready = false;
    /* Auto-brightness needs the inner sensor's AE running between Camera.app
       sessions. Keep the camera clock on in that case (docs/mcu.md §12). */
    if (als_ready && !als_want) {
        apt_standby(&cams[CAM_INNER]);   /* also when the client used an outer camera */
        als_ready = false;
        power(false);
        logp("[cam] inner camera standby (auto-brightness off)\n");
    } else if (!als_ready) power(false);
    publish(CAM_ST_OFF, 0);
    log_ok("[cam] cameras off (frames %u, FIFO overruns %u, CAM restarts %u)\n",
           (unsigned)box->frames, (unsigned)box->overruns, (unsigned)box->restarts);
}

/* ---- ambient light (auto-brightness on New 3DS / New 2DS XL) -------------
   The face-tracking ("inner") camera is the only ambient light source a New
   3DS has (Wikipedia's hardware section and 3dbrew QTM Services' GetCameraLux;
   no MCU register exposes a lux value). QTM keeps its AE running under
   Horizon; without Horizon we bring the sensor up once and leave it awake.
   If no sensor answers, samples fail and mcud keeps the user's brightness. */
void camera_als_enable(bool on)
{
    als_want = on;
    if (!on && als_ready && active < 0) {
        apt_standby(&cams[CAM_INNER]);
        als_ready = false;
        power(false);
        logp("[cam] inner camera standby (auto-brightness off)\n");
    }
}

/* True while any camera client streams. The ARM9 then leaves the cameras
   completely alone: QTM itself refuses to track while a camera is in use
   (3dbrew QTM Services), and we must not disturb Camera.app's AE either. */
bool camera_als_busy(void)
{
    return enabled && active >= 0;
}

bool camera_als_sample(uint8_t *luma)
{
    if (!enabled || !als_want || active >= 0) return false;
    const camdev_t *c = &cams[CAM_INNER];
    u16 v;
    if (!powered) power(true);
    if (!(box->present & (1u << CAM_INNER))) return false;
    if (!als_ready) {
        u32 err = apt_init(c, true);       /* PLL, AE window, sequencer refresh */
        if (err) {
            log_warn("[cam] ambient light: inner camera init failed (step %u)\n", (unsigned)err);
            return false;
        }
        als_ready = true;
        log_ok("[cam] inner camera awake for ambient light (AE luma var %04x)\n", ALS_LUMA_VAR);
    }
    if (!mcu_rd(c, ALS_LUMA_VAR, &v)) return false;
    *luma = (uint8_t)(v & 0xFF);
    return true;
}

void camera_init(u32 shim_pa, bool enable, bool self_test)
{
    if (!shim_pa) return;
    box = (volatile cammbox_t *)(shim_pa + SHIM_CAM_OFF);
    for (u32 i = 0; i < sizeof(cammbox_t) / 4; i++) ((vu32 *)box)[i] = 0;
    enabled = enable;
    box->enabled = enable;
    box->state = enable ? CAM_ST_OFF : CAM_ST_DISABLED;
    handled_seq = 0;
    if (!enable) {
        log_warn("[cam] disabled in boot.cfg (camera=0): iOS sees no camera\n");
        return;
    }
    /* probe once at boot, so iOS knows the cameras before the first start
       (run 23 reported "cameras present 0x0" until then), then standby + clock off */
    diag_cam_block("boot");
    power(true);
    if (self_test) {
        selftest();
        /* Leave nothing of the self-test behind (issue 1: cam_test=1 boots had
           no picture in Camera.app): soft-reset every sensor it touched
           (RESET_MISC / PAD_SLEW variants, its activate/standby sequence),
           then power-cycle the CAM blocks and probe again, so iOS starts from
           exactly the state of a cam_test=0 boot. */
        for (u32 i = 0; i < 3; i++) {
            u16 id = 0;
            if (!apt_rd(&cams[i], 0x0000, &id) || id != APTINA_CHIP_ID) continue;
            apt_wr(&cams[i], 0x001A, 0x0003);
            wait_ms(1);
            apt_wr(&cams[i], 0x001A, 0x0000);
            wait_ms(10);
            apt_standby(&cams[i]);
        }
        power(false);
        wait_ms(5);
        power(true);
        log_ok("[cam-test] sensors reset, CAM blocks re-initialised, cameras present 0x%x\n",
               (unsigned)box->present);
    }
    for (u32 i = 0; i < CAM_COUNT; i++)
        if (box->present & (1u << i)) apt_standby(&cams[i]);
    power(false);
}

void camera_poll(void)
{
    static u32 frames0, ovr0;
    u32 seq, want;
    if (!box || !enabled || box->magic != CAM_MBOX_MAGIC) return;
    if (active >= 0) watch_sample();
    if (active >= 0 && t9() - t_stat > 10u * 65457u) {      /* every 10 s while a camera runs */
        u32 f = box->frames, o = box->overruns;
        t_stat = t9();
        diag_shim(cams[active].name, f - frames0, o - ovr0, box->gap_max);
        frames0 = f;
        ovr0 = o;
        watch_report("10 s");
    }
    /* dumps 0.2 s, 1 s and 5 s after a start: the CAM blocks (with the shim's
       CAM_CNT applied), the status-bit toggles, the sensor */
    if (active >= 0 && diag_step < 3) {
        static const u32 at[3] = { 13091u, 65457u, 327285u };
        if (t9() - t_started > at[diag_step]) {
            static const char *const tag[3] = { "0.2 s after start", "1 s after start", "5 s after start" };
            diag_cam_block(tag[diag_step]);
            diag_toggles();
            if (diag_step == 2) diag_sensor(&cams[active]);
            diag_shim(cams[active].name, box->frames, box->overruns, box->gap_max);
            diag_step++;
        }
    }
    seq = box->req_seq;
    if (seq == handled_seq) return;
    handled_seq = seq;
    want = box->want;
    if (want && want - 1 < CAM_COUNT) {
        log_ok("[cam] iOS wants the %s camera\n", cams[want - 1].name);
        start(want - 1);
    } else {
        stop();
    }
}
