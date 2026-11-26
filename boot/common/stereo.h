/*
 * ios3ds stereoscopic 3D home screen (docs/stereo3d.md): the depth classes,
 * the eye offsets and the per-frame layer classifier, shared by the ARM9
 * screen copy (boot/arm9/display.c), the GL engine's compositor stamp
 * (tools/ios3ds-gl/stereo.c), the kernel shim (kernel/shim/pica.c) and the
 * host tests (tests/test_stereo.py).
 *
 * The composed frame carries the depth class of the topmost layer at each
 * pixel in its alpha byte (the engine stamps it while CoreAnimation draws);
 * the ARM9 screen copy reads that byte and builds the two eye images by
 * sampling the frame with a horizontal offset that depends on the class:
 * wallpaper furthest back, the icon grid at the convergence plane, dock and
 * status bar in front of it, folders and alerts in front of those. Frames
 * whose alpha carries no class map (apps, the lock screen, a software
 * compositor) are copied to both eyes unchanged.
 *
 * Only fixed-width C types, no library calls: included by the ARM9, the
 * ARM11 shim, the iOS engine and the host tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* 3D slider (MCU reg 0x08, 0..255) below this = off: flat 2D, barrier off.
   The resting "off" position reads 0 or 8 on hardware, so the threshold
   sits above both. */
#define STEREO_MIN_SLIDER   16u
/* shift in iOS pixels at slider 255: a few pixels at most (docs/stereo3d.md) */
#define STEREO_MAX_SHIFT    4

/* Hardware registers shared by the ARM9 copy, the ARM11 shim and QEMU.
   3dbrew "LCD Registers": parallax barrier enable 0x10202000 (bit 0 = on),
   parallax barrier PWM 0x10202004 (libn3ds LCD_setParallaxBarrier writes
   0x0A390A39 first, then 0x00010001; 0 turns the barrier off).
   3dbrew "GPU/External Registers": PDC0 (top) at 0x10400400; +0x68/+0x6C are
   framebuffer A (left eye) 0/1, +0x94/+0x98 framebuffer B (right eye) 0/1;
   +0x70 bits 5-4 = 2 (AB interlace) selects 3D mode, bit 6 (line doubling)
   must then be clear. All are 32-bit accesses. */
#define ST_BARRIER_PA      0x10202000u
#define ST_BARRIER_PWM_PA  0x10202004u
#define ST_BARRIER_ON      0x00010001u
#define ST_BARRIER_PWM     0x0A390A39u
#define ST_PDC0_PA         0x10400400u
#define ST_PDC_FMT_OFF     0x70u
#define ST_PDC_FMT_INTER   (2u << 4)       /* AB: alternate A/B per scanline */
#define ST_PDC_FMT_MASK    0x70u           /* interlace + line-doubling bits */
/* a home screen frame needs at least this many icon-grid draws; the lock
   screen and Spotlight have fewer, apps none */
#define STEREO_MIN_ICONS    4

/* The alpha byte of the composed frame = the depth class of the layer on
   top at that pixel. Values chosen well away from the premultiplied alphas
   a software compositor would leave behind (0x00 transparent, 0xFF opaque),
   so a frame without our stamps never looks like a class map. */
#define SA_FLAT     0x00u   /* both eyes identical */
#define SA_BG       0x33u   /* wallpaper: furthest back */
#define SA_ICONS    0x66u   /* icon grid: the convergence plane */
#define SA_FRONT    0x99u   /* dock, status bar */
#define SA_ALERT    0xCCu   /* folders, alerts: in front */

/* depth classes as numbers: 0 flat, 1 background, 2 icons, 3 front, 4 alert */
enum { STC_FLAT = 0, STC_BG = 1, STC_ICONS = 2, STC_FRONT = 3, STC_ALERT = 4 };

/* flag = boot.cfg stereo3d, slider = mcubox_t.slider3d */
static inline int stereo_active(int flag, uint32_t slider)
{
    return flag && slider >= STEREO_MIN_SLIDER;
}

/* shift magnitude k in iOS pixels for one slider position (0 when off) */
static inline int stereo_shift_k(uint32_t slider)
{
    if (slider < STEREO_MIN_SLIDER)
        return 0;
    return 1 + (int)((slider - STEREO_MIN_SLIDER) * (STEREO_MAX_SHIFT - 1u) /
                     (255u - STEREO_MIN_SLIDER));
}

/* boot.cfg stereo_depth (0..100 %, default 100 = the plain slider curve):
   scales the slider's shift. 0 = always flat (barrier off); any other value
   with the slider up gives at least 1 px, so a low setting is still 3D. */
#define STEREO_DEPTH_DEFAULT 100u
static inline int stereo_depth_k(uint32_t slider, uint32_t depth)
{
    int k = stereo_shift_k(slider);
    if (depth >= 100u)
        return k;
    if (!k || !depth)
        return 0;
    return (int)(((uint32_t)k * depth + 99u) / 100u);
}

/* alpha byte -> depth class, -1 = no class map at this pixel */
static inline int stereo_class_of(uint8_t a)
{
    switch (a) {
    case SA_FLAT:  return STC_FLAT;
    case SA_BG:    return STC_BG;
    case SA_ICONS: return STC_ICONS;
    case SA_FRONT: return STC_FRONT;
    case SA_ALERT: return STC_ALERT;
    default:       return -1;
    }
}

static inline uint8_t stereo_magic(int cls)
{
    switch (cls) {
    case STC_BG:    return SA_BG;
    case STC_ICONS: return SA_ICONS;
    case STC_FRONT: return SA_FRONT;
    case STC_ALERT: return SA_ALERT;
    default:        return SA_FLAT;
    }
}

/* Horizontal offset of one eye, in iOS pixels: out(x) = in(x - delta).
   The eyes shift in opposite directions; class 2 (the icon grid) stays at
   the screen, class 1 (wallpaper) moves -k in the left eye and +k in the
   right one (so the wallpaper sits behind the screen, the dock in front of
   it), class 0 (flat) and unknown alphas never move. */
static inline int stereo_eye_delta(int cls, int k, int right)
{
    int d;
    if (cls <= STC_FLAT || cls > STC_ALERT || k <= 0)
        return 0;
    d = k * (cls - STC_ICONS);
    return right ? -d : d;
}

/* ---- the per-frame layer classifier (engine side, host-tested) ---------- */

enum { STP_START = 0, STP_HOME = 1, STP_FLAT = 2, STP_ALERT = 3 };

typedef struct {
    uint8_t phase;      /* STP_* */
    uint8_t icons;      /* icon-grid draws seen (saturating) */
    uint8_t bg;         /* a full-screen background was seen */
    uint8_t draws;      /* draws into the display target (saturating) */
    uint8_t stamps;     /* draws that actually wrote a class (saturating) */
} stereo_frame_t;

static inline void stereo_frame_begin(stereo_frame_t *f)
{
    f->phase = STP_START;
    f->icons = f->bg = f->draws = f->stamps = 0;
}

/* One draw into the display target. x0..y1 is the draw's bounding box in
   viewport pixels (GL row 0 = the surface's first memory row = the iOS
   image's top row), vp = {x, y, w, h}; full = the box covers the viewport,
   opaque = the draw covers its box with opaque pixels. Returns the depth
   class to stamp into the alpha channel, or -1 = leave the alpha alone.

   Rules (docs/stereo3d.md section 4):
     - the first full-screen draw (opaque or translucent: SpringBoard's
       wallpaper has translucent texels) is the background;
     - a later full-screen opaque draw covers everything: the frame is flat
       (apps, the lock screen), nothing is stamped and the frame is wiped;
     - a later full-screen translucent draw is a backdrop: it keeps the
       classes below it, and content drawn after it (a folder or alert)
       becomes STC_ALERT;
     - when CA draws the wallpaper as many small layers instead of one
       full-screen quad, the first few small draws establish the background
       too (but are not stamped themselves);
     - everything else is classified by where it sits: within the status-bar
       band or the dock band = STC_FRONT, the icon grid = STC_ICONS. */
static inline int stereo_draw(stereo_frame_t *f, int x0, int y0, int x1, int y1,
                             const int vp[4], int full, int opaque)
{
    int h = vp[3], cls = -1;
    (void)x0;
    (void)x1;
    if (f->draws < 255)
        f->draws++;
    if (f->phase == STP_FLAT)
        return -1;
    if (full) {
        if (!f->bg) {
            /* Requiring an opaque full-screen draw did not work in QEMU:
               SpringBoard's first frame had 50 draws, all in phase START
               and none stamped. Treat the first full-screen draw (opaque or
               not) as the background; apps still go flat at the second one. */
            f->bg = 1;
            f->phase = STP_HOME;
            cls = STC_BG;
        } else if (!opaque) {               /* backdrop or container */
            if (f->phase != STP_ALERT && f->icons)
                f->phase = STP_ALERT;
        } else {
            f->phase = STP_FLAT;            /* an opaque full screen covers all */
        }
    } else if (f->phase == STP_START) {
        /* No full-screen wallpaper quad: count small draws until enough of
           them look like an icon grid. Those early draws are not stamped,
           but all the later icons and dock items are. */
        if (f->icons < 255)
            f->icons++;
        if (f->icons >= STEREO_MIN_ICONS) {
            f->bg = 1;
            f->phase = STP_HOME;
        }
    } else {
        /* status bar: entirely in the top ~1/16; dock: entirely in the bottom quarter */
        if (y1 <= vp[1] + h / 16 || y0 >= vp[1] + h - h / 4)
            cls = f->phase == STP_ALERT ? STC_ALERT : STC_FRONT;
        else {
            if (f->icons < 255)
                f->icons++;
            cls = f->phase == STP_ALERT ? STC_ALERT : STC_ICONS;
        }
    }
    if (cls >= 0 && f->stamps < 255)
        f->stamps++;
    return cls;
}

/* The frame keeps its class map only if it really is the home screen. At
   least a few draws must actually have stamped a class: a phase reached by
   counting small draws alone, with no stamps, is not a class map. */
static inline int stereo_frame_home(const stereo_frame_t *f)
{
    return (f->phase == STP_HOME || f->phase == STP_ALERT) && f->bg &&
           f->icons >= STEREO_MIN_ICONS && f->stamps >= STEREO_MIN_ICONS;
}

/* The screen copy's frame decision: every sampled alpha byte must be a
   class magic (a frame without stamps is not one) and at least two
   different non-flat classes must be there (a flat frame is uniform). */
static inline int stereo_alphas_stereo(const uint8_t *alpha, int n)
{
    unsigned seen = 0;
    int i, cls, distinct = 0;
    for (i = 0; i < n; i++) {
        cls = stereo_class_of(alpha[i]);
        if (cls < 0)
            return 0;                       /* not stamped: no class map */
        if (cls > STC_FLAT)
            seen |= 1u << cls;
    }
    while (seen) {
        distinct += seen & 1;
        seen >>= 1;
    }
    return distinct >= 2;
}
