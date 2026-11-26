/*
 * ios3ds-applog.dylib: NSLog/stderr of the apps SpringBoard launches -> /dev/console.
 *
 * SpringBoard gives an app a stderr only for a debug launch. With the
 * SpringBoard default SBGuardMallocAlways it puts DYLD_INSERT_LIBRARIES=
 * /usr/lib/libgmalloc.dylib into every app's environment
 * (-[SBApplication _addInternalDebugVariablesToEnvironment], re-log 62); the
 * device has no libgmalloc, so the diag rootfs links that name to this library.
 * Active only when the kernel boot-args contain ios3ds_diag=1 (boot.cfg diag=1)
 * or ios3ds_applog=1 (added to boot.cfg's cmdline, without diag's test jobs):
 * then it sets CFLOG_FORCE_STDERR=1 (CFLog/NSLog also write to stderr) and
 * points stdout and stderr at /dev/console, and logs every alert's title and
 * text as "[applog] <program> alert ..." (UIModalView/UIAlertView setters and
 * popup), and MobileSafari's page-load errors with domain, code and userInfo.
 * It also interposes mmap/vm_allocate/mach_vm_allocate and logs failed calls
 * >= 64 KiB as "[applog] <program> ENOMEM ..." (up to 16 per process).
 * Otherwise only the load errors (MobileSafari, re-log 75), failed big
 * allocations, SpringBoard's modal state and SpringBoard's cache warm-up
 * (below) are on.
 *
 * Cydia Substrate (docs/cydia.md): every executable inside a .app bundle
 * (SpringBoard, which inserts us through its launchd plist, and every app, which
 * gets us as SpringBoard's libgmalloc.dylib) loads
 * /Library/MobileSubstrate/MobileSubstrate.dylib from here, before main(), as if
 * it had been inserted with it; without Substrate on the image nothing happens.
 * No launchd plist names Substrate: mobilesubstrate's extrainst_ strips it there.
 * Sandboxed apps (MobileSafari, App Store) cannot read /Library; they load the
 * /usr/lib copies instead (docs/cydia.md, docs/certificates.md).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gps.h"

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern const char *getprogname(void);
int unsetenv(const char *name);
int _NSGetExecutablePath(char *buf, unsigned int *size);

/* libobjc through dlsym: the stub SDK lists only the symbols the tools import */
typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef id (*IMP)(id, SEL, ...);
static id (*p_getClass)(const char *);
static Method (*p_getInstanceMethod)(id, SEL);
static IMP (*p_setImplementation)(Method, IMP);
static SEL (*p_sel)(const char *);
static id (*p_msgSend)(id, SEL, ...);

static void applog_write(int fd, const char *line, int l)
{
    if (fd >= 0 && write(fd, line, l) == l) return;
    /* A sandboxed app (MobileSafari) is denied /dev/console: keep the line in
       the one directory it may write, so a QEMU/hardware log still has it. */
    int f = open("/private/var/tmp/ios3ds-applog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f < 0) return;
    write(f, line, (size_t)l);
    close(f);
}

/* ENOMEM from the three allocation entry points WebKit/JSC can use. The
   hardware "Cannot allocate memory" alert only gives NSPOSIXErrorDomain 12;
   these lines name the call, the requested size and the caller's PC, which is
   enough to tell a JSC heap reservation from a file/image mapping. No output
   on success; at most 16 lines per process, so a failing loop cannot flood
   the console. Only active when this dylib is inserted (diag/applog). */
static int nomem_lines;
static void log_nomem(const char *what, unsigned long long size, unsigned arg, unsigned pc)
{
    if (nomem_lines++ >= 16)
        return;
    char line[256];
    int l = snprintf(line, sizeof(line),
                     "[applog] %s ENOMEM %s size %llu arg %u caller 0x%08x\n",
                     getprogname(), what, size, arg, pc);
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;
    if (l > 0) applog_write(2, line, l);
}

/* Declared here, not in a SDK header: the stub SDK has no sys/mman.h or Mach
   VM declarations and these symbols live in libsystem. */
extern void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
extern int vm_allocate(unsigned task, unsigned *addr, unsigned size, int anywhere);
extern int mach_vm_allocate(unsigned task, unsigned long long *addr, unsigned long long size, int anywhere);

static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    void *r = mmap(addr, len, prot, flags, fd, off);
    if (r == (void *)-1 && errno == ENOMEM && len >= 65536)
        log_nomem("mmap", len, (unsigned)flags, (unsigned)__builtin_return_address(0));
    return r;
}

static int my_vm_allocate(unsigned task, unsigned *addr, unsigned size, int anywhere)
{
    int r = vm_allocate(task, addr, size, anywhere);
    if (r && size >= 65536)
        log_nomem("vm_allocate", size, (unsigned)r, (unsigned)__builtin_return_address(0));
    return r;
}

static int my_mach_vm_allocate(unsigned task, unsigned long long *addr, unsigned long long size, int anywhere)
{
    int r = mach_vm_allocate(task, addr, size, anywhere);
    if (r && size >= 65536)
        log_nomem("mach_vm_allocate", size, (unsigned)r, (unsigned)__builtin_return_address(0));
    return r;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *rep, *orig; } nomem_interpose[] = {
    { (const void *)my_mmap, (const void *)mmap },
    { (const void *)my_vm_allocate, (const void *)vm_allocate },
    { (const void *)my_mach_vm_allocate, (const void *)mach_vm_allocate },
};

static void say(const char *what, id str)
{
    const char *c = "";
    if (str) c = (const char *)p_msgSend(str, p_sel("UTF8String"));
    char line[512];
    int l = snprintf(line, sizeof(line), "[applog] %s alert %s \"%s\"\n", getprogname(), what, c ? c : "");
    if (l > (int)sizeof(line) - 1) l = sizeof(line) - 1;
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) applog_write(2, line, l);
}

/* one trampoline per hooked method (a subclass may call its superclass' one) */
#define HOOK(n)                                                                  \
    static IMP orig##n;                                                          \
    static const char *what##n;                                                  \
    static id hook##n(id self, SEL cmd, id arg)                                  \
    {                                                                            \
        say(what##n, arg);                                                       \
        return orig##n(self, cmd, arg);                                          \
    }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5)

static IMP orig_popup;
static id hook_popup(id self, SEL cmd, int animated, int type, float x, float y)
{
    say("shown, title", p_msgSend(self, p_sel("title")));
    return ((id (*)(id, SEL, int, int, float, float))orig_popup)(self, cmd, animated, type, x, y);
}

/* -[UIViewController viewDidAppear:]: the controller's class and its view tree */
static IMP orig_appear;
static const char *(*p_className)(id);
static id hook_appear(id self, SEL cmd, int animated)
{
    id r = ((id (*)(id, SEL, int))orig_appear)(self, cmd, animated);
    id v = p_msgSend(self, p_sel("view"));
    id d = v ? p_msgSend(v, p_sel("recursiveDescription")) : 0;
    const char *c = d ? (const char *)p_msgSend(d, p_sel("UTF8String")) : 0;
    char line[160];
    int l = snprintf(line, sizeof(line), "[applog] %s appeared: %s\n", getprogname(),
                     p_className ? p_className(self) : "?");
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) write(2, line, l);
    if (c) {
        size_t n = strlen(c);
        write(2, c, n > 16384 ? 16384 : n);
        write(2, "\n", 1);
    }
    return r;
}

/* MobileSafari's page-load failures (TabDocument, WebFrameLoadDelegate): the
   NSError behind "Cannot Open Page" with its domain, code and userInfo, whose
   _kCFStreamErrorDomainKey / _kCFStreamErrorCodeKey name the layer it came
   from (1 = POSIX socket call, 12 = getaddrinfo). The alert alone says only
   "Cannot allocate memory" (hardware reports, re-log 75). Always on in
   MobileSafari (one console line per failed load); the console is opened
   for these lines only unless applog is on. */
static IMP orig_fail[2];
static int errfd = 2, quiet_app;
static void say_error(const char *what, id err)
{
    char line[1024];
    id dom = err ? p_msgSend(err, p_sel("domain")) : 0;
    long code = err ? (long)p_msgSend(err, p_sel("code")) : 0;
    id info = err ? p_msgSend(err, p_sel("userInfo")) : 0;
    id desc = info ? p_msgSend(info, p_sel("description")) : 0;
    const char *d = dom ? (const char *)p_msgSend(dom, p_sel("UTF8String")) : "?";
    const char *i = desc ? (const char *)p_msgSend(desc, p_sel("UTF8String")) : "";
    int l = snprintf(line, sizeof(line), "[applog] %s load error (%s): %s %ld %s\n", getprogname(), what,
                     d ? d : "?", code, i ? i : "");
    if (l > (int)sizeof(line) - 1) l = sizeof(line) - 1;
    for (int k = 0; k < l - 1; k++)
        if (line[k] == '\n') line[k] = ' ';
    if (quiet_app && errfd == 2) {
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (fd >= 0) errfd = fd;
    }
    applog_write(errfd, line, l);
}

static id hook_fail0(id self, SEL cmd, id wv, id err, id frame);
static id hook_fail1(id self, SEL cmd, id wv, id err, id frame);
static void hook_load_errors(void)
{
    id td = p_getClass("TabDocument");
    Method m = td ? p_getInstanceMethod(td, p_sel("webView:didFailProvisionalLoadWithError:forFrame:")) : 0;
    if (m && !orig_fail[0]) orig_fail[0] = p_setImplementation(m, (IMP)hook_fail0);
    m = td ? p_getInstanceMethod(td, p_sel("webView:didFailLoadWithError:forFrame:")) : 0;
    if (m && !orig_fail[1]) orig_fail[1] = p_setImplementation(m, (IMP)hook_fail1);
}
static id hook_fail0(id self, SEL cmd, id wv, id err, id frame)
{
    say_error("provisional", err);
    return ((id (*)(id, SEL, id, id, id))orig_fail[0])(self, cmd, wv, err, frame);
}
static id hook_fail1(id self, SEL cmd, id wv, id err, id frame)
{
    say_error("load", err);
    return ((id (*)(id, SEL, id, id, id))orig_fail[1])(self, cmd, wv, err, frame);
}

static void hook_alerts(void)
{
    void *h = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!h) return;
    p_getClass = (id (*)(const char *))dlsym(h, "objc_getClass");
    p_getInstanceMethod = (Method (*)(id, SEL))dlsym(h, "class_getInstanceMethod");
    p_setImplementation = (IMP (*)(Method, IMP))dlsym(h, "method_setImplementation");
    p_sel = (SEL (*)(const char *))dlsym(h, "sel_registerName");
    p_msgSend = (id (*)(id, SEL, ...))dlsym(h, "objc_msgSend");
    if (!p_getClass || !p_getInstanceMethod || !p_setImplementation || !p_sel || !p_msgSend) return;
    static const char *names[6][2] = {
        {"UIModalView", "setTitle:"}, {"UIModalView", "setBodyText:"}, {"UIModalView", "setMessage:"},
        {"UIAlertView", "setTitle:"}, {"UIAlertView", "setBodyText:"}, {"UIAlertView", "setMessage:"},
    };
    IMP *origs[6] = {&orig0, &orig1, &orig2, &orig3, &orig4, &orig5};
    const char **whats[6] = {&what0, &what1, &what2, &what3, &what4, &what5};
    IMP hooks[6] = {(IMP)hook0, (IMP)hook1, (IMP)hook2, (IMP)hook3, (IMP)hook4, (IMP)hook5};
    Method done[6] = {0};
    for (int i = 0; i < 6; i++) {
        id cls = p_getClass(names[i][0]);
        Method m = cls ? p_getInstanceMethod(cls, p_sel(names[i][1])) : 0;
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= m && done[j] == m;
        if (!m || dup) continue;
        done[i] = m;
        *whats[i] = names[i][1];
        *origs[i] = p_setImplementation(m, hooks[i]);
    }
    id av = p_getClass("UIAlertView");
    Method m = av ? p_getInstanceMethod(av, p_sel("popupAlertAnimated:animationType:atOffset:")) : 0;
    if (m) orig_popup = p_setImplementation(m, (IMP)hook_popup);
    hook_load_errors();
    p_className = (const char *(*)(id))dlsym(h, "object_getClassName");
    id vc = p_getClass("UIViewController");
    m = vc ? p_getInstanceMethod(vc, p_sel("viewDidAppear:")) : 0;
    if (m) orig_appear = p_setImplementation(m, (IMP)hook_appear);
}

/*
 * Always on, in SpringBoard only: publish whether SpringBoard itself shows
 * something modal (lock screen, an alert item such as "Low Battery", or a
 * full-screen SBAlert such as the power-off slider) as the Darwin notify
 * state "com.ios3ds.sb.modal" (bit 0 locked, 1 alert item, 2 SBAlert). The
 * input daemon sends touches to the frontmost app's port, and the frontmost
 * app stays the app while SpringBoard's lock screen or an alert is over it:
 * touch "locked up" after a popup or a lock (issue #1, New 2DS XL log). The
 * daemon reads this state at every touch-down (tools/ios3ds-input).
 */
static int modal_token = -1, modal_last = -1;
static uint32_t (*p_notify_set_state)(int, uint64_t);

static int sb_responds(id o, const char *name)
{
    return o && ((uintptr_t)p_msgSend(o, p_sel("respondsToSelector:"), p_sel(name)) & 0xff) != 0;   /* BOOL */
}

static id sb_call(id o, const char *name)
{
    return sb_responds(o, name) ? p_msgSend(o, p_sel(name)) : 0;
}

static void modal_tick(void *timer, void *info)
{
    (void)timer; (void)info;
    int m = 0;
    if (sb_call(sb_call(p_getClass("SBAwayController"), "sharedAwayController"), "isLocked")) m |= 1;
    if (sb_call(sb_call(p_getClass("SBAlertItemsController"), "sharedInstance"), "hasVisibleAlert")) m |= 2;
    id app = sb_call(p_getClass("UIApplication"), "sharedApplication");
    if (sb_call(app, "topMostAlert")) m |= 4;
    /* The power-off slider (and the lock screen) is an SBAlert in an
       SBAlertWindow, which topMostAlert does not see. An app stays frontmost
       while one of these windows covers it, so its touches must go to
       SpringBoard. Hardware run 27: the slider over Camera got nothing. */
    if (!(m & 4) && p_className) {
        id ws = sb_call(app, "windows");
        unsigned n = (ws && sb_responds(ws, "count")) ? (unsigned)(uintptr_t)p_msgSend(ws, p_sel("count")) : 0;
        if (n > 32) n = 32;
        for (unsigned i = 0; i < n && sb_responds(ws, "objectAtIndex:"); i++) {
            id w = p_msgSend(ws, p_sel("objectAtIndex:"), i);
            const char *cn = w ? p_className(w) : 0;
            if (cn && !strcmp(cn, "SBAlertWindow") &&
                !((uintptr_t)p_msgSend(w, p_sel("isHidden")) & 0xff)) {   /* BOOL in r0 */
                m |= 4;
                break;
            }
        }
    }
    if (m != modal_last) {
        modal_last = m;
        p_notify_set_state(modal_token, (uint64_t)m);
    }
}

static void modal_start(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!objc || !cf) return;
    p_getClass = (id (*)(const char *))dlsym(objc, "objc_getClass");
    p_sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
    p_msgSend = (id (*)(id, SEL, ...))dlsym(objc, "objc_msgSend");
    p_className = (const char *(*)(id))dlsym(objc, "object_getClassName");
    uint32_t (*reg)(const char *, int *) = (uint32_t (*)(const char *, int *))dlsym(RTLD_DEFAULT, "notify_register_check");
    p_notify_set_state = (uint32_t (*)(int, uint64_t))dlsym(RTLD_DEFAULT, "notify_set_state");
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    double (*now)(void) = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **common = (void **)dlsym(cf, "kCFRunLoopCommonModes");
    if (!p_getClass || !p_sel || !p_msgSend || !reg || !p_notify_set_state || !mk || !now || !main_rl || !add || !common)
        return;
    if (reg("com.ios3ds.sb.modal", &modal_token) != 0) return;
    void *t = mk(NULL, now() + 1.0, 0.1, 0, 0, modal_tick, NULL);
    if (t) add(main_rl(), t, *common);
}

/*
 * SpringBoard warm-up (re-log 77). The first time a folder opens on a freshly
 * installed rootfs, SpringBoard stalls for ~2 s of emulated time (QEMU
 * --icount 2; several seconds on the console) before anything moves:
 *   - Folders' prepareToGhostCurrentPageIconsForRequester: asks every icon of
 *     the page for its "ghostly" (dimmed) image, -[SBIcon getIconImage:4];
 *     lsd renders each one (LICreateGhostlyVersion, ~25 ms of CPU per icon in
 *     QEMU, ~0.5 s for a page) and stores it in Caches/com.apple.IconsCache;
 *   - +[SBLinenView _generateLinenStripsIfNeeded] renders the folder's linen
 *     into Caches/com.apple.SpringBoard.folderSwitcherLinen.* (~0.15 s);
 *   - the first touch of an icon builds +[SBIcon homeScreenIconOverlayImage].
 * The two caches survive reboots, so after that only the first open per boot
 * pays for loading them. This does the same work in the background, one item
 * per main-runloop timer tick (0.5 s apart, from 30 s after SpringBoard
 * starts), so the first folder open finds everything ready. Boot-arg
 * ios3ds_sbwarm=0 turns it off.
 */
static int warm_step;
static id warm_icons;           /* retained NSArray of SBIconModel's leaf icons */
static unsigned warm_n, warm_done;
static double warm_t0, warm_busy;
static double (*p_now)(void);
static void (*p_invalidate)(void *);

static void warm_tick(void *timer, void *info)
{
    (void)info;
    double t = p_now();
    id ctl = sb_call(p_getClass("SBIconController"), "sharedInstance");
    id model = sb_call(p_getClass("SBIconModel"), "sharedInstance");
    if (!ctl || !model || ((uintptr_t)sb_call(ctl, "isEditing") & 0xff))   /* BOOL in r0 */
        return;                                         /* not up yet / user is busy */
    if (!warm_t0)
        warm_t0 = t;
    if (warm_step == 0) {
        sb_call(p_getClass("SBLinenView"), "_generateLinenStripsIfNeeded");
    } else if (warm_step == 1) {
        sb_call(p_getClass("SBIcon"), "homeScreenIconOverlayImage");
        sb_call(p_getClass("SBFolderIcon"), "homeScreenIconOverlayImage");
    } else if (warm_step == 2) {
        id set = sb_call(model, "leafIcons");
        id arr = sb_responds(set, "allObjects") ? sb_call(set, "allObjects") : set;
        warm_icons = arr && sb_responds(arr, "objectAtIndex:") ? sb_call(arr, "retain") : 0;
        warm_n = warm_icons ? (unsigned)(uintptr_t)sb_call(warm_icons, "count") : 0;
    } else if ((unsigned)warm_step - 3 < warm_n) {
        id icon = p_msgSend(warm_icons, p_sel("objectAtIndex:"), (unsigned)warm_step - 3);
        /* the image -[SBIcon prepareGhostlyImage] asks for (format 4) */
        if (sb_responds(icon, "getIconImage:") && !((uintptr_t)sb_call(icon, "isGhostly") & 0xff)) {
            p_msgSend(icon, p_sel("getIconImage:"), 4);
            warm_done++;
        }
    } else {
        char line[160];
        int l = snprintf(line, sizeof(line), "[sbwarm] linen, icon overlay and %u of %u ghostly icons ready "
                         "(%d ms on the main thread, done %d s after the first step)\n",
                         warm_done, warm_n, (int)(warm_busy * 1000), (int)(t - warm_t0));
        if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
        if (l > 0) write(2, line, l);
        if (warm_icons)
            sb_call(warm_icons, "release");
        warm_icons = 0;
        p_invalidate(timer);
        return;
    }
    warm_step++;
    warm_busy += p_now() - t;
}

static void warm_start(void)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) == 0) {
        ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
        if (strstr(ba, "ios3ds_sbwarm=0")) return;
    }
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf || !p_getClass || !p_sel || !p_msgSend) return;
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    p_now = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    p_invalidate = (void (*)(void *))dlsym(cf, "CFRunLoopTimerInvalidate");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **defmode = (void **)dlsym(cf, "kCFRunLoopDefaultMode");
    if (!mk || !p_now || !p_invalidate || !main_rl || !add || !defmode) return;
    /* default mode only: no steps while a scroll/drag tracks touches */
    void *t = mk(NULL, p_now() + 30.0, 0.5, 0, 0, warm_tick, NULL);
    if (t) add(main_rl(), t, *defmode);
}

static void probe(const char *fmt, ...);
static void (*p_tlsfix_invalidate)(void *);
static int tlsfix_loaded, tlsfix_tries;
static void tlsfix_poll(void *timer, void *info)
{
    (void)info;
    if (tlsfix_loaded || ++tlsfix_tries > 240) {   /* give up after a minute */
        if (p_tlsfix_invalidate) p_tlsfix_invalidate(timer);
        return;
    }
    /* TLSFix only makes sense once CFNetwork (its Substrate filter) is up */
    if (dlopen("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", RTLD_LAZY | 0x10 /* NOLOAD */)) {
        tlsfix_loaded = 1;
        dlopen("/usr/lib/ios3ds-CydiaSubstrate.dylib", RTLD_LAZY | RTLD_GLOBAL);
        void *t = dlopen("/usr/lib/ios3ds-tlsfix.dylib", RTLD_LAZY | RTLD_GLOBAL);
        const char *e = dlerror();
        probe("TLSFix copy loaded: %p dlerror=%s", t, e ? e : "(none)");
        if (p_tlsfix_invalidate) p_tlsfix_invalidate(timer);
    }
}
static void tlsfix_deferred(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf) return;
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    double (*now)(void) = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **common = (void **)dlsym(cf, "kCFRunLoopCommonModes");
    p_tlsfix_invalidate = (void (*)(void *))dlsym(cf, "CFRunLoopTimerInvalidate");
    if (!mk || !now || !main_rl || !add || !common) return;
    void *t = mk(NULL, now() + 0.5, 0.25, 0, 0, tlsfix_poll, NULL);
    if (t) add(main_rl(), t, *common);
}

/* ---- Substrate safe mode (issue 1: a tweak that breaks SpringBoard must not
 * stop the device from booting). Every Substrate load on ios3ds goes through
 * load_substrate() below, so it is the one place to say no:
 *   - boot-arg ios3ds_safe=1 (boot.cfg safe_mode=1, or L held while the ARM9
 *     loads iOS): no tweaks in SpringBoard or any app for this boot;
 *   - automatically: SpringBoard counts its starts in SB_STARTS and clears the
 *     count once it has run SB_STABLE_S seconds. Two starts in a row that did
 *     not get that far (a tweak crashing or hanging SpringBoard) make the next
 *     start a safe one; SB_SAFE tells the apps. The next respring after a
 *     stable safe-mode run tries the tweaks again, like Substrate's own safe
 *     mode. */
#define SB_STARTS   "/private/var/mobile/Library/Caches/ios3ds-springboard-starts"
#define SB_SAFE     "/private/var/mobile/Library/Caches/ios3ds-substrate-safe"
#define SB_STABLE_S 90.0
static int safe_bootarg(void)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return 0;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, "ios3ds_safe=");
    return p && p[12] && p[12] != '0';
}
static void say_console(const char *msg)
{
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd < 0) return;
    write(fd, msg, strlen(msg));
    close(fd);
}
static int sb_starts_read(void)
{
    char b[16] = { 0 };
    int fd = open(SB_STARTS, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    return r > 0 ? atoi(b) : 0;
}
static void sb_starts_write(int k)
{
    char b[16];
    int fd = open(SB_STARTS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    int l = snprintf(b, sizeof(b), "%d\n", k);
    write(fd, b, l);
    fsync(fd);
    close(fd);
}
static void sb_stable(void *timer, void *info)
{
    (void)timer; (void)info;
    sb_starts_write(0);
    if (unlink(SB_SAFE) == 0)
        say_console("[safe] SpringBoard ran stably in safe mode: the next respring loads the tweaks again\n");
}
static void sb_stable_arm(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf) return;
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    double (*now)(void) = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **common = (void **)dlsym(cf, "kCFRunLoopCommonModes");
    if (!mk || !now || !main_rl || !add || !common) return;
    void *t = mk(NULL, now() + SB_STABLE_S, 0, 0, 0, sb_stable, NULL);
    if (t) add(main_rl(), t, *common);
}
/* 1 = load the tweaks in this process */
static int substrate_allowed(void)
{
    int sb = !strcmp(getprogname(), "SpringBoard");
    if (safe_bootarg()) {
        if (sb) say_console("[safe] boot-arg ios3ds_safe=1: Substrate tweaks are not loaded this boot\n");
        return 0;
    }
    if (!sb) return access(SB_SAFE, 0 /* F_OK */) != 0;
    int k = sb_starts_read();
    sb_starts_write(k + 1);
    sb_stable_arm();
    if (k >= 2) {
        char m[200];
        snprintf(m, sizeof(m), "[safe] SpringBoard did not run %d s the last %d starts: Substrate tweaks are "
                 "not loaded (safe mode); remove the tweak that broke it in Cydia\n", (int)SB_STABLE_S, k);
        say_console(m);
        int fd = open(SB_SAFE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        return 0;
    }
    unlink(SB_SAFE);
    return 1;
}

static void load_substrate(void)
{
    static const char ms[] = "/Library/MobileSubstrate/MobileSubstrate.dylib";
    const char *e = getenv("DYLD_INSERT_LIBRARIES");
    char exe[1024], old[1024], v[1200];
    unsigned int n = sizeof(exe);
    if (!e || strstr(e, ms) || strlen(e) >= sizeof(old)) return;   /* already inserted */
    if (_NSGetExecutablePath(exe, &n) != 0 || !strstr(exe, ".app/")) return;
    if (!substrate_allowed()) return;
    memcpy(old, e, strlen(e) + 1);
    snprintf(v, sizeof(v), "%s:%s", old, ms);
    /* Substrate's loader looks for itself in DYLD_INSERT_LIBRARIES and takes
     * itself out again, leaving ours. Sandboxed apps (MobileSafari, App Store
     * apps) may not read /Library, so the dlopen fails there; they load the
     * /usr/lib copies instead (see below). */
    setenv("DYLD_INSERT_LIBRARIES", v, 1);
    void *h = dlopen(ms, RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        /* Sandboxed app (MobileSafari, App Store): the profile denies every
           file under /Library, so Substrate's loader cannot read its own
           directory or scan DynamicLibraries. Load the copies under /usr/lib
           (allowed by the profile) instead and bring TLSFix in by hand. This
           needs neither the SANDBOX=1 kernel patch nor a wider sandbox.
           CFNetwork is not loaded yet when an app's constructors run, so
           TLSFix is loaded from a run-loop timer once it is. */
        tlsfix_deferred();
        setenv("DYLD_INSERT_LIBRARIES", old, 1);
    }
}

/* Sandbox probe (issue: TLSFix in MobileSafari). With boot-arg
 * ios3ds_probe=1 every app appends what its sandbox lets it do to
 * /private/var/tmp/ios3ds-applog-probe.txt (the one directory sandboxed apps
 * may write): executable, DYLD_INSERT_LIBRARIES, open() of the Substrate,
 * TLSFix and profile paths, and the dlopen results. The console is denied to
 * a sandboxed app, so this is the only way to see the decision from inside. */
static int probe_on;
static void probe(const char *fmt, ...)
{
    if (!probe_on) return;
    char line[1024];
    int n = snprintf(line, sizeof(line), "[probe] %s pid %d: ", getprogname(), (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n += m;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    int fd = open("/private/var/tmp/ios3ds-applog-probe.txt",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, line, (size_t)n);
    close(fd);
}
static void probe_init(void)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    if (!strstr(ba, "ios3ds_probe=1")) return;
    probe_on = 1;
    char exe[1024];
    unsigned int en = sizeof(exe);
    int r = _NSGetExecutablePath(exe, &en);
    probe("exe %s (r=%d) DYLD_INSERT_LIBRARIES=%s", r == 0 ? exe : "?",
          r, getenv("DYLD_INSERT_LIBRARIES") ? getenv("DYLD_INSERT_LIBRARIES") : "(unset)");
    static const char *paths[] = {
        "/Library/MobileSubstrate/MobileSubstrate.dylib",
        "/Library/MobileSubstrate/DynamicLibraries/tlsfix.dylib",
        "/var/mobile/Library/ConfigurationProfiles/ios3ds-roots.plist",
        "/usr/lib/libobjc.A.dylib",
        "/private/var/tmp",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_RDONLY);
        probe("open %s -> %d errno %d", paths[i], fd, fd < 0 ? errno : 0);
        if (fd >= 0) close(fd);
    }
}

/* ---- fake GPS (CoreLocation hooks, docs/location.md) ------------------
 *
 * iOS 4.2.1 has no location simulation API for third parties, locationd is
 * disabled on ios3ds (recipe.json) and injecting anything into a daemon can
 * hang the boot (docs/cydia.md). So all the work happens in the app: this
 * dylib is already inserted into SpringBoard and every app (libgmalloc link),
 * including sandboxed MobileSafari, which can read /usr/lib. When a location
 * is configured we hook CLLocationManager's class and instance methods, keep
 * a list of managers that asked for updates and call their delegate every
 * second from the main run loop. Without a gps= boot-arg and without the
 * preferences plist none of this is installed.
 */
static int gps_enabled = 0;
static double gps_lat = 0, gps_lon = 0, gps_acc = 50.0;
static void *gps_cfh = NULL;
static void * gps_managers = NULL;
static void * gps_timer = NULL;
static int gps_hooks_installed = 0;
static int gps_logging = 0;

static void gps_log(const char *fmt, ...)
{
    if (!gps_logging) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) write(2, line, (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
}

/* CoreFoundation function pointers */
static void *(*gps_CFStringCreateWithCString)(void *, const char *, unsigned) = NULL;
static void *(*gps_CFArrayCreateMutable)(void *, long, const void *) = NULL;
static long (*gps_CFArrayGetCount)(void *) = NULL;
static const void *(*gps_CFArrayGetValueAtIndex)(void *, long) = NULL;
static void (*gps_CFArrayAppendValue)(void *, const void *) = NULL;
static void (*gps_CFArrayRemoveValueAtIndex)(void *, long) = NULL;
static void *(*gps_CFRunLoopTimerCreate)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) = NULL;
static void *(*gps_CFRunLoopGetMain)(void) = NULL;
static void (*gps_CFRunLoopAddTimer)(void *, void *, void *) = NULL;
static void **gps_kCFRunLoopCommonModes = NULL;
static double (*gps_CFAbsoluteTimeGetCurrent)(void) = NULL;
static void *(*gps_CFNotificationCenterGetDarwinNotifyCenter)(void) = NULL;
static void (*gps_CFNotificationCenterAddObserver)(void *, void *, void (*)(void *, void *, void *, const void *, void *), void *, const void *, int) = NULL;

static void *gps_cf(void)
{
    if (!gps_cfh)
        gps_cfh = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    return gps_cfh;
}

static void *gps_cfstr(const char *c)
{
    if (!gps_CFStringCreateWithCString) {
        void *h = gps_cf();
        if (h) gps_CFStringCreateWithCString = dlsym(h, "CFStringCreateWithCString");
    }
    if (!gps_CFStringCreateWithCString) return NULL;
    return gps_CFStringCreateWithCString(NULL, c, 0x08000100);
}

static void gps_load_cf(void)
{
    void *h = gps_cf();
    if (!h) return;
    if (!gps_CFArrayCreateMutable) gps_CFArrayCreateMutable = dlsym(h, "CFArrayCreateMutable");
    if (!gps_CFArrayGetCount) gps_CFArrayGetCount = dlsym(h, "CFArrayGetCount");
    if (!gps_CFArrayGetValueAtIndex) gps_CFArrayGetValueAtIndex = dlsym(h, "CFArrayGetValueAtIndex");
    if (!gps_CFArrayAppendValue) gps_CFArrayAppendValue = dlsym(h, "CFArrayAppendValue");
    if (!gps_CFArrayRemoveValueAtIndex) gps_CFArrayRemoveValueAtIndex = dlsym(h, "CFArrayRemoveValueAtIndex");
    if (!gps_CFRunLoopTimerCreate) gps_CFRunLoopTimerCreate = dlsym(h, "CFRunLoopTimerCreate");
    if (!gps_CFRunLoopGetMain) gps_CFRunLoopGetMain = dlsym(h, "CFRunLoopGetMain");
    if (!gps_CFRunLoopAddTimer) gps_CFRunLoopAddTimer = dlsym(h, "CFRunLoopAddTimer");
    if (!gps_kCFRunLoopCommonModes) gps_kCFRunLoopCommonModes = dlsym(h, "kCFRunLoopCommonModes");
    if (!gps_CFAbsoluteTimeGetCurrent) gps_CFAbsoluteTimeGetCurrent = dlsym(h, "CFAbsoluteTimeGetCurrent");
    if (!gps_CFNotificationCenterGetDarwinNotifyCenter) gps_CFNotificationCenterGetDarwinNotifyCenter = dlsym(h, "CFNotificationCenterGetDarwinNotifyCenter");
    if (!gps_CFNotificationCenterAddObserver) gps_CFNotificationCenterAddObserver = dlsym(h, "CFNotificationCenterAddObserver");
}

static id gps_make_location(void)
{
    if (!gps_enabled) return NULL;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!objc) return NULL;
    if (!p_getClass) p_getClass = (id (*)(const char *))dlsym(objc, "objc_getClass");
    if (!p_sel) p_sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
    if (!p_msgSend) p_msgSend = (id (*)(id, SEL, ...))dlsym(objc, "objc_msgSend");
    if (!p_getClass || !p_sel || !p_msgSend) return NULL;

    dlopen("/System/Library/Frameworks/CoreLocation.framework/CoreLocation", RTLD_NOW | RTLD_GLOBAL);
    id locCls = p_getClass("CLLocation");
    if (!locCls) return NULL;
    id nsDate = p_getClass("NSDate");
    if (!nsDate) return NULL;

    SEL allocSel = p_sel("alloc");
    SEL initSel = p_sel("initWithCoordinate:altitude:horizontalAccuracy:verticalAccuracy:course:speed:timestamp:");
    /* iOS 2.x/3.x have only the 2.0 initializer (no course/speed) */
    SEL oldSel = p_sel("initWithCoordinate:altitude:horizontalAccuracy:verticalAccuracy:timestamp:");
    int full = (char)(uintptr_t)p_msgSend(locCls, p_sel("instancesRespondToSelector:"), initSel) != 0;
    id obj = p_msgSend(locCls, allocSel);
    typedef struct { double latitude; double longitude; } Coord;
    Coord coord = { gps_lat, gps_lon };
    /* -date would autorelease; there is no run-loop pool inside a timer
       callback, so make our own date and release it after the init. */
    id date = p_msgSend(p_msgSend(nsDate, p_sel("alloc")), p_sel("init"));
    id loc = full
        ? ((id (*)(id, SEL, Coord, double, double, double, double, double, id))p_msgSend)(
              obj, initSel, coord, 0.0, gps_acc, gps_acc, -1.0, -1.0, date)
        : ((id (*)(id, SEL, Coord, double, double, double, id))p_msgSend)(
              obj, oldSel, coord, 0.0, gps_acc, gps_acc, date);
    p_msgSend(date, p_sel("release"));
    if (!loc) p_msgSend(obj, p_sel("release"));
    return loc;
}

static void gps_register_manager(id m)
{
    gps_load_cf();
    if (!gps_managers) {
        if (!gps_CFArrayCreateMutable) return;
        /* weak pointers: retaining a manager here would keep every app's
           CLLocationManager alive after stopUpdatingLocation. */
        struct callbacks {
            long version;
            const void *(*retain)(const void *);
            void (*release)(const void *);
            const void *(*copyDescription)(const void *);
            unsigned char (*equal)(const void *, const void *);
        } cb = { 0, NULL, NULL, NULL, NULL };
        gps_managers = gps_CFArrayCreateMutable(NULL, 0, (const void *)&cb);
    }
    if (!gps_managers) return;
    long n = gps_CFArrayGetCount(gps_managers);
    for (long i = 0; i < n; i++) {
        if (gps_CFArrayGetValueAtIndex(gps_managers, i) == (const void *)m)
            return;
    }
    gps_CFArrayAppendValue(gps_managers, (const void *)m);
}

static void gps_unregister_manager(id m)
{
    if (!gps_managers || !gps_CFArrayGetCount) return;
    long n = gps_CFArrayGetCount(gps_managers);
    for (long i = n - 1; i >= 0; i--) {
        if (gps_CFArrayGetValueAtIndex(gps_managers, i) == (const void *)m) {
            if (gps_CFArrayRemoveValueAtIndex) gps_CFArrayRemoveValueAtIndex(gps_managers, i);
        }
    }
}

static void gps_timer_cb(void *timer, void *info)
{
    (void)timer; (void)info;
    if (!gps_enabled || !gps_managers) return;
    if (!p_msgSend || !p_sel) return;
    id loc = gps_make_location();
    if (!loc) return;
    long n = gps_CFArrayGetCount(gps_managers);
    for (long i = n - 1; i >= 0; i--) {
        id mgr = (id)gps_CFArrayGetValueAtIndex(gps_managers, i);
        id delegate = p_msgSend(mgr, p_sel("delegate"));
        if (!delegate) continue;
        SEL cb = p_sel("locationManager:didUpdateToLocation:fromLocation:");
        char r = (char)(uintptr_t)p_msgSend(delegate, p_sel("respondsToSelector:"), cb);
        if (r) {
            ((void (*)(id, SEL, id, id, id))p_msgSend)(delegate, cb, mgr, loc, NULL);
        }
    }
    p_msgSend(loc, p_sel("release"));
}

static void gps_start_timer(void)
{
    gps_load_cf();
    if (gps_timer) return;
    if (!gps_CFRunLoopTimerCreate || !gps_CFAbsoluteTimeGetCurrent || !gps_CFRunLoopGetMain || !gps_CFRunLoopAddTimer || !gps_kCFRunLoopCommonModes)
        return;
    gps_timer = gps_CFRunLoopTimerCreate(NULL, gps_CFAbsoluteTimeGetCurrent() + 1.0, 1.0, 0, 0, gps_timer_cb, NULL);
    if (gps_timer)
        gps_CFRunLoopAddTimer(gps_CFRunLoopGetMain(), gps_timer, *gps_kCFRunLoopCommonModes);
}

static IMP gps_orig_locationServicesEnabled = NULL;
static IMP gps_orig_authorizationStatus = NULL;
static IMP gps_orig_instLocationServicesEnabled = NULL;   /* 2.x/3.x: an instance method */
static IMP gps_orig_location = NULL;
static IMP gps_orig_startUpdating = NULL;
static IMP gps_orig_stopUpdating = NULL;

static char gps_hook_locationServicesEnabled(id self, SEL cmd)
{
    if (gps_enabled) return 1;
    if (gps_orig_locationServicesEnabled)
        return ((char (*)(id, SEL))gps_orig_locationServicesEnabled)(self, cmd);
    return 0;
}

static char gps_hook_instLocationServicesEnabled(id self, SEL cmd)
{
    if (gps_enabled) return 1;
    if (gps_orig_instLocationServicesEnabled)
        return ((char (*)(id, SEL))gps_orig_instLocationServicesEnabled)(self, cmd);
    return 0;
}

static int gps_hook_authorizationStatus(id self, SEL cmd)
{
    if (gps_enabled) return 3;
    if (gps_orig_authorizationStatus)
        return ((int (*)(id, SEL))gps_orig_authorizationStatus)(self, cmd);
    return 0;
}

static id gps_hook_location(id self, SEL cmd)
{
    if (gps_enabled) {
        id loc = gps_make_location();
        return p_msgSend(loc, p_sel("autorelease"));
    }
    if (gps_orig_location)
        return ((id (*)(id, SEL))gps_orig_location)(self, cmd);
    return NULL;
}

static void gps_hook_startUpdating(id self, SEL cmd)
{
    /* when we supply the location the real service (locationd, disabled on
       ios3ds) is not needed; when off, behave exactly like stock iOS */
    if (gps_enabled) {
        gps_register_manager(self);
        return;
    }
    if (gps_orig_startUpdating)
        ((void (*)(id, SEL))gps_orig_startUpdating)(self, cmd);
}

static void gps_hook_stopUpdating(id self, SEL cmd)
{
    gps_unregister_manager(self);
    if (gps_orig_stopUpdating)
        ((void (*)(id, SEL))gps_orig_stopUpdating)(self, cmd);
}

static void gps_install_hooks(void)
{
    if (gps_hooks_installed) return;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!objc) return;
    if (!p_getClass) p_getClass = (id (*)(const char *))dlsym(objc, "objc_getClass");
    if (!p_sel) p_sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
    if (!p_msgSend) p_msgSend = (id (*)(id, SEL, ...))dlsym(objc, "objc_msgSend");
    if (!p_getClass || !p_sel || !p_msgSend) return;

    dlopen("/System/Library/Frameworks/CoreLocation.framework/CoreLocation", RTLD_NOW | RTLD_GLOBAL);
    id cls = p_getClass("CLLocationManager");
    if (!cls) return;

    Method (*p_class_getClassMethod)(id, SEL) = dlsym(RTLD_DEFAULT, "class_getClassMethod");
    Method (*p_class_getInstanceMethod)(id, SEL) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
    IMP (*p_method_setImplementation)(Method, IMP) = dlsym(RTLD_DEFAULT, "method_setImplementation");
    if (!p_class_getClassMethod || !p_class_getInstanceMethod || !p_method_setImplementation) return;

    Method m;
    m = p_class_getClassMethod(cls, p_sel("locationServicesEnabled"));
    if (m) gps_orig_locationServicesEnabled = p_method_setImplementation(m, (IMP)gps_hook_locationServicesEnabled);
    m = p_class_getClassMethod(cls, p_sel("authorizationStatus"));
    if (m) gps_orig_authorizationStatus = p_method_setImplementation(m, (IMP)gps_hook_authorizationStatus);
    m = p_class_getInstanceMethod(cls, p_sel("locationServicesEnabled"));
    if (m) gps_orig_instLocationServicesEnabled = p_method_setImplementation(m, (IMP)gps_hook_instLocationServicesEnabled);
    m = p_class_getInstanceMethod(cls, p_sel("location"));
    if (m) gps_orig_location = p_method_setImplementation(m, (IMP)gps_hook_location);
    m = p_class_getInstanceMethod(cls, p_sel("startUpdatingLocation"));
    if (m) gps_orig_startUpdating = p_method_setImplementation(m, (IMP)gps_hook_startUpdating);
    m = p_class_getInstanceMethod(cls, p_sel("stopUpdatingLocation"));
    if (m) gps_orig_stopUpdating = p_method_setImplementation(m, (IMP)gps_hook_stopUpdating);

    gps_hooks_installed = 1;
}

/* Read the preferences file directly instead of through CFPreferences:
   on 4.2.1 a CFPreferences domain caches its values in the process, so a
   notify_post from ios3ds-gps did not change what running apps saw. The
   command-line tool writes the same plist to the Preferences folder and to
   /private/var/tmp (the latter is readable by sandboxed apps). */
static int gps_plist_number(const char *buf, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "<key>%s</key>", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p = strstr(p, "<real>");
    return p && sscanf(p, "<real>%lf</real>", out) == 1;
}

static int gps_read_domain(void)
{
    static const char *paths[] = {
        "/private/var/tmp/com.ios3ds.location.plist",
        "/var/mobile/Library/Preferences/com.ios3ds.location.plist",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        /* fopen/fread are not exported by the iOS 4.2 stub SDK; use POSIX I/O. */
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) continue;
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n < 0) continue;
        buf[n] = 0;
        const char *en = strstr(buf, "<key>Enabled</key>");
        if (!en) continue;
        if (!strstr(en, "<true/>")) {
            gps_enabled = 0;
            return 1;
        }
        double la, lo, ac;
        if (!gps_plist_number(buf, "Latitude", &la) ||
            !gps_plist_number(buf, "Longitude", &lo) ||
            !gps_plist_number(buf, "Accuracy", &ac))
            continue;
        char s[128];
        snprintf(s, sizeof(s), "%.17g,%.17g,%.17g", la, lo, ac);
        if (gps_parse(s, &gps_lat, &gps_lon, &gps_acc) != 0) continue;
        gps_enabled = 1;
        return 1;
    }
    return 0;
}

static void gps_reload_config(void)
{
    gps_load_cf();

    /* boot-args are read once: they carry both the boot-arg location and the
       optional ios3ds_gpslog=1 switch for the [gps] console lines */
    char ba[512];
    size_t n = sizeof(ba) - 1;
    int have_ba = sysctlbyname("kern.bootargs", ba, &n, NULL, 0) == 0;
    if (have_ba) ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    gps_logging = have_ba && strstr(ba, "ios3ds_gpslog=1") != NULL;
    if (!gps_read_domain()) {
        /* fall back to boot arg */
        if (have_ba) {
            char *p = strstr(ba, "ios3ds_gps=");
            if (p) {
                char buf[128];
                int i = 0;
                p += 11;
                while (*p && *p != ' ' && i < (int)sizeof(buf) - 1)
                    buf[i++] = *p++;
                buf[i] = 0;
                double la, lo, ac;
                if (gps_parse(buf, &la, &lo, &ac) == 0) {
                    gps_lat = la;
                    gps_lon = lo;
                    gps_acc = ac;
                    gps_enabled = 1;
                } else {
                    gps_enabled = 0;
                }
            } else {
                gps_enabled = 0;
            }
        } else {
            gps_enabled = 0;
        }
    }

    gps_log("[gps] %s: %s %.6f,%.6f acc %.1f m\n", getprogname(),
            gps_enabled ? "on" : "off", gps_lat, gps_lon, gps_acc);
    if (gps_enabled) {
        gps_install_hooks();
        gps_start_timer();
    }
}

static void gps_notify_cb(void *center, void *observer, void *name, const void *object, void *userInfo)
{
    (void)center; (void)observer; (void)name; (void)object; (void)userInfo;
    gps_log("[gps] Darwin notification received\n");
    gps_reload_config();
}

static void gps_register_notify(void)
{
    gps_load_cf();
    if (!gps_CFNotificationCenterGetDarwinNotifyCenter || !gps_CFNotificationCenterAddObserver) return;
    void *ctr = gps_CFNotificationCenterGetDarwinNotifyCenter();
    if (!ctr) return;
    gps_CFNotificationCenterAddObserver(ctr, NULL, gps_notify_cb, gps_cfstr("com.ios3ds.location.changed"), NULL, 1);
}

__attribute__((constructor)) static void applog_init(void)
{
    probe_init();
    if (!strcmp(getprogname(), "SpringBoard")) {
        modal_start();
        warm_start();
    }
    load_substrate();
    if (probe_on) {
        void *ms = dlopen("/Library/MobileSubstrate/MobileSubstrate.dylib", RTLD_LAZY | 0x10 /* NOLOAD */);
        void *lf = dlopen("/Library/MobileSubstrate/DynamicLibraries/tlsfix.dylib", RTLD_LAZY | 0x10);
        void *us = dlopen("/usr/lib/ios3ds-CydiaSubstrate.dylib", RTLD_LAZY | 0x10);
        void *ut = dlopen("/usr/lib/ios3ds-tlsfix.dylib", RTLD_LAZY | 0x10);
        const char *e = dlerror();
        probe("after Substrate: ms=%p tlsfix=%p usr_cs=%p usr_tls=%p dlerror=%s",
              ms, lf, us, ut, e ? e : "(none)");
    }
    gps_register_notify();
    gps_reload_config();
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, "ios3ds_diag=");
    const char *q = strstr(ba, "ios3ds_applog=");
    if (!(p && p[12] && p[12] != '0') && !(q && q[14] && q[14] != '0')) {
        if (!strcmp(getprogname(), "MobileSafari")) {
            void *h = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
            if (!h) return;
            p_getClass = (id (*)(const char *))dlsym(h, "objc_getClass");
            p_getInstanceMethod = (Method (*)(id, SEL))dlsym(h, "class_getInstanceMethod");
            p_setImplementation = (IMP (*)(Method, IMP))dlsym(h, "method_setImplementation");
            p_sel = (SEL (*)(const char *))dlsym(h, "sel_registerName");
            p_msgSend = (id (*)(id, SEL, ...))dlsym(h, "objc_msgSend");
            if (!p_getClass || !p_getInstanceMethod || !p_setImplementation || !p_sel || !p_msgSend) return;
            quiet_app = 1;
            hook_load_errors();
        }
        return;
    }
    setenv("CFLOG_FORCE_STDERR", "1", 1);
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd < 0) return;
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    /* the app's children (and the next exec) need not inherit us */
    unsetenv("DYLD_INSERT_LIBRARIES");
    char line[160];
    int l = snprintf(line, sizeof(line), "[applog] %s pid %d%s%s\n", getprogname(), (int)getpid(),
                     dlopen("/Library/MobileSubstrate/MobileSubstrate.dylib", RTLD_LAZY | 0x10 /* NOLOAD */)
                         ? ", Substrate loaded" : "",
                     dlopen("/Library/MobileSubstrate/DynamicLibraries/tlsfix.dylib", RTLD_LAZY | 0x10)
                         ? ", TLSFix loaded" : "");
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) write(2, line, l);
    hook_alerts();
}
