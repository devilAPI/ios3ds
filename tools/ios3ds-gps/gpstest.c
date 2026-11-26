/*
 * ios3ds-gpstest: a tiny CLLocationManager client for QEMU (re-log 83).
 *
 * It is launched by com.ios3ds.gpstest.plist through
 * "ios3ds-diag -g ios3ds_gps", so it only runs when boot.cfg has a gps= key.
 * It loads ios3ds-applog.dylib (which contains the CoreLocation hooks), asks
 * for updates, prints the delegate callbacks and the -location property once
 * a second, and switches the location with ios3ds-gps(1) in the middle. That
 * exercises both the boot-arg and the runtime/Darwin-notification path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int system(const char *);

typedef void *id;
typedef void *SEL;
typedef void *IMP;
typedef void *Class;

static id (*x_msgSend)(id, SEL, ...);
static void (*x_msgSendStret)(void *, id, SEL);
static double (*x_runMode)(void *, double, unsigned char);
static void **x_defaultMode;
static SEL (*x_sel)(const char *);
static id (*x_getClass)(const char *);
static Class (*x_addMethod)(Class, SEL, IMP, const char *);
static FILE *out;

typedef struct { double latitude, longitude; } Coord;

static void print_location(const char *what, id loc)
{
    if (!loc) {
        fprintf(out, "[gpstest] %s: (null)\n", what);
        return;
    }
    /* CLLocationCoordinate2D is a 16-byte struct: ARM ObjC needs
       objc_msgSend_stret, not objc_msgSend (which crashes here). */
    Coord c = { 0, 0 };
    x_msgSendStret(&c, loc, x_sel("coordinate"));
    double (*acc)(id, SEL) = (double (*)(id, SEL))x_msgSend;
    fprintf(out, "[gpstest] %s: %.6f,%.6f acc %.1f m\n", what,
            c.latitude, c.longitude, acc(loc, x_sel("horizontalAccuracy")));
    fflush(out);
}

/* a delegate method added to NSObject for the duration of this test process */
static void delegate_update(id self, SEL cmd, id manager, id newLoc, id oldLoc)
{
    (void)self; (void)cmd; (void)manager; (void)oldLoc;
    print_location("delegate", newLoc);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 12;
    if (secs < 2) secs = 12;
    /* IOS3DS_HWTEST (ios3ds-hwtest): output stays on our stdout pipe and the
       location is not switched at runtime */
    int hw = getenv("IOS3DS_HWTEST") != NULL;
    int fd = hw ? -1 : open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }
    out = stdout;
    fprintf(out, "[gpstest] starting (%d s)\n", secs);
    fflush(out);

    /* install the CoreLocation hooks before any CLLocationManager is used */
    if (!dlopen("/usr/lib/ios3ds-applog.dylib", RTLD_NOW | RTLD_GLOBAL)) {
        fprintf(out, "[gpstest] applog dlopen failed\n");
        return 1;
    }
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!objc) return 1;
    x_msgSend = (id (*)(id, SEL, ...))dlsym(objc, "objc_msgSend");
    x_msgSendStret = (void (*)(void *, id, SEL))dlsym(objc, "objc_msgSend_stret");
    x_sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
    x_getClass = (id (*)(const char *))dlsym(objc, "objc_getClass");
    x_addMethod = (Class (*)(Class, SEL, IMP, const char *))dlsym(objc, "class_addMethod");
    if (!x_msgSend || !x_sel || !x_getClass || !x_addMethod) return 1;
    void *cl = dlopen("/System/Library/Frameworks/CoreLocation.framework/CoreLocation", RTLD_NOW | RTLD_GLOBAL);
    id mgrClass = x_getClass("CLLocationManager");
    if (!cl || !mgrClass) {
        fprintf(out, "[gpstest] CoreLocation not available\n");
        return 1;
    }
    Class ns = (Class)x_getClass("NSObject");
    SEL cb = x_sel("locationManager:didUpdateToLocation:fromLocation:");
    if (!ns || !x_addMethod(ns, cb, (IMP)delegate_update, "v@:@@@"))
        fprintf(out, "[gpstest] warning: could not add delegate method\n");
    fprintf(out, "[gpstest] runtime ready\n");

    id mgr = x_msgSend(x_msgSend(mgrClass, x_sel("alloc")), x_sel("init"));
    id delegate = x_msgSend(x_msgSend(ns, x_sel("alloc")), x_sel("init"));
    fprintf(out, "[gpstest] manager %p, delegate %p\n", mgr, delegate);
    x_msgSend(mgr, x_sel("setDelegate:"), delegate);
    /* 4.x: class methods; 2.x/3.x: -locationServicesEnabled only, no
       authorization status (reported as -1) */
    SEL se = x_sel("locationServicesEnabled"), au = x_sel("authorizationStatus");
    SEL rts = x_sel("respondsToSelector:");
    int services = (char)(uintptr_t)x_msgSend(mgrClass, rts, se)
        ? (char)(uintptr_t)x_msgSend(mgrClass, se) : (char)(uintptr_t)x_msgSend(mgr, se);
    int auth = (char)(uintptr_t)x_msgSend(mgrClass, rts, au) ? (int)(uintptr_t)x_msgSend(mgrClass, au) : -1;
    fprintf(out, "[gpstest] services=%d authorization=%d\n", services, auth);
    fprintf(out, "[gpstest] calling startUpdatingLocation\n");
    x_msgSend(mgr, x_sel("startUpdatingLocation"));
    fprintf(out, "[gpstest] updates started\n");

    /* the hook's 1 Hz timer and the Darwin notification are runloop sources:
       a plain C tool has to run the main runloop for them to fire */
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (cf) {
        x_runMode = (double (*)(void *, double, unsigned char))dlsym(cf, "CFRunLoopRunInMode");
        x_defaultMode = (void **)dlsym(cf, "kCFRunLoopDefaultMode");
    }
    for (int i = 0; i < secs; i++) {
        if (x_runMode && x_defaultMode) x_runMode(*x_defaultMode, 1.0, 0);
        else sleep(1);
        fprintf(out, "[gpstest] tick %d\n", i);
        print_location("location", x_msgSend(mgr, x_sel("location")));
        if (i == secs / 2 && !hw) {
            fprintf(out, "[gpstest] switching location at runtime\n");
            if (system("/usr/bin/ios3ds-gps set 40.7128 -74.0060 20; /usr/bin/ios3ds-gps show") != 0)
                fprintf(out, "[gpstest] ios3ds-gps set failed\n");
        }
    }
    x_msgSend(mgr, x_sel("stopUpdatingLocation"));
    fprintf(out, "[gpstest] done\n");
    return 0;
}
