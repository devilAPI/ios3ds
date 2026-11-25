/*
 * ios3ds-input: turn 3DS buttons + touch panel into iOS 4.2.1 HID events.
 *
 *   ARM9 monitor --(inmbox_t in the shim page)--> sysctl kern.ios3ds_input
 *     --> this daemon --> IOHIDEventSystemClientDispatchEvent()
 *     --> SpringBoard's own IOHIDEventSystem callback (SBHIDInterface)
 *
 * SpringBoard 4.2.1 hosts the Mach service com.apple.iohideventsystem and
 * handles events dispatched by clients exactly like events from a kernel HID
 * service (IOKit __io_hideventsystem_dispatch_event -> _IOHIDEventSystemDispatchEvent
 * -> the callback SpringBoard passed to IOHIDEventSystemOpen, service = NULL).
 * That callback turns digitizer events into GSEvent hand events, finds the
 * window/app under the finger and routes them, and turns consumer-page
 * keyboard events into menu/lock/volume button GSEvents - so we never build a
 * GSEventRecord ourselves and need no app/port bookkeeping.
 * Details and the disassembly notes: docs/input-design.md.
 *
 * Pure C, declarations local (stub SDK). Build: make (uses scripts/ios-cc).
 * Arguments: flipx flipy stretch debug   (see map_touch()).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

#include "input_proto.h"
#include "mcu_proto.h"

/* ---- libSystem ----------------------------------------------------- */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
extern mach_port_t bootstrap_port;
uint64_t mach_absolute_time(void);
kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
kern_return_t mach_port_type(mach_port_t task, mach_port_t name, unsigned *type);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
int sync(void);
int reboot(int howto);
#define RB_HALT 0x08            /* Darwin sys/reboot.h: halt after the BSD shutdown path */
#define MACH_PORT_TYPE_DEAD_NAME 0x100000u

/* ---- IOKit HID event SPI (IOKit.framework, 4.2.1 armv6) -------------
 * Argument layout verified against the dyld cache:
 *   IOHIDEventCreateDigitizerEvent        0x337fa3a8
 *   IOHIDEventCreateDigitizerFingerEvent  0x337fa568
 *   IOHIDEventCreateKeyboardEvent         0x337fa9dc
 *   IOHIDEventAppendEvent                 0x337fa114
 *   IOHIDEventSystemClientCreate          0x337fd5fc
 *   IOHIDEventSystemClientDispatchEvent   0x337fcad8
 * AbsoluteTime is a uint64 in r1:r2 (APCS, no register-pair alignment);
 * IOHIDFloat is a 32-bit float passed in core registers / on the stack
 * (stored 16.16 fixed); the Booleans are read as full words by the callee,
 * so they are declared int here.
 */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;

IOHIDEventRef IOHIDEventCreateDigitizerEvent(const void *allocator, uint64_t timeStamp,
        uint32_t transducerType, uint32_t index, uint32_t identity, uint32_t eventMask,
        uint32_t buttonMask, float x, float y, float z, float tipPressure,
        float barrelPressure, int range, int touch, uint32_t options);
IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(const void *allocator, uint64_t timeStamp,
        uint32_t index, uint32_t identity, uint32_t eventMask, float x, float y, float z,
        float tipPressure, float twist, int range, int touch, uint32_t options);
IOHIDEventRef IOHIDEventCreateKeyboardEvent(const void *allocator, uint64_t timeStamp,
        uint32_t usagePage, uint32_t usage, int down, uint32_t options);
void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child);
#ifdef INPUT_IOS3
/* iOS 3.x (7E18): IOKit has no IOHIDEventSystemClient API and SpringBoard
   serves no com.apple.iohideventsystem; every event already goes as a
   GSEvent to SpringBoard's PurpleSystemEventPort, so the client calls are
   no-ops and the connection check watches that port instead (re-log NEXT). */
#define IOHIDEventSystemClientDispatchEvent(c, e) ((void)(c), (void)(e))
#else
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(const void *allocator);
void IOHIDEventSystemClientDispatchEvent(IOHIDEventSystemClientRef client, IOHIDEventRef event);
#endif
void CFRelease(const void *cf);
typedef const struct __CFString *CFStringRef;
unsigned char CFStringGetCString(CFStringRef s, char *buf, long size, uint32_t encoding);
#define kCFStringEncodingUTF8 0x08000100u

#define kIOHIDDigitizerTransducerTypeHand 3
#define kIOHIDDigitizerEventRange    0x01
#define kIOHIDDigitizerEventTouch    0x02
#define kIOHIDDigitizerEventPosition 0x04

/* HID usages SpringBoard's keyboard handler (sub_5028c) acts on */
#define kHIDPage_Consumer            0x0C
#define kHIDUsage_Csmr_Power         0x30   /* lock button  -> GSEvent 1010/1011 */
#define kHIDUsage_Csmr_Menu          0x40   /* home button  -> GSEvent 1000/1001 */
#define kHIDUsage_Csmr_VolumeInc     0xE9   /*              -> GSEvent 1006/1007 */
#define kHIDUsage_Csmr_VolumeDec     0xEA   /*              -> GSEvent 1008/1009 */

/* IOHIDEventSystemClient: +8 = send right to the per-client server port
   that io_hideventsystem_open() returned (0 if SpringBoard was not there). */
#define CLIENT_PORT(c) (*(mach_port_t *)((char *)(c) + 8))

/* ---- 3DS pad bits (boot/common/ios3ds.h HID_*) --------------------- */
#define PAD_SELECT (1u << 2)
#define PAD_START  (1u << 3)
#define PAD_R      (1u << 8)
#define PAD_L      (1u << 9)
#define PAD_RIGHT  (1u << 4)
#define PAD_LEFT   (1u << 5)

#include "cpad.h"
#include "n3ds.h"
static cpad_t cpad;
static cstick_t cstick;
static int opt_nocpad, opt_non3ds, opt_norotate;

static const struct { uint16_t btn, usage; } keymap[] = {
    { PAD_START,  kHIDUsage_Csmr_Menu },
    { INBTN_HOME, kHIDUsage_Csmr_Menu },
    { PAD_SELECT, kHIDUsage_Csmr_Power },
    { INBTN_POWER, kHIDUsage_Csmr_Power },
    { PAD_R,      kHIDUsage_Csmr_VolumeInc },
    { PAD_L,      kHIDUsage_Csmr_VolumeDec },
};

static int opt_flipx, opt_flipy, opt_stretch, opt_debug;
static IOHIDEventSystemClientRef client;
static char app_id[256];                /* frontmost app the cached port belongs to */
static mach_port_t app_port;            /* its PurpleSystemEventPort send right (+1) */
static uint64_t home_grace_until;       /* touches go to SpringBoard after Menu */
#define HOME_GRACE_US 1500000ull
static uint64_t now_us(void);

/* Replace the cached app port and release the old send right: GSCopyPurpleNamedPort
   returns +1 and the old code dropped that right on every app switch. */
static void app_port_set(mach_port_t p)
{
    if (p == app_port) {
        if (p) mach_port_deallocate(mach_task_self_, p);
        return;
    }
    if (app_port) mach_port_deallocate(mach_task_self_, app_port);
    app_port = p;
}

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n, fd;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

/*
 * Bottom-screen panel -> normalised iOS screen position (0..1, origin top-left
 * of the 320x480 portrait UI).
 *
 * The ARM9 shows iOS on the top screen with iOS y along the 3DS x axis and
 * iOS x along the 3DS y axis (boot/arm9/display.c: column = 20 + iy*3/4,
 * row-from-top = ix*3/4). The touch panel uses the same axes, so:
 *     ny = panel x / 320            (480 pt over 320 px: 2/3 px per pt)
 *     nx = (panel y - 13.3) / 213.3 (same 2/3 scale, centred; the 13 px
 *                                    margins clamp to the screen edge)
 * i.e. the bottom screen is an absolute, aspect-true map of the iOS screen -
 * and becomes direct touch if the ARM9 also mirrors iOS there at 2/3 scale.
 * `stretch` uses the whole 240 px instead; `flipx`/`flipy` mirror an axis
 * (flipx is needed if display_blit is changed to ix = rr*4/3).
 */
static void map_touch(const insample_t *s, float *nx, float *ny)
{
    float px = (float)s->tx * (320.0f / 4096.0f);
    float py = (float)s->ty * (240.0f / 4096.0f);
    float x, y = px / 320.0f;

    if (opt_stretch) x = py / 240.0f;
    else             x = (py - (240.0f - 640.0f / 3.0f) / 2.0f) / (640.0f / 3.0f);
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;
    *nx = opt_flipx ? 1.0f - x : x;
    *ny = opt_flipy ? 1.0f - y : y;
}

/* One hand event with one finger child - the shape SBHIDInterface expects:
 *   parent: eventMask (Range|Touch on down/up, Position on move), touch, x, y
 *   child:  index, identity (<= 5), x, y, pressure, range, touch
 * Down = Range|Touch + touch 1, move = Position + touch 1,
 * up = Range|Touch + touch 0 (the child must still be present). */
static void send_touch_hid(float nx, float ny, int touch, uint32_t mask)
{
    uint64_t now = mach_absolute_time();
    IOHIDEventRef hand, finger;

    hand = IOHIDEventCreateDigitizerEvent(0, now, kIOHIDDigitizerTransducerTypeHand,
            0, 0, mask, 0, nx, ny, 0.0f, 0.0f, 0.0f, touch, touch, 0);
    if (!hand) return;
    finger = IOHIDEventCreateDigitizerFingerEvent(0, now, 1, 2, mask, nx, ny, 0.0f,
            touch ? 1.0f : 0.0f, 0.0f, touch, touch, 0);
    if (finger) {
        IOHIDEventAppendEvent(hand, finger);
        IOHIDEventSystemClientDispatchEvent(client, hand);
        CFRelease(finger);
    }
    CFRelease(hand);
}

static void send_touch_gs(float nx, float ny, int touch, uint32_t mask);

/* Touches: like the buttons, SpringBoard ignores digitizer IOHIDEvents that a
   client dispatches (QEMU, re-log 22), so they go as GraphicsServices hand
   events (send_touch_gs); IOHID is only the fallback. */
static void send_touch(float nx, float ny, int touch, uint32_t mask)
{
    send_touch_gs(nx, ny, touch, mask);
}

/* Hardware buttons: SpringBoard ignores keyboard IOHIDEvents that another
   client dispatches (verified in QEMU, re-log 19), so buttons go the
   iOS 3/4 way instead: a GSEventRecord to SpringBoard's PurpleSystemEventPort
   via GraphicsServices' GSSendSystemEvent (as Veency does). Record layout
   checked against _GSSendEvent in 8C148: infoSize at +0x30, 52 bytes. */
typedef struct {
    int32_t type, subtype;
    float x, y, wx, wy;
    uint32_t context;
    uint32_t ts_lo, ts_hi;          /* uint64 timestamp, 4-byte aligned on armv6 */
    uint32_t window, flags, pid, info_size;
} gsrec_t;
_Static_assert(sizeof(gsrec_t) == 52, "GSEventRecord (iOS 4)");

static int gs_type(uint32_t usage)
{
    switch (usage) {
    case kHIDUsage_Csmr_Menu:      return 1000;    /* kGSEventMenuButtonDown */
    case kHIDUsage_Csmr_VolumeInc: return 1006;
    case kHIDUsage_Csmr_VolumeDec: return 1008;
    case kHIDUsage_Csmr_Power:     return 1010;    /* kGSEventLockButtonDown */
    }
    return 0;
}

/* kGSEventHand (3001): GSEventRecord + GSHandInfo (0x24) + one GSPathInfo
   (0x18). Layout from 8C148: _GSEventGetHandInfo copies 0x24 bytes at
   record+0x34, _GSEventGetPathInfoAtIndex = record+0x58 + 24*i; UIKit's
   _UIApplicationHandleEvent reads pathInfosCount at hand+0x21, type 8 =
   cancel, and treats a path as touching while proximity bit 1 is set (it
   derives began/moved/ended from that and its own touch map). */
typedef struct {
    gsrec_t rec;
    struct {
        int32_t type;               /* 1 down, 2 dragged, 6 up (8 = cancel) */
        int16_t dx, dy;
        float f[6];
        uint8_t b20, count, pad[2];
    } hand;
    struct {
        uint8_t index, identity, proximity, pad;
        float pressure, major_radius;
        float x, y;
        uint32_t window;
    } path;
} gstouch_t;
_Static_assert(sizeof(gstouch_t) == 52 + 0x24 + 0x18, "GS hand event");

static void (*gs_send_event)(gsrec_t *, mach_port_t);
static mach_port_t (*gs_named_port)(const char *);
static mach_port_t (*sbs_port)(void);
static void (*sb_frontmost)(mach_port_t, char *);
static CFStringRef (*sb_frontmost_copy)(void);
static void (*gs_send_system)(gsrec_t *);
static uint64_t (*gs_timestamp)(void);

static void gs_load(void)
{
    static int done;
    void *h;
    if (done) return;
    done = 1;
    h = dlopen("/System/Library/PrivateFrameworks/GraphicsServices.framework/GraphicsServices", 1);
    if (h) {
        gs_send_system = (void (*)(gsrec_t *))dlsym(h, "GSSendSystemEvent");
        gs_send_event = (void (*)(gsrec_t *, mach_port_t))dlsym(h, "GSSendEvent");
        gs_named_port = (mach_port_t (*)(const char *))dlsym(h, "GSCopyPurpleNamedPort");
        gs_timestamp = (uint64_t (*)(void))dlsym(h, "GSCurrentEventTimestamp");
    }
    h = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 1);
    if (h) {
        sbs_port = (mach_port_t (*)(void))dlsym(h, "SBSSpringBoardServerPort");
        sb_frontmost = (void (*)(mach_port_t, char *))dlsym(h, "SBFrontmostApplicationDisplayIdentifier");
        /* The Copy variant returns NULL when SpringBoard/home is frontmost,
           while the old identifier function can keep returning the app that
           just went away (re-log 78/85). */
        sb_frontmost_copy = (CFStringRef (*)(void))dlsym(h, "SBSCopyFrontmostApplicationDisplayIdentifier");
    }
    if (!gs_send_system) say("[input] GraphicsServices unavailable; using IOHID");
}

/* SpringBoard shows something modal over the frontmost app? bit 0 locked,
   1 alert item, 2 full-screen alert: from ios3ds-applog.dylib in SpringBoard
   (notify state com.ios3ds.sb.modal), plus SBGetScreenLockStatus for the lock
   screen in case that dylib is not there */
static int sb_modal(void)
{
    static int tok = -1, tried;
    static uint32_t (*get)(int, uint64_t *);
    static void (*lockst)(mach_port_t, unsigned char *, unsigned char *);
    int m = 0;
    if (!tried) {
        tried = 1;
        uint32_t (*reg)(const char *, int *) = (uint32_t (*)(const char *, int *))dlsym(RTLD_DEFAULT, "notify_register_check");
        get = (uint32_t (*)(int, uint64_t *))dlsym(RTLD_DEFAULT, "notify_get_state");
        if (reg && reg("com.ios3ds.sb.modal", &tok) != 0) tok = -1;
        void *h = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 1);
        if (h) lockst = (void (*)(mach_port_t, unsigned char *, unsigned char *))dlsym(h, "SBGetScreenLockStatus");
    }
    if (tok >= 0 && get) {
        uint64_t st = 0;
        if (get(tok, &st) == 0) m = (int)(st & 7);
    }
    if (!(m & 1) && lockst && sbs_port) {
        unsigned char locked = 0, pass = 0;
        lockst(sbs_port(), &locked, &pass);
        if (locked) m |= 1;
    }
    return m;
}

/* A cached send right stays usable until the app exits; SpringBoard can also
   restart an app with the same bundle id, which leaves a dead name behind.
   Check before reuse so touches keep reaching the new process. */
static int mach_port_alive(mach_port_t p)
{
    unsigned type = 0;
    if (!p) return 0;
    if (mach_port_type(mach_task_self_, p, &type) != 0) return 0;
    return !(type & MACH_PORT_TYPE_DEAD_NAME);
}

static void send_touch_gs(float nx, float ny, int touch, uint32_t mask)
{
    gstouch_t e;
    uint64_t ts;
    float x = nx * 320.0f, y = ny * 480.0f;

    gs_load();
    if (!gs_send_system) {
        send_touch_hid(nx, ny, touch, mask);
        return;
    }
    memset(&e, 0, sizeof e);
    ts = gs_timestamp ? gs_timestamp() : mach_absolute_time();
    e.rec.type = 3001;                          /* kGSEventHand */
    e.rec.x = e.rec.wx = x;
    e.rec.y = e.rec.wy = y;
    e.rec.ts_lo = (uint32_t)ts;
    e.rec.ts_hi = (uint32_t)(ts >> 32);
    e.rec.pid = (uint32_t)getpid();
    e.rec.info_size = sizeof e.hand + sizeof e.path;
    e.hand.type = (mask & kIOHIDDigitizerEventPosition) ? 2 : touch ? 1 : 6;
    e.hand.count = 1;
    e.path.index = 1;
    e.path.identity = 2;
    e.path.proximity = touch ? 3 : 0;
    e.path.pressure = touch ? 1.0f : 0.0f;
    e.path.major_radius = touch ? 1.0f : 0.0f;
    e.path.x = x;
    e.path.y = y;

    /* frontmost app gets the touch on its own purple port; on the home screen
       (no app) SpringBoard gets it through the system event port */
    if (touch && !(mask & kIOHIDDigitizerEventPosition)) {   /* new touch: re-resolve */
        char id[256] = "";
        int modal = sb_modal();
        int grace = now_us() < home_grace_until;
        /* SpringBoard's lock screen or an alert over the app: the touch is
           SpringBoard's (issue #1: touch "locked up" after a popup / lock) */
        if (!modal && !grace) {
            if (sb_frontmost_copy) {
                CFStringRef fs = sb_frontmost_copy();
                if (fs) {
                    if (!CFStringGetCString(fs, id, sizeof id, kCFStringEncodingUTF8))
                        id[0] = 0;
                    CFRelease(fs);
                }
            } else if (sbs_port && sb_frontmost) {
                sb_frontmost(sbs_port(), id);
            }
        }
        /* Log every touch while modal: when SpringBoard covers the app its
           frontmost identifier can stay empty (QEMU), so a change-of-id test
           alone would hide the switch to the system event port. Re-resolve a
           dead port too, and force the system port during the Home grace:
           an app-targeted hand event after Home can kill the app/SB. */
        if (modal || grace || strcmp(id, app_id) || (app_port && !mach_port_alive(app_port))) {
            if (opt_debug && app_port && !mach_port_alive(app_port))
                say("[input] stale touch port for %s: re-resolving", app_id[0] ? app_id : "SpringBoard");
            mach_port_t p = (id[0] && gs_named_port) ? gs_named_port(id) : 0;
            if (opt_debug && id[0] && !p)
                say("[input] no touch port for %s; using SpringBoard", id);
            app_port_set(p);
            strncpy(app_id, id, sizeof app_id - 1);
            app_id[sizeof app_id - 1] = 0;
            if (opt_debug) say("[input] touch target %s%s", id[0] ? id : "SpringBoard",
                               modal & 6 ? " (alert)" : modal & 1 ? " (lock screen)" : "");
        }
    }
    if (app_port && gs_send_event) gs_send_event(&e.rec, app_port);
    else gs_send_system(&e.rec);
}

static void send_key(uint32_t usage, int down)
{
    int t = gs_type(usage);
    gs_load();
    if (gs_send_system && t) {
        gsrec_t r;
        uint64_t ts = gs_timestamp ? gs_timestamp() : mach_absolute_time();
        memset(&r, 0, sizeof r);
        r.type = t + (down ? 0 : 1);
        r.ts_lo = (uint32_t)ts;
        r.ts_hi = (uint32_t)(ts >> 32);
        r.pid = (uint32_t)getpid();
        gs_send_system(&r);
        return;
    }
    IOHIDEventRef ev = IOHIDEventCreateKeyboardEvent(0, mach_absolute_time(),
            kHIDPage_Consumer, usage, down, 0);
    if (!ev) return;
    IOHIDEventSystemClientDispatchEvent(client, ev);
    CFRelease(ev);
}

/* SpringBoard arms a hold timer when it handles a button-down (lock: 2.5 s
   -> power-off sheet; menu: Voice Control / hint). Its run loop can lag by
   seconds (QEMU, software compositor), so a separately sent up can lose the
   race and every tap becomes a hold (re-log 19/20). Short taps are therefore
   sent as down+up back to back on release; the down of a real hold is sent
   once the button has been held for HOLD_US. */
#define NKEYS   (sizeof keymap / sizeof keymap[0])
#define HOLD_US 800000ull
static uint8_t key_state[NKEYS];        /* 0 idle, 1 down pending, 2 down sent */
static uint64_t key_t0[NKEYS];

/* poll-loop time: the sum of the loop's sleeps (16 ms, or 100 ms while the
   screen is off); mach_timebase_info is not usable here */
static uint64_t loops, slept_us;
static uint64_t now_us(void) { return slept_us; }

/* SpringBoard's frontmost-app state lags the Home animation: a touch sent
   right after Home still resolves the app that is being closed (and in QEMU
   the app-targeted GSEvent panics the kernel, re-log 85). Send touches to
   SpringBoard for a moment after Menu; the app port is dropped then anyway. */
static void key_send(unsigned i, int down)
{
    if (opt_debug) say("[input] key %02x %s @%u", keymap[i].usage, down ? "down" : "up", (unsigned)loops);
    send_key(keymap[i].usage, down);
}

static void key_edge(unsigned i, int down)
{
    if (down) {
        key_state[i] = 1;
        key_t0[i] = now_us();
    } else {
        if (key_state[i] == 1) key_send(i, 1);
        if (key_state[i]) key_send(i, 0);
        if (keymap[i].usage == kHIDUsage_Csmr_Menu)
            home_grace_until = now_us() + HOME_GRACE_US;
        key_state[i] = 0;
    }
}

static void key_tick(void)
{
    unsigned i;
    for (i = 0; i < NKEYS; i++)
        if (key_state[i] == 1 && now_us() - key_t0[i] >= HOLD_US) {
            key_send(i, 1);
            key_state[i] = 2;
        }
}

static void apply(const insample_t *prev, const insample_t *cur)
{
    unsigned i;
    uint16_t diff = prev->buttons ^ cur->buttons;
    int was = prev->flags & INFLAG_PEN, is = cur->flags & INFLAG_PEN;
    float nx, ny;

    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (diff & keymap[i].btn)
            key_edge(i, (cur->buttons & keymap[i].btn) != 0);
    if ((diff & cur->buttons & INBTN_ZR) && !opt_non3ds) {     /* New 3DS: n3ds.h */
        n3key_t k[4];
        int n = zr_keys(1, k);
        if (opt_debug) say("[input] ZR: screenshot");
        for (i = 0; i < (unsigned)n; i++) {
            if (k[i].wait_ms) usleep(k[i].wait_ms * 1000u);
            send_key(k[i].key == N3K_MENU ? kHIDUsage_Csmr_Menu : kHIDUsage_Csmr_Power, k[i].down);
        }
    }
    if (is && !was) {
        if (cpad.finger) {                  /* a real touch ends the virtual finger first */
            send_touch(cpad.x, cpad.y, 0, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
            cpad.finger = 0;
            cpad.swipe = 0;
        }
        map_touch(cur, &nx, &ny);
        if (opt_debug) say("[input] down %d,%d", (int)(nx * 320), (int)(ny * 480));
        send_touch(nx, ny, 1, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
    } else if (is && (cur->tx != prev->tx || cur->ty != prev->ty)) {
        map_touch(cur, &nx, &ny);
        send_touch(nx, ny, 1, kIOHIDDigitizerEventPosition);
    } else if (!is && was) {
        map_touch(prev, &nx, &ny);          /* lift where the finger last was */
        if (opt_debug) say("[input] up %d,%d", (int)(nx * 320), (int)(ny * 480));
        send_touch(nx, ny, 0, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
    }
}

#ifdef INPUT_IOS3
static mach_port_t purple_port;
#endif
static int client_alive(void)
{
    unsigned type = 0;
#ifdef INPUT_IOS3
    mach_port_t p = purple_port;
#else
    mach_port_t p = CLIENT_PORT(client);
#endif
    if (!p) return 0;
    if (mach_port_type(mach_task_self_, p, &type) != 0) return 0;
    return !(type & MACH_PORT_TYPE_DEAD_NAME);
}

int main(int argc, char **argv)
{
    int mib[8], mcumib[8], i;
    size_t mculen = 8;
    int have_mcu = 0, screen_off = 0;
    uint32_t last_power_holds = 0;
    int mcu_seen = 0, poweroff_started = 0;
    size_t miblen = 8, len;
    inmbox_t box;
    insample_t state;
    uint32_t tail = 0, idle = 0;
    int synced = 0;
    mach_port_t svc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "flipx")) opt_flipx = 1;
        else if (!strcmp(argv[i], "flipy")) opt_flipy = 1;
        else if (!strcmp(argv[i], "stretch")) opt_stretch = 1;
        else if (!strcmp(argv[i], "debug")) opt_debug = 1;
        else if (!strcmp(argv[i], "nocpad")) opt_nocpad = 1;
        else if (!strcmp(argv[i], "non3ds")) opt_non3ds = 1;
        else if (!strcmp(argv[i], "norotate")) opt_norotate = 1;
    }

    /* the shim registers the sysctl at root mount; no sysctl = old shim */
    while (sysctlnametomib(INPUT_SYSCTL_NAME, mib, &miblen) != 0) {
        if (!idle++) say("[input] no %s (shim without input support); waiting", INPUT_SYSCTL_NAME);
        miblen = 8;
        sleep(10);
    }

#ifdef INPUT_IOS3
    /* SpringBoard checks in PurpleSystemEventPort once its event loop runs */
    while (bootstrap_look_up(bootstrap_port, "PurpleSystemEventPort", &svc) != 0)
        sleep(1);
    purple_port = svc;
    gs_load();
    if (!gs_send_system) {
        say("[input] GraphicsServices GSSendSystemEvent missing; retrying");
        sleep(2);
        return 1;
    }
    say("[input] connected to SpringBoard's PurpleSystemEventPort (pid %d)", getpid());
#else
    /* SpringBoard's launchd job owns the service; wait until it is known */
    while (bootstrap_look_up(bootstrap_port, "com.apple.iohideventsystem", &svc) != 0)
        sleep(1);
    mach_port_deallocate(mach_task_self_, svc);

    /* blocks in io_hideventsystem_open until SpringBoard serves the port */
    client = IOHIDEventSystemClientCreate(0);
    if (!client || !client_alive()) {
        say("[input] cannot open com.apple.iohideventsystem; retrying");
        sleep(2);
        return 1;                           /* launchd KeepAlive restarts us */
    }
    /* The pid makes a KeepAlive restart unambiguous in a console log: a
       SpringBoard restart and an input-daemon crash otherwise print the same
       line (re-log 100). */
    say("[input] connected to SpringBoard's HID event system (pid %d)", getpid());
#endif

    memset(&state, 0, sizeof state);
    for (idle = 0;; idle++) {
        len = sizeof box;
        if (sysctl(mib, (unsigned)miblen, &box, &len, 0, 0) != 0 || len != sizeof box ||
            box.magic != INPUT_MAGIC || box.version != INPUT_VERSION) {
            sleep(1);                       /* ARM9 without input support (yet) */
            continue;
        }
        if (!synced) {
            /* adopt the current state without replaying history, but never
               start with a finger or button "already down" */
            tail = box.head;
            synced = 1;
            say("[input] mailbox up: touch %s, flipx=%d flipy=%d stretch=%d",
                box.tsc_ok ? "ok" : "absent", opt_flipx, opt_flipy, opt_stretch);
        }
        if (box.head - tail >= INPUT_RING)  /* overrun: the oldest slot may be in flight */
            tail = box.head - (INPUT_RING - 1);
        while (tail != box.head) {
            const insample_t *s = &box.s[tail % INPUT_RING];
            apply(&state, s);
            state = *s;
            tail++;
        }
        if ((idle & 63) == 0 && !client_alive()) {
            say("[input] SpringBoard went away; restarting");
            return 0;                       /* reconnect via launchd */
        }
        loops++;
        key_tick();
        if (synced && !screen_off && !opt_nocpad) {    /* circle pad / D-pad gestures */
            cpad_out_t o;
            unsigned dpad = dpad_screen(state.buttons, !opt_norotate);
            int sx, sy;
            if (!opt_non3ds) {          /* New 3DS C-stick flicks: pages and page scrolls (n3ds.h) */
                static int said;
                if ((box.ext & INEXT_VALID) && !said++) say("[input] New 3DS C-stick, ZL/ZR: on");
                stick_screen(INEXT_X(box.ext), INEXT_Y(box.ext), !opt_norotate, &sx, &sy);
                dpad |= cstick_dirs(&cstick, sx, sy, (box.ext & INEXT_VALID) != 0);
                if (state.buttons & INBTN_ZL) dpad |= 16u;     /* ZL: tap the status bar (scroll to top) */
            }
            stick_screen(state.cx, state.cy, !opt_norotate, &sx, &sy);
            cpad_step(&cpad, sx, sy, (state.flags & INFLAG_CPAD) != 0,
                      (state.flags & INFLAG_PEN) != 0, dpad, &o);
            if (o.ev == CP_DOWN || o.ev == CP_UP) {
                if (opt_debug) say("[input] cpad %s %d,%d", o.ev == CP_DOWN ? "down" : "up",
                                   (int)(o.x * 320), (int)(o.y * 480));
                send_touch(o.x, o.y, o.ev == CP_DOWN, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
            } else if (o.ev == CP_MOVE) {
                send_touch(o.x, o.y, 1, kIOHIDDigitizerEventPosition);
            }
        }
        /* screen off (ARM9 power saving, docs/mcu.md section 9): nothing to
           do with touches and iOS sleeps, so poll at 10 Hz instead of 60.
           Button samples queue up in the mailbox ring (30 deep), so a HOME
           or POWER press is still seen, within 100 ms. */
        if ((loops & (screen_off ? 3 : 15)) == 0) {     /* every 0.25 s on, 0.4 s off */
            mcubox_t mb;
            size_t ml = sizeof mb;
            if (!have_mcu) {
                mculen = 8;
                have_mcu = sysctlnametomib(MCU_SYSCTL_NAME, mcumib, &mculen) == 0 ? 1 : -1;
            }
            if (have_mcu > 0 && sysctl(mcumib, (unsigned)mculen, &mb, &ml, 0, 0) == 0 && ml == sizeof mb &&
                mb.magic == MCU_MAGIC) {
                int off = mb.ps_on && mb.screen_off;
                if (off != screen_off && opt_debug) say("[input] screen %s: polling at %d Hz", off ? "off" : "on", off ? 10 : 60);
                screen_off = off;
                /* The MCU's POWER-held IRQ arrives while the key is still down
                   (3 s in QEMU, before the console's ~10 s hard cut). The ARM9
                   counts those events in mcubox.power_holds; use the count to
                   start iOS's own clean shutdown instead of waiting for the
                   user to drag "slide to power off". reboot(RB_HALT) is the
                   same call launchd makes after its shutdown handlers have
                   unmounted the file systems, so md0 ends up clean. Adopt the
                   first value (a POWER press held through boot is not a
                   shutdown request). */
                if (!poweroff_started) {
                    if (!mcu_seen) {
                        last_power_holds = mb.power_holds;
                        mcu_seen = 1;
                    } else if (mb.power_holds != last_power_holds) {
                        last_power_holds = mb.power_holds;
                        poweroff_started = 1;
                        say("[input] POWER held on the 3DS (count %u): sync + clean shutdown",
                            (unsigned)mb.power_holds);
                        sync();
                        reboot(RB_HALT);
                        say("[input] reboot(RB_HALT) returned; leaving shutdown to the slider");
                    }
                }
            }
        }
        {
            unsigned us = screen_off ? 100000 : 16000;
            usleep(us);
            slept_us += us;
        }
    }
}
