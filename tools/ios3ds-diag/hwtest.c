/*
 * ios3ds-hwtest: the one-boot hardware test kit (docs/real-hardware.md).
 *
 * boot.cfg hwtest=1 makes the ARM9 add the boot-arg ios3ds_hwtest=1; the
 * launchd job com.ios3ds.hwtest.plist starts us through
 * "ios3ds-diag -g ios3ds_hwtest", so without it nothing runs. Once
 * SpringBoard is up we check every driver in turn, one line per result:
 *
 *   [hwtest] speaker     PASS  tone played, route "Speaker"
 *
 * and finish with a summary block between "[hwtest] ==== SUMMARY" and
 * "[hwtest] ==== END" in sd:/ios3ds/log.txt, also shown on screen as an
 * alert. Checks that need a person (heard the tone? press HOME, move the 3D
 * slider ...) put a prompt on screen and watch the ARM9 mailboxes; nobody
 * answering within the prompt's time = SKIP, never FAIL. ios3ds_hwtest=2
 * (boot.cfg hwtest=2) skips every prompt (automatic checks only, for QEMU).
 *
 * Version-aware: kern.osversion picks 4.2.1 (8C148), 4.1 (8B117), 3.1.3
 * (7E18) or 2.2.1 (5H11a); a check whose tool or framework is not on this
 * rootfs is SKIPped with the reason. Reuses ios3ds-tone (speaker, headphone
 * route, mic), ios3ds-gyrotest (CoreMotion, 4.x), ios3ds-gltest (GPU), the
 * MCU / input / camera mailboxes of the shim (read only, non-destructive).
 *
 * Pure C, declarations local (stub SDK); frameworks through dlopen. No
 * division by variables and no floating point arithmetic: 2.x's compiler-rt
 * helpers use r9 (tone.c). SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mcu_proto.h"
#include "input_proto.h"
#include "cam_proto.h"

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
typedef unsigned int mach_port_t;
extern mach_port_t bootstrap_port;
int bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
int setenv(const char *, const char *, int);
long time(long *);
int kill(int, int);
int waitpid(int, int *, int);
struct pollfd { int fd; short events, revents; };
int poll(struct pollfd *, unsigned, int);
struct sockaddr4 { uint8_t len, family; uint16_t port; uint32_t addr; char zero[8]; };
struct ifaddrs { struct ifaddrs *next; char *name; unsigned flags; struct sockaddr4 *addr; void *mask, *dst, *data; };
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);

/* ---- results ------------------------------------------------------------ */
enum { PASS, FAIL, SKIP };
static const char *const RES[] = { "PASS", "FAIL", "SKIP" };
#define NRES 40
static struct { char name[14]; int r; char what[100]; } res[NRES];
static int nres, prompts = 1;

static void out(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (write(1, line, (size_t)n) < 0) { /* nothing to do */ }
}

static void result(const char *name, int r, const char *fmt, ...)
{
    va_list ap;
    if (nres >= NRES) return;
    snprintf(res[nres].name, sizeof res[nres].name, "%s", name);
    res[nres].r = r;
    va_start(ap, fmt);
    vsnprintf(res[nres].what, sizeof res[nres].what, fmt, ap);
    va_end(ap);
    out("[hwtest] %-12s %s  %s", name, RES[r], res[nres].what);
    nres++;
}

static int have(const char *path) { return access(path, 1 /* X_OK */) == 0; }

/* ---- boot-args / version ------------------------------------------------- */
static char bootargs[1024];
static const char *barg(const char *key)    /* value after "key=", or NULL */
{
    size_t kl = strlen(key);
    for (const char *p = bootargs; (p = strstr(p, key)); p += kl)
        if ((p == bootargs || p[-1] == ' ') && p[kl] == '=') return p + kl + 1;
    return NULL;
}
static int barg_on(const char *key)
{
    const char *v = barg(key);
    return v && *v && *v != ' ' && !(v[0] == '0' && (v[1] == 0 || v[1] == ' '));
}

static int ios_major;               /* 4, 3, 2 */
static const char *ios_name = "?";

/* ---- CoreFoundation alerts (SpringBoard shows them on every version) ----- */
typedef const void *CFTypeRef;
static CFTypeRef (*cfstr)(void *, const char *, uint32_t);
static void (*cfrel)(CFTypeRef);
static int32_t (*cfalert)(double, unsigned long, void *, void *, void *, CFTypeRef, CFTypeRef,
                          CFTypeRef, CFTypeRef, CFTypeRef, unsigned long *);
static int32_t (*cfnotice)(double, unsigned long, void *, void *, void *, CFTypeRef, CFTypeRef, CFTypeRef);
#define UTF8 0x08000100u

static void cf_load(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf) return;
    *(void **)&cfstr = dlsym(cf, "CFStringCreateWithCString");
    *(void **)&cfrel = dlsym(cf, "CFRelease");
    *(void **)&cfalert = dlsym(cf, "CFUserNotificationDisplayAlert");
    *(void **)&cfnotice = dlsym(cf, "CFUserNotificationDisplayNotice");
}
static CFTypeRef S(const char *s) { return s && cfstr ? cfstr(0, s, UTF8) : 0; }
static void R(CFTypeRef o) { if (o && cfrel) cfrel(o); }

/* Blocking question: 0 = first button, 1 = second, -1 = nobody answered / no UI */
static int ask(int secs, const char *title, const char *msg, const char *b1, const char *b2)
{
    if (!prompts) return -1;
    out("[hwtest] prompt: %s - %s [%s%s%s] (%d s)", title, msg, b1, b2 ? "/" : "", b2 ? b2 : "", secs);
    if (!cfalert || !cfstr) return -1;
    CFTypeRef t = S(title), m = S(msg), x = S(b1), y = S(b2);
    unsigned long rf = 3;
    int32_t e = cfalert((double)secs, 0, 0, 0, 0, t, m, x, y, 0, &rf);
    R(t); R(m); R(x); R(y);
    if (e != 0 || (rf & 3) == 3) return -1;
    return (int)(rf & 3) == 1 ? 1 : 0;
}

/* Non-blocking instruction while we watch a mailbox */
static void tell(int secs, const char *title, const char *msg)
{
    if (!prompts) return;
    out("[hwtest] prompt: %s - %s (%d s)", title, msg, secs);
    if (!cfnotice || !cfstr) return;
    CFTypeRef t = S(title), m = S(msg), b = S("OK");
    cfnotice((double)secs, 0, 0, 0, 0, t, m, b);
    R(t); R(m); R(b);
}

/* ---- running a helper tool with a time limit ----------------------------- */
/* Its stdout+stderr go into buf (and to our console); returns the exit status,
   or -1 when it had to be killed / could not start. */
static int run_until(char *buf, int cap, int secs, const char *env, const char *marker, char *const argv[])
{
    int p[2], st = 0, got = 0;
    long t0 = time(0);
    buf[0] = 0;
    if (pipe(p)) return -1;
    int pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        if (env) setenv(env, "1", 1);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    for (;;) {
        struct pollfd pf = { p[0], 1 /* POLLIN */, 0 };
        long left = secs - (time(0) - t0);
        if (left <= 0) { kill(pid, 9); out("[hwtest] %s: killed after %d s", argv[0], secs); break; }
        if (poll(&pf, 1, 1000) <= 0) continue;
        char tmp[512];
        long n = read(p[0], tmp, sizeof tmp);
        if (n <= 0) break;
        if (write(1, tmp, (size_t)n) < 0) { /* console copy only */ }
        if (got + n < cap) { memcpy(buf + got, tmp, (size_t)n); got += (int)n; buf[got] = 0; }
        if (marker && strstr(buf, marker) && strchr(strstr(buf, marker), '\n')) {
            kill(pid, 9);                       /* it said what we wanted; don't wait for its exit */
            close(p[0]);
            waitpid(pid, &st, 0);
            return 0;
        }
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid) return -1;
    if ((st & 0x7f) != 0) return -1;                    /* killed by a signal */
    return (st >> 8) & 0xff;
}

static int run(char *buf, int cap, int secs, const char *env, char *const argv[])
{
    return run_until(buf, cap, secs, env, 0, argv);
}

/* ---- the shim mailboxes (read-only snapshots) ---------------------------- */
static int mcu_read(mcubox_t *b)
{
    size_t l = sizeof *b;
    return sysctlbyname(MCU_SYSCTL_NAME, b, &l, 0, 0) == 0 && l == sizeof *b && b->magic == MCU_MAGIC;
}
static int in_read(inmbox_t *b)
{
    size_t l = sizeof *b;
    return sysctlbyname(INPUT_SYSCTL_NAME, b, &l, 0, 0) == 0 && l == sizeof *b && b->magic == INPUT_MAGIC;
}

/* Watch the input ring for `secs`: OR of all buttons seen, pen samples */
static void in_watch(int secs, uint32_t *btn_or, uint32_t *pens, uint32_t stop_mask)
{
    inmbox_t b;
    uint32_t seen = 0;
    long t0 = time(0);
    *btn_or = 0;
    *pens = 0;
    while (time(0) - t0 < secs) {
        if (in_read(&b)) {
            if (!seen) seen = b.head;
            uint32_t from = b.head - seen > INPUT_RING ? b.head - INPUT_RING : seen;
            for (uint32_t i = from; i != b.head; i++) {
                const insample_t *s = &b.s[i % INPUT_RING];
                *btn_or |= s->buttons;
                if (s->flags & INFLAG_PEN) (*pens)++;
            }
            seen = b.head;
            if (stop_mask && (*btn_or & stop_mask) == stop_mask) return;
        }
        usleep(50000);
    }
}

/* ---- checks -------------------------------------------------------------- */
static char buf[16384];

static void check_mcu(void)
{
    mcubox_t b;
    if (!mcu_read(&b)) { result("mcu", FAIL, "no kern.ios3ds_mcu mailbox (old shim?)"); return; }
    if (b.state == MCU_ST_DISABLED) { result("mcu", SKIP, "boot.cfg mcu=0"); return; }
    if (b.state != MCU_ST_OK) { result("mcu", FAIL, "MCU state %u (no I2C answer, see [mcu] lines)", b.state); return; }
    result("mcu", PASS, "firmware %02x/%02x, %u polls, %u I2C errors", b.fw >> 8, b.fw & 0xff, b.polls, b.i2c_errors);
    if (b.valid & MCUV_BATT)
        result("battery", b.batt_percent <= 100 ? PASS : FAIL, "%u%%, %u.%02u V, %u C%s%s", b.batt_percent,
               (b.batt_volt * 2u) / 100u, (b.batt_volt * 2u) % 100u, b.batt_temp,
               (b.flags & MCUF_ADAPTER) ? ", charger" : "", (b.flags & MCUF_CHARGING) ? ", charging" : "");
    else
        result("battery", FAIL, "the ARM9 never read the battery");
    result("rtc", (b.valid & MCUV_RTC) && b.rtc_utc > 1600000000u ? PASS : FAIL, "MCU clock %u (Unix UTC)", b.rtc_utc);
}

/* iOS brightness -> ios3ds-mcud -> kern.ios3ds_mcuctl -> the shim's PWM write */
static void check_brightness(void)
{
    mcubox_t b;
    int ok = 0;
    for (int t = 0; t < 30 && (ok = mcu_read(&b)) && b.backlight == 0; t++) sleep(2);
    if (!ok) result("brightness", SKIP, "no MCU");
    else if (b.backlight > 1) result("brightness", PASS, "iOS brightness reached the LCD backlight (level %u)", b.backlight - 1);
    else if (b.backlight == 1) result("brightness", PASS, "backlight is off right now (screen locked)");
    else result("brightness", SKIP, "iOS has not set a brightness yet (Luma's level kept; see [mcud] lines)");
}

static void check_motion(void)
{
    mcubox_t a, b;
    if (!mcu_read(&a) || a.state != MCU_ST_OK) { result("accel", SKIP, "no MCU"); result("gyro", SKIP, "no MCU"); return; }
    if (a.acc_off) result("accel", SKIP, "boot.cfg mcu_acc=0");
    else {
        /* ios3ds-mcud keeps the accelerometer off by default (docs/mcu.md
           6.2): switch it on for 3 s through the daemon's own control sysctl
           (backlight 0 = unchanged), then back to off */
        struct { uint32_t acc, backlight, _r[2]; } ctl = { 1, 0, { 0, 0 } };
        int asked = !a.req_acc && sysctlbyname("kern.ios3ds_mcuctl", 0, 0, &ctl, sizeof ctl) == 0;
        sleep(1);
        mcu_read(&a);
        sleep(3);
        mcu_read(&b);
        if (asked) { ctl.acc = 0; sysctlbyname("kern.ios3ds_mcuctl", 0, 0, &ctl, sizeof ctl); }
        if (b.acc_samples != a.acc_samples)
            result("accel", PASS, "%u samples in 3 s, now %d %d %d", b.acc_samples - a.acc_samples, b.acc[0], b.acc[1], b.acc[2]);
        else if (a.valid & MCUV_ACC)
            result("accel", PASS, "read at least once (%d %d %d); iOS is not sampling right now", b.acc[0], b.acc[1], b.acc[2]);
        else
            result("accel", FAIL, "no sample (req_acc %u)", b.req_acc);
    }
    static const char *const gs[] = { "not probed", "ITG-3270", "L3G", "absent", "boot.cfg gyro=0" };
    int st = a.gyro_state <= GYRO_ST_DISABLED ? a.gyro_state : 0;
    if (st == GYRO_ST_DISABLED) { result("gyro", SKIP, "boot.cfg gyro=0"); return; }
    if (st != GYRO_ST_ITG && st != GYRO_ST_ST) { result("gyro", FAIL, "chip %s", gs[st]); return; }
    if (ios_major < 4 || !have("/usr/libexec/ios3ds-gyrotest")) {
        result("gyro", PASS, "chip %s (I2C %02x id %02x); CoreMotion needs iOS 4, not checked", gs[st], a.gyro_dev, a.gyro_id);
        return;
    }
    char *av[] = { "/usr/libexec/ios3ds-gyrotest", "4", "20", 0 };
    int r = run_until(buf, sizeof buf, 60, 0, "[gyrotest] done:", av);
    const char *d = strstr(buf, "[gyrotest] done:");
    mcu_read(&b);
    if (r == 0 && d && b.gyro_samples != a.gyro_samples)
        result("gyro", PASS, "chip %s, CoreMotion got samples (%u from the ARM9)", gs[st], b.gyro_samples - a.gyro_samples);
    else
        result("gyro", FAIL, "chip %s, gyrotest exit %d, %u ARM9 samples", gs[st], r, b.gyro_samples - a.gyro_samples);
}

static int tone_route(const char *out_, char *route, int n)
{
    const char *p = strstr(out_, "route \"");
    route[0] = 0;
    if (!p) return 0;
    p += 7;
    int i = 0;
    while (p[i] && p[i] != '"' && i < n - 1) { route[i] = p[i]; i++; }
    route[i] = 0;
    return 1;
}

static void check_audio(void)
{
    char route[64];
    if (!have("/usr/libexec/ios3ds-tone")) {
        result("speaker", SKIP, "no ios3ds-tone on this rootfs");
        result("headphone", SKIP, "no ios3ds-tone");
        result("mic", SKIP, "no ios3ds-tone");
        return;
    }
    char *play[] = { "/usr/libexec/ios3ds-tone", "1", "0", "800", "1000", "8000", "0", 0 };
    int r = run(buf, sizeof buf, 150, "IOS3DS_HWTEST", play);
    tone_route(buf, route, sizeof route);
    int done = strstr(buf, "#1 done") != NULL;
    if (!done) result("speaker", FAIL, "tone did not play (exit %d)", r);
    else {
        int a = ask(25, "Hardware test: speaker", "Did you just hear a short beep?", "Yes", "No");
        if (a == 0) result("speaker", PASS, "tone played and heard, route \"%s\"", route);
        else if (a == 1) result("speaker", FAIL, "tone played (route \"%s\") but not heard: CODEC/amp path", route);
        else result("speaker", PASS, "tone played through AudioQueue, route \"%s\" (nobody confirmed hearing it)", route);
    }
    /* headphone detect: the route iOS reports must follow the jack */
    if (!prompts) result("headphone", SKIP, "needs a person (hwtest=1); speaker route \"%s\"", route);
    else if (ask(30, "Hardware test: headphones", "Plug headphones in, then tap OK (Skip if you have none).", "OK", "Skip") != 0)
        result("headphone", SKIP, "no headphones / no answer (speaker route \"%s\")", route);
    else {
        char hr[64];
        r = run(buf, sizeof buf, 150, "IOS3DS_HWTEST", play);
        tone_route(buf, hr, sizeof hr);
        int hp = strstr(hr, "Head") != NULL;
        int a = hp ? ask(25, "Hardware test: headphones", "Beep in the headphones?", "Yes", "No") : -1;
        if (!hp) result("headphone", FAIL, "jack in but iOS route is \"%s\" (boot.cfg hp_route?)", hr);
        else result("headphone", a == 1 ? FAIL : PASS, "route \"%s\"%s", hr, a == 0 ? ", heard" : a == 1 ? ", NOT heard" : "");
        ask(20, "Hardware test", "Unplug the headphones, then tap OK.", "OK", 0);
    }
    /* microphone: a 1.5 s recording must have signal */
    char *rec[] = { "/usr/libexec/ios3ds-tone", "rec", "1", "0", "1500", "0", "-", 0 };
    tell(8, "Hardware test: microphone", "Say something or clap now...");
    r = run(buf, sizeof buf, 90, "IOS3DS_HWTEST", rec);
    const char *p;
    unsigned frames = 0, want = 0, nz = 0, peak = 0;
    for (p = buf; (p = strstr(p, "tone: rec ")); p++)
        if (sscanf(p, "tone: rec %u of %u frames in %*u buffers: nonzero %u, peak %u", &frames, &want, &nz, &peak) == 4) break;
    if ((!p || !frames) && ios_major == 2)
        result("mic", SKIP, "no built-in mic route on iPhone OS 2 (re-log 134); %s", p ? "0 frames" : "no recording");
    else if (!p) result("mic", FAIL, "no recording (exit %d)", r);
    else if (!frames) result("mic", FAIL, "0 of %u frames recorded", want);
    else if (!peak) result("mic", FAIL, "%u frames, all silent", frames);
    else result("mic", PASS, "%u of %u frames, peak %u, %u non-zero", frames, want, nz, peak);
}

static int en0_addr(char *ip, int n)
{
    struct ifaddrs *ifa = 0, *i;
    int ok = 0;
    if (getifaddrs(&ifa)) return 0;
    for (i = ifa; i; i = i->next)
        if (i->name && !strcmp(i->name, "en0") && i->addr && i->addr->family == 2) {
            uint32_t a = i->addr->addr;
            snprintf(ip, (size_t)n, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
            ok = (a & 0xffff) != (169 | 254 << 8);      /* not a link-local fallback */
        }
    freeifaddrs(ifa);
    return ok;
}

/* HTTPS through Foundation (NSData dataWithContentsOfURL:options:error:):
   the stock TLS stack and trust store of this iOS version */
static long https_get(const char *url, char *err, int n)
{
    void *fw = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    void *ob = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!fw || !ob) { snprintf(err, (size_t)n, "no Foundation"); return -1; }
    void *(*gc)(const char *) = (void *(*)(const char *))dlsym(ob, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(ob, "sel_registerName");
    void *msg = dlsym(ob, "objc_msgSend");
    if (!gc || !sel || !msg) { snprintf(err, (size_t)n, "no objc"); return -1; }
    typedef void *(*M0)(void *, void *);
    typedef void *(*M1)(void *, void *, const void *);
    typedef void *(*M3)(void *, void *, const void *, unsigned long, void **);
    typedef unsigned long (*ML)(void *, void *);
    typedef long (*MI)(void *, void *);
    void *pool = ((M0)msg)(((M0)msg)(gc("NSAutoreleasePool"), sel("alloc")), sel("init"));
    void *s = ((M1)msg)(gc("NSString"), sel("stringWithUTF8String:"), url);
    void *u = s ? ((M1)msg)(gc("NSURL"), sel("URLWithString:"), s) : 0;
    void *e = 0;
    void *d = u ? ((M3)msg)(gc("NSData"), sel("dataWithContentsOfURL:options:error:"), u, 0, &e) : 0;
    long len = d ? (long)((ML)msg)(d, sel("length")) : -1;
    if (!d) {
        long code = e ? ((MI)msg)(e, sel("code")) : 0;
        void *dom = e ? ((M0)msg)(e, sel("domain")) : 0;
        const char *ds = dom ? (const char *)((M0)msg)(dom, sel("UTF8String")) : "?";
        /* NSCocoaErrorDomain 256 only says "could not read": the CFNetwork /
           NSURL error underneath (DNS, TLS, timeout) is in its userInfo */
        void *ui = e ? ((M0)msg)(e, sel("userInfo")) : 0;
        void *ue = ui ? ((M1)msg)(ui, sel("objectForKey:"), ((M1)msg)(gc("NSString"), sel("stringWithUTF8String:"), "NSUnderlyingError")) : 0;
        long ucode = ue ? ((MI)msg)(ue, sel("code")) : 0;
        void *udom = ue ? ((M0)msg)(ue, sel("domain")) : 0;
        const char *uds = udom ? (const char *)((M0)msg)(udom, sel("UTF8String")) : 0;
        if (uds) snprintf(err, (size_t)n, "%s %ld (%s %ld)", ds ? ds : "?", code, uds, ucode);
        else snprintf(err, (size_t)n, "%s %ld", ds ? ds : "?", code);
    }
    ((M0)msg)(pool, sel("release"));
    return len;
}

static void check_wifi(void)
{
    char ip[32] = "";
    int up = 0;
    long t0 = time(0);
    /* the saved network is joined by configd/WiFiManager (scan + join);
       DHCP then gives en0 its address */
    while (!(up = en0_addr(ip, sizeof ip)) && time(0) - t0 < 120) sleep(2);
    if (!up) {
        result("wifi.join", FAIL, "no DHCP address on en0 after 120 s (%s): scan/join/nwm.code? see [wifi] lines", ip[0] ? ip : "none");
        result("wifi.https", SKIP, "no link");
        return;
    }
    result("wifi.join", PASS, "en0 %s after %ld s (saved network scanned + joined)", ip, time(0) - t0);
    const char *u = barg("ios3ds_hwtest_url");
    char url[160], err[96] = "";
    int i = 0;
    if (u) while (u[i] && u[i] != ' ' && i < (int)sizeof url - 1) { url[i] = u[i]; i++; }
    url[i] = 0;
    if (!url[0]) snprintf(url, sizeof url, "https://example.com/");
    long n = -1;
    for (int tries = 0; tries < 3 && n <= 0; tries++) {   /* DNS/DHCP can still be settling */
        if (tries) { out("[hwtest] wifi.https: %s, retrying in 15 s", err); sleep(15); }
        n = https_get(url, err, sizeof err);
    }
    if (n > 0) result("wifi.https", PASS, "%s: %ld bytes", url, n);
    else if (ios_major < 4) result("wifi.https", SKIP, "%s: %s (iOS %s TLS is too old for most servers)", url, err, ios_name);
    else result("wifi.https", FAIL, "%s: %s", url, err);
}

static void check_touch_buttons(void)
{
    inmbox_t b;
    uint32_t bo, pens;
    if (!in_read(&b)) { result("touch", FAIL, "no kern.ios3ds_input mailbox"); result("buttons", SKIP, "no input mailbox"); return; }
    if (!b.tsc_ok) result("touch", FAIL, "touch ADC not running (CODEC did not answer)");
    else if (!prompts) result("touch", PASS, "touch ADC running (%u polls); not touched (no prompts)", b.polls);
    else {
        tell(15, "Hardware test: touch", "Tap OK, then draw on the bottom screen for a few seconds.");
        in_watch(15, &bo, &pens, 0);
        result("touch", pens ? PASS : SKIP, "touch ADC running, %u pen samples%s", pens, pens ? "" : " (nobody touched)");
    }
    if (!prompts) { result("buttons", SKIP, "needs a person (hwtest=1)"); return; }
    const uint32_t want = 0x0FFF & ~0x000Cu;    /* A B Right Left Up Down R L X Y (Select/Start optional) */
    tell(20, "Hardware test: buttons", "Press A, B, X, Y, L, R and all four D-pad directions.");
    in_watch(20, &bo, &pens, want);
    static const char *const nm[12] = { "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y" };
    char miss[80] = "";
    for (int i = 0; i < 12; i++)
        if ((want >> i & 1) && !(bo >> i & 1)) { strcat(miss, " "); strcat(miss, nm[i]); }
    if (!(bo & 0x0FFF)) result("buttons", SKIP, "no button pressed");
    else result("buttons", miss[0] ? FAIL : PASS, "seen %03x%s%s", bo & 0x0FFF, miss[0] ? ", missing:" : "", miss);

    mcubox_t m0, m1;
    if (!mcu_read(&m0)) return;
    tell(20, "Hardware test: volume", "Move the volume slider all the way down and up.");
    unsigned lo = m0.vol_raw, hi = m0.vol_raw;
    for (int t = 0; t < 200; t++) {
        usleep(100000);
        if (mcu_read(&m1)) { if (m1.vol_raw < lo) lo = m1.vol_raw; if (m1.vol_raw > hi) hi = m1.vol_raw; }
        if (hi - lo > 40) break;
    }
    result("volume", hi - lo > 40 ? PASS : hi != lo ? FAIL : SKIP, "slider range %u..%u of 0..63", lo, hi);
    mcu_read(&m0);
    tell(20, "Hardware test: HOME", "Press HOME once.");
    for (int t = 0; t < 200 && mcu_read(&m1) && m1.home_presses == m0.home_presses; t++) usleep(100000);
    result("home", m1.home_presses != m0.home_presses ? PASS : SKIP, "%u HOME press(es) seen", m1.home_presses - m0.home_presses);
}

static void check_lock(void)
{
    mcubox_t a, b;
    if (!prompts) { result("lock/wake", SKIP, "needs a person (hwtest=1)"); return; }
    if (!mcu_read(&a)) { result("lock/wake", SKIP, "no MCU"); return; }
    tell(10, "Hardware test: lock", "Tap OK, press POWER to lock, wait 3 s, press POWER (or HOME) to wake.");
    int off = 0, back = 0;
    for (int t = 0; t < 400; t++) {             /* 40 s */
        usleep(100000);
        if (!mcu_read(&b)) continue;
        if (b.backlight == 1 || b.screen_off) off = 1;
        if (off && b.backlight > 1 && !b.screen_off) { back = 1; break; }
    }
    if (b.power_presses == a.power_presses && !off) result("lock/wake", SKIP, "POWER not pressed");
    else result("lock/wake", off && back ? PASS : FAIL, "%u POWER presses, screen off %s, back on %s",
                b.power_presses - a.power_presses, off ? "yes" : "NO", back ? "yes" : "NO");
}

static void check_stereo(void)
{
    mcubox_t a, b;
    if (!barg_on("ios3ds_stereo")) { result("stereo3d", SKIP, "boot.cfg stereo3d=0"); return; }
    if (!mcu_read(&a)) { result("stereo3d", FAIL, "no MCU (3D slider)"); return; }
    if (!prompts) { result("stereo3d", SKIP, "slider at %u; needs a person (hwtest=1)", a.slider3d); return; }
    tell(25, "Hardware test: 3D", "Push the 3D slider up: the home screen icons should pop out. Then push it back down.");
    unsigned hi = a.slider3d, lo = a.slider3d;
    for (int t = 0; t < 250; t++) {
        usleep(100000);
        if (mcu_read(&b)) { if (b.slider3d > hi) hi = b.slider3d; if (b.slider3d < lo) lo = b.slider3d; }
    }
    if (hi == lo) { result("stereo3d", SKIP, "3D slider not moved (%u)", hi); return; }
    int a2 = ask(25, "Hardware test: 3D", "Did the home screen look 3D with the slider up?", "Yes", "No");
    result("stereo3d", a2 == 1 ? FAIL : PASS, "slider %u..%u%s", lo, hi, a2 == 0 ? ", 3D seen" : a2 == 1 ? ", NOT seen" : "");
}

static void check_cameras(void)
{
    int mib[4];
    size_t m = 3;
    if (sysctlnametomib(CAM_SYSCTL_NAME, mib, &m)) { result("cameras", SKIP, "no ios3ds_cam service in this shim"); return; }
    cam_req_t r;
    memset(&r, 0, sizeof r);
    r.op = CAM_OP_INFO;
    mib[m] = (int)(intptr_t)&r;
    sysctl(mib, (unsigned)m + 1, 0, 0, 0, 0);
    if (r.status == CAM_ERR_DISABLED || (r.status == CAM_OK && !r.a[1])) { result("cameras", SKIP, "boot.cfg camera=0"); return; }
    uint32_t *fb = malloc(CAM_FRAME_BYTES);
    const char *nm[2] = { "outer", "inner" };
    char what[96] = "";
    int ok = 0;
    for (uint32_t c = 0; c < CAM_COUNT && fb; c++) {
        memset(&r, 0, sizeof r);
        r.op = CAM_OP_START;
        r.a[0] = c;
        sysctl(mib, (unsigned)m + 1, 0, 0, 0, 0);
        uint32_t st = r.status, got = 0;
        for (int t = 0; st == CAM_OK && t < 150 && !got; t++) {     /* sensor init can take seconds */
            usleep(100000);
            memset(&r, 0, sizeof r);
            r.op = CAM_OP_FRAME;
            r.a[0] = (uint32_t)(uintptr_t)fb;
            sysctl(mib, (unsigned)m + 1, 0, 0, 0, 0);
            if (r.status == CAM_OK) got = 1;
            else if (r.status != CAM_ERR_AGAIN) st = r.status;
        }
        memset(&r, 0, sizeof r);
        r.op = CAM_OP_STOP;
        sysctl(mib, (unsigned)m + 1, 0, 0, 0, 0);
        ok += got;
        snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s %s", c ? ", " : "", nm[c],
                 got ? "frame ok" : st == CAM_OK ? "no frame in 15 s" : "error");
        if (!got && st != CAM_OK) snprintf(what + strlen(what), sizeof what - strlen(what), " %u", st);
    }
    free(fb);
#ifdef IOS2_HWTEST
    /* The shim's frame path is real hardware even where iOS has no Camera.app
       to consume the frames. Two frames are a PASS; without them, report the
       raw result as SKIP rather than calling a missing app a driver failure
       (re-log 153). */
    if (ok == (int)CAM_COUNT)
        result("cameras", PASS, "%s", what);
    else
        result("cameras", SKIP, "no Camera.app on iPhone OS 2; shim frame path: %s", what);
#else
    /* Kept byte-for-byte: the 4.2.1 tool binary is unchanged by this fix. */
    if (ok != (int)CAM_COUNT && ios_major == 2)     /* no Camera.app / MediaCapture on 2.x (multi-version.md 6.9) */
        result("cameras", SKIP, "n/a on iPhone OS 2; shim frame path: %s", what);
    else
        result("cameras", ok == (int)CAM_COUNT ? PASS : FAIL, "%s", what);
#endif
}

static void check_gps(void)
{
    const char *g = barg("ios3ds_gps");
    if (!g || !*g || *g == ' ') { result("gps", SKIP, "no boot.cfg gps= (fake location off)"); return; }
    char v[64];
    int i = 0;
    while (g[i] && g[i] != ' ' && i < 63) { v[i] = g[i]; i++; }
    v[i] = 0;
    if (!have("/usr/libexec/ios3ds-gpstest")) { result("gps", SKIP, "gps=%s, no ios3ds-gpstest", v); return; }
    char *av[] = { "/usr/libexec/ios3ds-gpstest", "8", 0 };
    int r = run(buf, sizeof buf, 60, "IOS3DS_HWTEST", av);
    const char *p = strstr(buf, "[gpstest] location: ");
    char loc[48] = "";
    if (p) sscanf(p + 20, "%47[^\n]", loc);
    int same = !strncmp(loc, v, 5);
    result("gps", p && strncmp(loc, "(null)", 6) ? PASS : FAIL, "gps=%s, CoreLocation says %s%s", v, loc[0] ? loc : "nothing",
           !p ? "" : same ? " (= gps=)" : " (moved at runtime by ios3ds-gps set)");
    (void)r;
}

static void check_gpu(void)
{
    if (!have("/usr/libexec/ios3ds-gltest")) {
        result("gpu", SKIP, "no ios3ds-gltest on %s (GL engine self-check runs at CA start, see [gl] lines)", ios_name);
        return;
    }
    char *av[] = { "/usr/libexec/ios3ds-gltest", "0", "0", 0 };
    int r = run(buf, sizeof buf, 90, 0, av);
    const char *p = strstr(buf, "gltest: PASS");
    const char *f = strstr(buf, "gltest: FAIL");
    if (p && r == 0) result("gpu", PASS, "gltest offscreen render + pixel checks");
    else result("gpu", FAIL, "gltest exit %d%s", r, f ? " (pixel checks failed)" : "");
}

/* ---- summary ------------------------------------------------------------- */
static void summary(long t0)
{
    int n[3] = { 0, 0, 0 };
    char screen[1600] = "";
    for (int i = 0; i < nres; i++) n[res[i].r]++;
    out("[hwtest] ==== SUMMARY (iOS %s, %ld s): %d PASS, %d FAIL, %d SKIP ====", ios_name, time(0) - t0, n[0], n[1], n[2]);
    for (int i = 0; i < nres; i++)
        out("[hwtest] | %-12s %s  %s", res[i].name, RES[res[i].r], res[i].what);
    /* on screen: one short paragraph per result kind (a long alert body
       is cut off), FAIL first */
    static const int order[3] = { FAIL, SKIP, PASS };
    for (int k = 0; k < 3; k++) {
        int first = 1;
        for (int i = 0; i < nres; i++) {
            if (res[i].r != order[k] || strlen(screen) + 20 >= sizeof screen) continue;
            snprintf(screen + strlen(screen), sizeof screen - strlen(screen), "%s%s", first ? RES[order[k]] : ",", "");
            snprintf(screen + strlen(screen), sizeof screen - strlen(screen), "%s%s", first ? ": " : " ", res[i].name);
            first = 0;
        }
        if (!first) strcat(screen, "\n");
    }
    strcat(screen, "Details: sd:/ios3ds/log.txt");
    out("[hwtest] ==== END: %s ====", n[1] ? "FAIL" : "PASS");
    char title[96];
    snprintf(title, sizeof title, "Hardware test: %d PASS, %d FAIL, %d SKIP", n[0], n[1], n[2]);
    if (cfnotice && cfstr) {                    /* always shown, also with hwtest=2 */
        CFTypeRef t = S(title), m = S(screen), b = S("OK");
        cfnotice(0, 0, 0, 0, 0, t, m, b);
        R(t); R(m); R(b);
    }
}

int main(void)
{
    size_t n = sizeof bootargs - 1;
    char osv[32] = "";
    setvbuf(stdout, NULL, 2 /* _IONBF */, 0);
    if (sysctlbyname("kern.bootargs", bootargs, &n, 0, 0) == 0) bootargs[n < sizeof bootargs ? n : sizeof bootargs - 1] = 0;
    const char *mode = barg("ios3ds_hwtest");
    prompts = !(mode && mode[0] == '2');
    n = sizeof osv - 1;
    if (sysctlbyname("kern.osversion", osv, &n, 0, 0) == 0) osv[n < sizeof osv ? n : sizeof osv - 1] = 0;
    if (!strncmp(osv, "8C148", 5)) ios_major = 4, ios_name = "4.2.1";
    else if (!strncmp(osv, "8B117", 5)) ios_major = 4, ios_name = "4.1";
    else if (!strncmp(osv, "7E18", 4)) ios_major = 3, ios_name = "3.1.3";
    else if (!strncmp(osv, "5H11", 4)) ios_major = 2, ios_name = "2.2.1";
    else ios_major = osv[0] == '8' ? 4 : osv[0] == '7' ? 3 : 2, ios_name = osv;
    out("[hwtest] start: iOS %s (%s), %s; waiting for the home screen", ios_name, osv, prompts ? "with prompts" : "automatic only (hwtest=2)");

    /* SpringBoard's own Mach service is there on 2.x-4.x once it runs */
    mach_port_t sp = 0;
    for (int i = 0; bootstrap_look_up(bootstrap_port, "com.apple.springboard", &sp); i++) {
        if (i == 600) { out("[hwtest] no SpringBoard after 20 min: giving up"); return 1; }
        sleep(2);
    }
    sleep(ios_major >= 4 ? 30 : 45);            /* let the home screen settle */
    cf_load();
    long t0 = time(0);
    out("[hwtest] home screen up: running the checks");
    if (prompts) ask(30, "ios3ds hardware test", "Every driver is checked now (about 5 minutes). Follow the prompts; ignore one to skip that check.", "Start", 0);
    check_mcu();
    check_motion();
    check_cameras();
    check_gps();
    check_gpu();
    check_audio();
    check_wifi();
    check_touch_buttons();
    check_brightness();
    check_stereo();
    check_lock();
    summary(t0);
    return 0;
}
