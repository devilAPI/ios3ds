/*
 * ios3ds camera: the contracts between
 *   - the ARM9 monitor (boot/arm9/camera.c): sensor power, I2C (Aptina MT9V113), CAM_CNT,
 *   - the ARM11 kernel shim (kernel/shim/campica.c): FIFO drain into frame buffers, sysctl,
 *   - userland (tools/ios3ds-camera/vcd: the vcd.mediacapture plug-in in mediaserverd),
 *   - the QEMU board (emu/qemu/hw/arm/ctr_cam.c) and the host tests.
 * See docs/camera.md. C only, fixed-width types, no libc.
 *
 * Userland -> shim: the top-level sysctl node "ios3ds_cam" (like ios3ds_gpu,
 * docs/pica200.md): one extra MIB component = the address of a cam_req_t.
 *
 *     int mib[4]; size_t n = 4;
 *     sysctlnametomib("ios3ds_cam", mib, &n);
 *     mib[n] = (int)&req;
 *     sysctl(mib, n + 1, NULL, NULL, NULL, 0);     // req.status = result
 *
 * Shim <-> ARM9: cammbox_t at shim + SHIM_CAM_OFF (one cache line per writer).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

#define CAM_SYSCTL_NAME     "ios3ds_cam"
#define CAM_PROTO_VERSION   1

/* the 3DS has three cameras; iOS gets two of them */
#define CAM_OUTER           0u      /* outer left camera: iOS "back" camera (PortTypeBack) */
#define CAM_INNER           1u      /* inner (self-facing) camera: iOS "front" camera */
#define CAM_COUNT           2u

/* frame format: what the CAM block delivers (YUV422, Y0 U Y1 V per 32-bit word) */
#define CAM_W               320u
#define CAM_H               240u
#define CAM_FRAME_BYTES     (CAM_W * CAM_H * 2u)
#define CAM_NBUF            3u      /* frame buffers in the shim (one being filled) */

enum {
    CAM_OP_INFO = 0,    /* -> info (does not touch the hardware) */
    CAM_OP_START,       /* a0 camera: power + sensor init (ARM9) + capture; returns at once */
    CAM_OP_STOP,        /* stop capture, sensor standby */
    CAM_OP_FRAME,       /* a0 user buffer (CAM_FRAME_BYTES), a1 last seq seen
                           -> newest complete frame if its seq differs: a1 = seq, a2 = capture time
                           (shim ticks, 1.047 MHz), status CAM_OK; else CAM_ERR_AGAIN */
};

enum {
    CAM_OK = 0,
    CAM_ERR_BAD_OP = 1,
    CAM_ERR_DISABLED = 2,   /* boot.cfg camera=0, or no ARM9 support */
    CAM_ERR_NO_MEM = 3,     /* no frame buffer memory */
    CAM_ERR_AGAIN = 4,      /* FRAME: nothing new yet */
    CAM_ERR_FAULT = 5,      /* bad user pointer */
    CAM_ERR_NOT_RUNNING = 6,/* FRAME without START */
    CAM_ERR_FAILED = 7,     /* the ARM9 could not bring the sensor up (state CAM_ST_FAILED, see log) */
    CAM_ERR_NO_SERVICE = 8, /* userland only: the sysctl does not exist */
};

typedef struct {
    uint32_t op;
    uint32_t status;
    uint32_t a[6];
} cam_req_t;

typedef struct {               /* CAM_OP_INFO result, copied into cam_req_t.a[] */
    uint32_t version;          /* CAM_PROTO_VERSION */
    uint32_t enabled;          /* boot.cfg camera != 0 and the ARM9 supports it */
    uint32_t state;            /* CAM_ST_* */
    uint32_t frames;           /* complete frames captured since boot */
    uint32_t overruns;         /* FIFO overruns (each costs one frame) */
    uint32_t cams;             /* bit n: camera n answered on I2C at its last power-up */
} cam_info_t;

/* ---- shim <-> ARM9 mailbox ------------------------------------------------
   Shim (ARM11, cached, cleans after writing) owns lines 0, 2 and 3 (offsets 0,
   64 and 96), the ARM9 (uncached) line 1 (offset 32). */
#define SHIM_CAM_OFF        0x7E00u         /* cammbox_t in the shim page (128 bytes) */
#define CAM_MBOX_MAGIC      0x304D4143u     /* 'CAM0' */
#define CAM_ST_OFF          0u      /* sensors in standby / unpowered */
#define CAM_ST_STARTING     1u      /* ARM9 is initialising the requested camera */
#define CAM_ST_RUNNING      2u      /* sensor streams, CAM block enabled: the shim drains */
#define CAM_ST_FAILED       3u      /* the ARM9 gave up (no I2C answer, timeout): see [cam] in the log */
#define CAM_ST_DISABLED     4u      /* boot.cfg camera=0 */

typedef struct {
    /* shim -> ARM9 */
    uint32_t magic;            /* CAM_MBOX_MAGIC once the shim knows the mailbox */
    uint32_t want;             /* 0 = off, 1 + camera = run that camera */
    uint32_t req_seq;          /* bumped on every change of `want` */
    uint32_t restarts;         /* shim restarted the CAM block after an overrun */
    uint32_t frames;
    uint32_t overruns;
    uint32_t words;            /* FIFO words drained (wraps) */
    uint32_t resyncs;          /* frames dropped at a vertical blank because their
                                  word count was off (lost or extra FIFO words) */
    /* ARM9 -> shim */
    uint32_t state;            /* CAM_ST_* */
    uint32_t ack_seq;          /* req_seq the state belongs to */
    uint32_t cam;              /* camera that is running */
    uint32_t port;             /* CAM block: 0 = 0x10120000, 1 = 0x10121000 */
    uint32_t present;          /* bit n: camera n answered on I2C */
    uint32_t enabled;          /* boot.cfg camera (1 = yes) */
    uint32_t err;              /* last error code (camera.c CAMERR_*) */
    uint32_t pad1;
    /* shim -> ARM9, line 3: hardware diagnostics (docs/camera.md section 7) */
    uint32_t services;         /* drain services while streaming */
    uint32_t data_seen;        /* services that found a non-empty FIFO */
    uint32_t stat_last;        /* last CAM_STAT: drained port | other port << 16 */
    uint32_t stat_max;         /* largest FIFO fill seen (8-byte units) */
    uint32_t cnt_rb;           /* CAM_CNT read back: drained port | other port << 16 */
    uint32_t variant;          /* CAM block set-up variant in use (cam.c cam_variants) */
    uint32_t port_used;        /* port the shim drains (may move to where data appears) */
    uint32_t probing;          /* 1 while no data has arrived since the start */
    /* shim -> ARM9, line 4 for the frame dump (docs/camera.md section 4.1):
       at STOP the shim has stopped writing and cleaned the buffers, the ARM9
       copies the two newest complete frames to sd:/ios3ds/photos/CAM<seq>.RAW */
    uint32_t buf_pa[CAM_NBUF];/* frame buffers' physical addresses (0 = none) */
    uint32_t newest;          /* buffer index of the newest complete frame (CAM_NBUF = none) */
    uint32_t newest_seq;      /* its sequence number (0 = none yet this session) */
    uint32_t snew;            /* buffer index of the second-newest complete frame */
    uint32_t snew_seq;        /* its sequence number */
    uint32_t gap_max;         /* worst interval between two drain services since
                                 the camera started, in shim ticks (1047 / ms; the
                                 FIFO holds ~5.5 ms of the stream: more than that
                                 and words are lost even without the overrun bit) */
} cammbox_t;
_Static_assert(sizeof(cammbox_t) == 128, "cammbox layout");
#define CAM_NVARIANTS       6u      /* kernel/shim/cam.c cam_var_*, boot/arm9/camera.c self-test */
/* 0: the CAMERA sysmodule's own set-up (re-log 71): CAM_CNT bits 8-9 = 2,
   10 = 1, 14 = 1 (bit 4 kept on CAM0), transfer unit 4 lines = 2560 bytes */
#define CAM_VAR_CNT_LIST    { 0xC610u, 0x8010u, 0x8610u, 0x8E10u, 0x8610u, 0xC610u }
#define CAM_VAR_CLR_LIST    { CAM_XFER_UNITS(CAM_W * 2u * 4u), 0x00u, 0x00u, 0x00u, 0x22u, 0x22u }

/* ---- hardware (GBATEK "3DS Video CAM Registers", "3DS I2C" ; 3dbrew "IO Registers") ----
   CAM0 0x10120000: inner camera + outer right camera (the DSi pair, shared bus)
   CAM1 0x10121000: outer left camera. Both CPUs reach the registers; the data
   FIFO window (0x10320000 / 0x10321000, 0x80 bytes, must be read at increasing
   addresses) is ARM11 only. */
#define CAM_REG_PA(port)    (0x10120000u + (port) * 0x1000u)
#define CAM_FIFO_PA(port)   (0x10320000u + (port) * 0x1000u)
#define CAM_FIFO_WINDOW     0x80u
#define CAM_CNT             0x00u   /* u32 R/W mask 0xef1c */
#define CAM_STAT            0x04u   /* u16 R: bits 0-10 FIFO fill in 8-byte units, bit 15 overrun */
#define CAM_CLR             0x06u   /* u16: bit 15 = clear overrun (while CAM_CNT bit 15 = 0) */
#define CAM_SOFS            0x10u
#define CAM_EOFS            0x14u
#define CAM_CNT_B4          (1u << 4)   /* CAM0 only: reset of both blocks (0 = held), see re-log 71 */
#define CAM_CNT_IRQ         (1u << 9)
#define CAM_CNT_MODE_MASK   0x0300u     /* bits 8-9: the cam module writes 2 */
#define CAM_CNT_MODE_2      0x0200u
#define CAM_CNT_B10         (1u << 10)  /* the cam module sets it at init, clears it at shutdown */
#define CAM_CNT_B11         (1u << 11)  /* the cam module clears it at init */
#define CAM_CNT_TRIM        (1u << 13)  /* trimming (SOFS/EOFS) on */
#define CAM_CNT_DRQ         (1u << 14)  /* the cam module sets it on both blocks at init */
#define CAM_CNT_EN          (1u << 15)
/* +6 bits 0-5: transfer unit in 256-byte steps minus 1 (at most 10240 bytes;
   the cam module uses lines x width x 2); bit 15 (write) clears the overrun */
#define CAM_XFER_UNITS(bytes) ((bytes) / 256u - 1u)
#define CAM_STAT_FILL(v)    ((v) & 0x7FFu)
#define CAM_STAT_OVR        (1u << 15)
#define CAM_FIFO_UNITS      0x500u  /* GBATEK: fill counts up to 0x500 (8-byte units) */

#define CFG11_CAMERA_CNT_PA 0x10141224u /* bit 0: 16 MHz camera clock (needed for camera I2C) */

/* I2C: GBATEK bus 0 = 3dbrew bus 1 = 0x10161000 (DSi devices), bus 1 = 0x10144000 */
#define CAM_I2C_BUS_DSI     0x10161000u
#define CAM_I2C_BUS_3DS     0x10144000u
#define CAM_I2C_INNER       0x7Au   /* 8-bit write address on CAM_I2C_BUS_DSI, CAM0 */
#define CAM_I2C_OUTER_R     0x78u   /* on CAM_I2C_BUS_DSI, CAM0 (not used by iOS) */
#define CAM_I2C_OUTER_L     0x78u   /* on CAM_I2C_BUS_3DS, CAM1 */
#define APTINA_CHIP_ID      0x2280u /* MT9V113 */
