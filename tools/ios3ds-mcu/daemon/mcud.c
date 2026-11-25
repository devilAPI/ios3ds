/*
 * ios3ds-mcud: the 3DS system controller (MCU) -> iOS userland.
 *
 *   ARM9 monitor --(mcubox_t in the shim page)--> sysctl kern.ios3ds_mcu
 *     -> this daemon:
 *        volume slider        -> AVSystemController setActiveCategoryVolumeTo:
 *        accelerometer        -> IOHID accelerometer events (type 13) into
 *                                SpringBoard's HID event system (orientation,
 *                                UIAccelerometer)
 *     <- iOS brightness: SpringBoard's HID event system properties
 *        DisplayBrightness x DisplayBrightnessFactor -> sysctl
 *        kern.ios3ds_mcuctl -> the shim writes the LCD backlight PWM
 *
 * Reading the sysctl also lets the shim refresh its IOPMPowerSource (battery
 * icon). The RTC, HOME/POWER and shell close/open never pass through here
 * (kernel calendar, input mailbox, ARM9). See docs/mcu.md.
 *
 * Arguments: debug (log every change), acc (turn the accelerometer on:
 * off by default, SpringBoard ignores client-dispatched IOHID events, see
 * docs/mcu.md section 6.2), acc=AXES (implies acc; default "-y,-x,-z": iOS x,
 * y, z from the 3DS axes), novol, nobl.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <math.h>

#include "mcu_proto.h"
#include "als_curve.h"
#include "backlight.h"

/* ---- libSystem ----------------------------------------------------------- */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t bootstrap_port;
extern mach_port_t mach_task_self_;
uint64_t mach_absolute_time(void);
kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
kern_return_t mach_port_type(mach_port_t task, mach_port_t name, unsigned *type);
#define MACH_PORT_TYPE_DEAD_NAME 0x100000u
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
struct tv32 { long tv_sec; int tv_usec; };     /* armv6 Darwin timeval */
int gettimeofday(void *tv, void *tz);
struct mtb { unsigned numer, denom; };
int mach_timebase_info(struct mtb *);
uint32_t notify_register_check(const char *name, int *token);
uint32_t notify_get_state(int token, uint64_t *state);

/* ---- CoreFoundation / IOKit HID (layouts as in tools/ios3ds-input) ------- */
typedef const void *CFTypeRef;
typedef const void *CFBooleanRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFNumber *CFNumberRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
CFStringRef CFStringCreateWithCString(const void *alloc, const char *s, uint32_t enc);
CFTypeRef CFPreferencesCopyAppValue(CFStringRef key, CFStringRef applicationID);
unsigned long CFGetTypeID(CFTypeRef cf);
unsigned long CFBooleanGetTypeID(void);
unsigned char CFBooleanGetValue(CFBooleanRef b);
unsigned long CFNumberGetTypeID(void);
unsigned char CFNumberGetValue(CFNumberRef n, long type, void *out);
void CFRelease(CFTypeRef cf);
#define kCFStringEncodingUTF8 0x08000100u
#define kCFNumberFloatType    12
#define kCFNumberIntType      3

int IOPMAssertionCreateWithName(CFStringRef type, uint32_t level, CFStringRef name, uint32_t *id);
#define kIOPMAssertionLevelOn 255
#ifdef MCU_OLD
/* iOS 2.x/3.x (-DMCU_OLD): IOKit has no IOHIDEventSystemClient API and
   SpringBoard serves no com.apple.iohideventsystem, so there is never a
   client: no brightness properties to read and no accelerometer events to
   send. Battery (the sysctl read refreshes the shim's IOPMPowerSource), the
   volume slider (Celestial's AVSystemController) and the no-sleep assertion
   still work (re-log NEXT). */
#define IOHIDEventSystemClientCreate(a) ((IOHIDEventSystemClientRef)0)
#define IOHIDEventSystemClientCopyProperty(c, k) ((void)(c), (void)(k), (CFTypeRef)0)
#define IOHIDEventSystemClientDispatchEvent(c, e) ((void)(c), (void)(e))
#define IOHIDEventCreateAccelerometerEvent(a, t, x, y, z, o) ((void)(x), (void)(y), (void)(z), (IOHIDEventRef)0)
#define SB_UP 1
#else
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(const void *allocator);
CFTypeRef IOHIDEventSystemClientCopyProperty(IOHIDEventSystemClientRef client, CFStringRef key);
void IOHIDEventSystemClientDispatchEvent(IOHIDEventSystemClientRef client, IOHIDEventRef event);
/* 0x337faca4 in the 4.2.1 dyld cache: (alloc r0, timestamp r1:r2, x r3,
   y [sp], z [sp+4], options [sp+8]) -> the common 3-axis creator, type 13 */
IOHIDEventRef IOHIDEventCreateAccelerometerEvent(const void *allocator, uint64_t timeStamp,
        float x, float y, float z, uint32_t options);
#define SB_UP client
#endif

/* ---- Objective-C (AVSystemController lives in Celestial.framework) ------- */
typedef void *id;
typedef void *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
#define MSG(type, obj, sel) ((type)objc_msgSend)(obj, sel_registerName(sel)

static int opt_debug, opt_noacc = 1, opt_novol, opt_nobl, opt_sleepok, opt_noals;
static void say(const char *fmt, ...);

/* Clock cross-check (the open "gettimeofday vs mach_absolute_time" question,
   docs/mcu.md): the same interval measured by the 3DS RTC (ARM9 mailbox, 1 s
   resolution), gettimeofday() and mach_absolute_time(). */
static struct { struct tv32 tv; uint64_t m; uint32_t rtc; int set; } clk0;

static void clock_check(const mcubox_t *b, const char *when)
{
    struct tv32 tv;
    struct mtb tb = { 0, 0 };
    uint64_t m = mach_absolute_time();
    gettimeofday(&tv, 0);
    mach_timebase_info(&tb);
    if (!clk0.set) {
        clk0.tv = tv; clk0.m = m; clk0.rtc = b->rtc_utc; clk0.set = 1;
        return;
    }
    double gtod = (tv.tv_sec - clk0.tv.tv_sec) * 1000.0 + (tv.tv_usec - clk0.tv.tv_usec) / 1000.0;
    double mach = tb.denom ? (double)(m - clk0.m) * tb.numer / tb.denom / 1e6 : -1;
    say("[mcud] clocks %s: RTC +%d s, gettimeofday +%.0f ms, mach_absolute_time +%.0f ms (%llu ticks, timebase %u/%u)",
        when, (int)(b->rtc_utc - clk0.rtc), gtod, mach, (unsigned long long)(m - clk0.m), tb.numer, tb.denom);
}
static int accmap[3][2] = { { 1, -1 }, { 0, -1 }, { 2, -1 } };   /* iOS axis <- (3DS axis, sign) */

static void say(const char *fmt, ...)            /* one line to the console (= the 3DS log) */
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

static IOHIDEventSystemClientRef client;
static int sleep_assertion;             /* NoIdleSleepAssertion held (once per process) */

/* SpringBoard can restart (a crash, or launchd killing it): the per-client
   Mach port at +8 then becomes a dead name and every brightness/accelerometer
   call silently fails. Detect it and connect again, like ios3ds-input does. */
static int client_alive(void)
{
    unsigned type = 0;
    mach_port_t p = client ? *(mach_port_t *)((char *)client + 8) : 0;
    if (!p) return 0;
    if (mach_port_type(mach_task_self_, p, &type) != 0) return 0;
    return !(type & MACH_PORT_TYPE_DEAD_NAME);
}

static int parse_accmap(const char *s)
{
    int m[3][2], i = 0;
    while (i < 3 && *s) {
        int sign = 1;
        if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
        if (*s < 'x' || *s > 'z') return 0;
        m[i][0] = *s - 'x';
        m[i][1] = sign;
        s++;
        i++;
        if (*s == ',') s++;
    }
    if (i != 3) return 0;
    memcpy(accmap, m, sizeof m);
    return 1;
}

/* ---- volume -------------------------------------------------------------- */
static id avsc;

static int volume_set(float v)
{
    if (!avsc) {
        id cls;
        dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_LAZY);
        cls = objc_getClass("AVSystemController");
        if (cls) avsc = MSG(id (*)(id, SEL), cls, "sharedAVSystemController"));
        if (!avsc) return 0;
    }
    return MSG(signed char (*)(id, SEL, float), avsc, "setActiveCategoryVolumeTo:"), v) != 0;
}

/* ---- brightness ---------------------------------------------------------- */
static CFStringRef k_bright, k_factor;

/* SpringBoard's Settings > Wallpaper & Brightness switch ("Auto-Brightness")
   lives in com.apple.springboard as SBEnableALS. On an iPod2,1 that key is
   normally absent (no ALS hardware); when absent we leave auto-brightness
   on, as on the real iPhone/iPod models whose Settings shows the switch.
   The boot.cfg auto_brightness=0 switch is enforced by the ARM9 (the mailbox
   then says there is no sensor), so both kill switches have the same effect. */
static int als_pref(void)
{
    static CFStringRef key, app;
    if (!key) {
        key = CFStringCreateWithCString(0, "SBEnableALS", kCFStringEncodingUTF8);
        app = CFStringCreateWithCString(0, "com.apple.springboard", kCFStringEncodingUTF8);
    }
    CFTypeRef v = CFPreferencesCopyAppValue(key, app);
    if (!v) return -1;
    int on = 1;
    if (CFGetTypeID(v) == CFBooleanGetTypeID())
        on = CFBooleanGetValue((CFBooleanRef)v) != 0;
    else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int n = 1;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n)) on = n != 0;
    }
    CFRelease(v);
    return on;
}

static als_smooth_t als;
static float als_factor = 1.0f;       /* kept between loops: the shim is written on every slider change */
static int als_on = -1, als_pref_last = -1, als_paused_logged;
static uint8_t als_luma_last;
static float als_factor_last = -1;

static int copy_float(CFStringRef key, float *out)
{
    CFTypeRef v = client ? IOHIDEventSystemClientCopyProperty(client, key) : 0;
    int ok = 0;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            ok = CFNumberGetValue((CFNumberRef)v, kCFNumberFloatType, out);
        CFRelease(v);
    }
    return ok;
}

int main(int argc, char **argv)
{
    int mib[8], cmib[8], i;
    size_t miblen = 8, cmiblen = 8, len;
    mcubox_t box;
    struct { uint32_t acc, backlight, _r[2]; } ctl, ctl_sent;
    uint32_t loops = 0, seen_seq = 0, acc_seen = 0, waits = 0;
    int synced = 0, vol_last = -1, vol_pending = 0;
    float g_avg = 0, bright = -1, factor = -1;
    int acc_dom = -1, orient_tok = -1, orient_last = -1;
    uint32_t orient_check = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "debug")) opt_debug = 1;
        else if (!strcmp(argv[i], "noacc")) opt_noacc = 1;
        else if (!strcmp(argv[i], "acc")) opt_noacc = 0;
        else if (!strcmp(argv[i], "novol")) opt_novol = 1;
        else if (!strcmp(argv[i], "nobl")) opt_nobl = 1;
        else if (!strcmp(argv[i], "noals")) opt_noals = 1;
        else if (!strcmp(argv[i], "sleepok")) opt_sleepok = 1;
        else if (!strncmp(argv[i], "acc=", 4)) {
            opt_noacc = 0;
            if (!parse_accmap(argv[i] + 4)) say("[mcud] bad %s (want e.g. acc=-y,-x,-z); keeping the default", argv[i]);
        }
    }

    while (sysctlnametomib(MCU_SYSCTL_NAME, mib, &miblen) != 0 ||
           sysctlnametomib("kern.ios3ds_mcuctl", cmib, &cmiblen) != 0) {
        if (!waits++) say("[mcud] no %s (shim without MCU support); waiting", MCU_SYSCTL_NAME);
        miblen = cmiblen = 8;
        sleep(10);
    }
    memset(&ctl, 0, sizeof ctl);
    memset(&ctl_sent, 0xFF, sizeof ctl_sent);
    k_bright = CFStringCreateWithCString(0, "DisplayBrightness", kCFStringEncodingUTF8);
    k_factor = CFStringCreateWithCString(0, "DisplayBrightnessFactor", kCFStringEncodingUTF8);

    for (;; loops++) {
        len = sizeof box;
        if (sysctl(mib, (unsigned)miblen, &box, &len, 0, 0) != 0 || len != sizeof box ||
            box.magic != MCU_MAGIC || box.version != MCU_VERSION || box.state != MCU_ST_OK) {
            if (!synced && !waits++)
                say("[mcud] MCU mailbox not live (magic %08x state %u): nothing to do",
                    (unsigned)box.magic, (unsigned)box.state);
            sleep(5);
            continue;
        }
        if (!synced) {
            synced = 1;
            say("[mcud] MCU version %02x/%02x: battery %u%%%s%s, volume slider %u/63, shell %s, RTC %u",
                box.fw >> 8, box.fw & 0xFF, box.batt_percent, (box.flags & MCUF_ADAPTER) ? ", charger" : "",
                (box.flags & MCUF_CHARGING) ? ", charging" : "", box.vol_raw,
                (box.flags & MCUF_SHELL_OPEN) ? "open" : "closed", (unsigned)box.rtc_utc);
            clock_check(&box, "start");
        }
        if (loops == 600) clock_check(&box, "after ~60 s");
        if (loops == 3000) clock_check(&box, "after ~300 s");

        /* SpringBoard owns the HID event system; connect once it is there,
           and reconnect after it restarts (the old client's port dies). */
        if (client && (loops % 20) == 0 && !client_alive()) {
            say("[mcud] SpringBoard's HID event port went away; reconnecting");
            CFRelease(client);
            client = 0;
        }
        if (!client && (loops % 20) == 0) {
            mach_port_t svc = 0;
            if (bootstrap_look_up(bootstrap_port, "com.apple.iohideventsystem", &svc) == 0) {
                mach_port_deallocate(mach_task_self_, svc);
                client = IOHIDEventSystemClientCreate(0);
                if (client && !client_alive()) {
                    CFRelease(client);          /* SpringBoard went away while opening */
                    client = 0;
                }
                if (client) say("[mcud] connected to SpringBoard's HID event system");
                /* No system sleep: after a lock iOS would put the whole system
                   to sleep ("System Sleep"), and nothing on the 3DS can wake it
                   (the PMU interrupt that does on an iPod does not exist). The
                   screen still goes off; the CPUs idle (docs/mcu.md section 9).
                   One assertion for the life of this process: a reconnect must
                   not stack another one (and the two CFStrings are reused). */
                if (client && !opt_sleepok && !sleep_assertion) {
                    static CFStringRef k_idle, k_why;
                    uint32_t aid = 0;
                    int r;
                    if (!k_idle) k_idle = CFStringCreateWithCString(0, "NoIdleSleepAssertion", kCFStringEncodingUTF8);
                    if (!k_why) k_why = CFStringCreateWithCString(0, "ios3ds: no wake source for system sleep",
                                                                  kCFStringEncodingUTF8);
                    r = IOPMAssertionCreateWithName(k_idle, kIOPMAssertionLevelOn, k_why, &aid);
                    if (!r) sleep_assertion = 1;
                    say("[mcud] NoIdleSleepAssertion %s (0x%x, id %u)", r ? "FAILED" : "held", (unsigned)r, (unsigned)aid);
                }
            }
        }

#ifdef MCU_OLD
        /* no HID client to wait for: take the no-sleep assertion once */
        if (!opt_sleepok && !sleep_assertion && (loops % 20) == 0) {
            static CFStringRef k_idle, k_why;
            uint32_t aid = 0;
            int r;
            if (!k_idle) k_idle = CFStringCreateWithCString(0, "NoIdleSleepAssertion", kCFStringEncodingUTF8);
            if (!k_why) k_why = CFStringCreateWithCString(0, "ios3ds: no wake source for system sleep",
                                                          kCFStringEncodingUTF8);
            r = IOPMAssertionCreateWithName(k_idle, kIOPMAssertionLevelOn, k_why, &aid);
            if (!r) sleep_assertion = 1;
            if (!r || loops == 0)
                say("[mcud] NoIdleSleepAssertion %s (0x%x, id %u)", r ? "FAILED" : "held", (unsigned)r, (unsigned)aid);
        }
#endif
        /* volume slider: absolute, 0..63 -> 0..1; move iOS only when the
           slider moved (by 2 raw steps or to an end), so iOS' own buttons
           (L/R) keep working in between */
        if (!opt_novol && (box.valid & MCUV_SLIDERS)) {
            int v = box.vol_raw;
            if (vol_last < 0 || abs(v - vol_last) >= 2 || ((v == 0 || v == 63) && v != vol_last)) {
                vol_last = v;
                vol_pending = 1;
            }
            if (vol_pending && SB_UP) {
                float f = v / 63.0f;
                if (volume_set(f)) {
                    vol_pending = 0;
                    if (opt_debug || loops < 50) say("[mcud] volume slider %d/63 -> iOS volume %.2f", v, f);
                } else if ((loops % 100) == 0) {
                    say("[mcud] AVSystemController not available yet (volume %d/63 pending)", v);
                }
            }
        }

        /* screen off (ARM9 power saving): poll the brightness every (slower)
           loop, so the wake's backlight reaches the LCD at once */
        int scr_off = box.ps_on && box.screen_off;

        /* Auto-brightness (New models only): the ARM9 samples the inner
           camera's AE luminance into the mailbox. The Settings switch
           SBEnableALS decides whether we use it; with no key (iPod2,1
           default) it is on. Smoothing/hysteresis are in als_curve.h. */
        if ((loops % 50) == 0) als_pref_last = als_pref();
        int pref_on = als_pref_last < 0 ? 1 : als_pref_last;
        /* als_state 2: a camera client owns the sensors; the ARM9 holds the
           last luma and we hold the factor we already applied (no jump). */
        int want_als = !opt_noals && pref_on && box.als_state != 0 && (box.valid & MCUV_ALS) != 0;
        if (want_als != als_on) {
            say("[mcud] auto-brightness %s (SBEnableALS %s, ambient luma %u/255)",
                want_als ? "on" : "off", als_pref_last < 0 ? "absent" : als_pref_last ? "1" : "0",
                (unsigned)box.als_luma);
            als_on = want_als;
            als.valid = 0;                      /* snap to the current light level */
            als_factor = want_als ? als_lux_to_factor(als_luma_to_lux(box.als_luma)) : 1.0f;
            als_factor_last = als_factor;
        }
        int paused = als_on && box.als_state == 2;
        if (paused != als_paused_logged) {
            say("[mcud] auto-brightness %s (camera in use; holding factor %.2f)",
                paused ? "paused" : "resumed", (double)als_factor);
            als_paused_logged = paused;
        }
        if (als_on && !paused && (loops % 5) == 0 && bright >= 0.0f) {
            als_factor = als_smooth(&als, box.als_luma, scr_off ? 250u : 100u);
            if (fabsf(als_factor - als_factor_last) > 0.004f ||
                (box.als_luma != als_luma_last && fabsf(als_factor - als_factor_last) > 0.001f)) {
                say("[mcud] auto-brightness: ambient luma %u/255 (~%.0f lux) -> factor %.2f, "
                    "brightness %.2f x %.2f -> LCD backlight %u/1000",
                    (unsigned)box.als_luma, als_luma_to_lux(box.als_luma), als_factor,
                    bright < 0 ? 0.0f : bright, factor < 0 ? 1.0f : factor,
                    (unsigned)((bright < 0 ? 0.0f : bright) * (factor < 0 ? 1.0f : factor) *
                               als_factor * 1000.0f + 0.5f));
                als_factor_last = als_factor;
            }
            als_luma_last = box.als_luma;
        }

        /* iOS brightness -> LCD backlight, 2x per second */
        if (!opt_nobl && client && (scr_off || (loops % 5) == 0)) {
            float b = -1, fa = -1;
            int hb = copy_float(k_bright, &b), hf = copy_float(k_factor, &fa);
            if (hb && !hf) fa = 1;
            float af = als_on ? als_factor : 1.0f;
            if (hb && (fabsf(b - bright) > 0.005f || fabsf(fa - factor) > 0.005f)) {
                bright = b;
                factor = fa;
                say("[mcud] iOS brightness %.2f x factor %.2f%s -> LCD backlight %u/1000%s", b, fa,
                    als_on ? " x auto-brightness" : "",
                    (unsigned)ios_backlight_level(b, fa * af),
                    fa > 0.0f && b * fa * af < (float)IOS_BL_MIN_LEVEL / 1000.0f ? " (minimum)" : "");
            }
            if (hb) {
                /* level 0 stays "screen off" (factor 0); a slider at 0 with
                   the screen on is clamped so the LCD is still readable. */
                ctl.backlight = 1 + ios_backlight_level(bright, factor * af);
            } else if (bright < -0.5f && loops == 50) {
                say("[mcud] SpringBoard reports no DisplayBrightness: LCD backlight left as Luma set it");
            }
        }

        /* accelerometer while the screen is on */
        ctl.acc = !opt_noacc && client && factor != 0.0f;
        if (ctl.acc && box.acc_samples != acc_seen && (box.valid & MCUV_ACC)) {
            float r[3], g, out[3];
            acc_seen = box.acc_samples;
            for (i = 0; i < 3; i++) r[i] = box.acc[i];
            g = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (g > 1.0f) {
                /* scale: the running mean of |a| is 1 g (the raw unit is not
                   documented; this makes the mapping independent of it) */
                g_avg = g_avg > 0 ? g_avg * 0.98f + g * 0.02f : g;
                for (i = 0; i < 3; i++) out[i] = accmap[i][1] * r[accmap[i][0]] / g_avg;
                IOHIDEventRef ev = IOHIDEventCreateAccelerometerEvent(0, mach_absolute_time(),
                                                                      out[0], out[1], out[2], 0);
                if (ev) {
                    IOHIDEventSystemClientDispatchEvent(client, ev);
                    CFRelease(ev);
                }
                /* log when the dominant axis changes, with SpringBoard's own
                   view of it (notify state com.apple.springboard.rawOrientation) */
                {
                    int dom = 0, k;
                    for (k = 1; k < 3; k++) if (fabsf(out[k]) > fabsf(out[dom])) dom = k;
                    dom = dom * 2 + (out[dom] < 0);
                    if (dom != acc_dom) {
                        uint64_t st = 0;
                        if (orient_tok < 0) notify_register_check("com.apple.springboard.rawOrientation", &orient_tok);
                        if (orient_tok >= 0) notify_get_state(orient_tok, &st);
                        say("[mcud] accelerometer raw %d %d %d -> iOS %.2f %.2f %.2f g (SpringBoard rawOrientation %u)",
                            box.acc[0], box.acc[1], box.acc[2], out[0], out[1], out[2], (unsigned)st);
                        acc_dom = dom;
                        orient_check = loops + 30;
                    }
                }
            }
        }

        /* SpringBoard's own device orientation (set from the accelerometer
           service, docs/mcu.md section 6.2): log every change */
        if ((loops % 10) == 0) {
            uint64_t st = 0;
            if (orient_tok < 0) notify_register_check("com.apple.springboard.rawOrientation", &orient_tok);
            if (orient_tok >= 0 && notify_get_state(orient_tok, &st) == 0 && (int)st != orient_last) {
                if (orient_last != -1 || st)
                    say("[mcud] SpringBoard device orientation %d -> %u", orient_last, (unsigned)st);
                orient_last = (int)st;
            }
        }
        if (orient_check && loops == orient_check && orient_tok >= 0) {
            uint64_t st = 0;
            notify_get_state(orient_tok, &st);
            say("[mcud] SpringBoard rawOrientation 3 s later: %u", (unsigned)st);
            orient_check = 0;
        }
        if (memcmp(&ctl, &ctl_sent, sizeof ctl)) {
            if (sysctl(cmib, (unsigned)cmiblen, 0, 0, &ctl, sizeof ctl) == 0) {
                if (ctl.acc != ctl_sent.acc) say("[mcud] accelerometer %s", ctl.acc ? "on" : "off");
                ctl_sent = ctl;
            }
        }
        if (opt_debug && box.seq != seen_seq && (loops % 50) == 0)
            say("[mcud] seq %u: battery %u%% flags %02x volume %u 3D %u irq %08x home %u power %u/%u shell %u/%u",
                (unsigned)box.seq, box.batt_percent, box.flags, box.vol_raw, box.slider3d, (unsigned)box.irq_seen,
                (unsigned)box.home_presses, (unsigned)box.power_presses, (unsigned)box.power_holds,
                (unsigned)box.shell_closes, (unsigned)box.shell_opens);
        seen_seq = box.seq;
        usleep(scr_off ? 250000 : 100000);     /* 4 Hz while the screen is off, 10 Hz on */
    }
}
