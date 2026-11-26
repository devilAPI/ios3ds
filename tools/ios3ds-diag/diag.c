/*
 * ios3ds-diag: launchd wrapper for the test and diagnostic jobs (gltest, the
 * boot tone, the [smp] sampler). Runs "argv[1] argv[2..]" only when the
 * kernel boot-args contain ios3ds_diag=1 (boot.cfg diag=1 adds it), else
 * exits 0 at once. docs/boot-perf.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <dlfcn.h>

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

/* "ios3ds-diag -iobusy [secs]": list the IOServices whose busy count is not 0,
   i.e. what launchctl's IOKitWaitQuiet (5 s timeout at boot) waits for.
   IOKit is looked up with dlsym: the stub SDK lists only the symbols the
   other tools import. */
typedef unsigned int io_obj;
static int iobusy(int secs)
{
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!h) return 1;
    void *(*matching)(const char *) = (void *(*)(const char *))dlsym(h, "IOServiceMatching");
    int (*get)(unsigned, void *, io_obj *) = (int (*)(unsigned, void *, io_obj *))dlsym(h, "IOServiceGetMatchingServices");
    io_obj (*next)(io_obj) = (io_obj (*)(io_obj))dlsym(h, "IOIteratorNext");
    int (*busy)(io_obj, unsigned *) = (int (*)(io_obj, unsigned *))dlsym(h, "IOServiceGetBusyState");
    int (*name)(io_obj, char *) = (int (*)(io_obj, char *))dlsym(h, "IORegistryEntryGetName");
    int (*cls)(io_obj, char *) = (int (*)(io_obj, char *))dlsym(h, "IOObjectGetClass");
    int (*rel)(io_obj) = (int (*)(io_obj))dlsym(h, "IOObjectRelease");
    io_obj (*root)(unsigned) = (io_obj (*)(unsigned))dlsym(h, "IORegistryGetRootEntry");
    int (*iter)(io_obj, const char *, unsigned, io_obj *) =
        (int (*)(io_obj, const char *, unsigned, io_obj *))dlsym(h, "IORegistryEntryCreateIterator");
    int (*state)(io_obj, unsigned long long *) = (int (*)(io_obj, unsigned long long *))dlsym(h, "IOServiceGetState");
    if (!matching || !get || !next || !busy || !name || !cls || !rel) return 1;
    for (int pass = 0; pass <= secs; pass++) {
        io_obj it = 0, s;
        /* the whole IOService plane (also services not registered yet), else matching */
        if (!(root && iter && iter(root(0), "IOService", 1, &it) == 0) &&
            get(0, matching("IOService"), &it) != 0) return 1;
        int total = 0, nbusy = 0;
        while ((s = next(it)) != 0) {
            unsigned b = 0;
            char n[128] = "", c[128] = "";
            total++;
            if (busy(s, &b) == 0 && b) {
                name(s, n);
                cls(s, c);
                unsigned long long st = 0;
                if (state) state(s, &st);
                printf("[iobusy] %ds: %s (%s) busy %u state %llx\n", pass, n, c, b, st);
                nbusy++;
            }
            rel(s);
        }
        rel(it);
        printf("[iobusy] %ds: %d of %d services busy\n", pass, nbusy, total);
        fflush(stdout);
        if (!nbusy) break;
        sleep(1);
    }
    return 0;
}

/* "ios3ds-diag -urlpoll a.b.c.d port": QEMU test aid (emu/safari_test.py).
   Every 2 s it GETs http://a.b.c.d:port/next (BSD sockets, HTTP/1.0); a body
   that starts with "http" and differs from the last one is opened with
   SpringBoardServices' SBSOpenSensitiveURLAndUnlock, i.e. in Safari, as a
   link from another app would. One "[urltest]" line per URL.

   Without a boot-arg the old 40 s "no answer, open p1 anyway" fallback stays:
   emu/safari_test.py --nolink uses it to exercise Safari with no network.
   With ios3ds_urlwait=1 (set by every other safari_test run) the poll waits
   for the harness's URL instead: on a cold boot of 4.1/3.1.3 the 40 s timer
   fires before Wi-Fi/DHCP is up, so Safari was launched with no link and
   its first page failed with ENOMEM/EINVAL (re-log 143). */
#include <errno.h>
#include <stdint.h>
time_t time(time_t *);
struct sin4 { uint8_t len, family; uint16_t port; uint32_t addr; char zero[8]; };
int socket(int, int, int);
int connect(int, const void *, unsigned);
long send(int, const void *, size_t, int);
long recv(int, void *, size_t, int);
static int urlpoll(const char *ip, int port)
{
    char ba[512];
    size_t bn = sizeof(ba) - 1;
    int wait_for_url = 0;
    if (sysctlbyname("kern.bootargs", ba, &bn, NULL, 0) == 0) {
        ba[bn < sizeof(ba) ? bn : sizeof(ba) - 1] = 0;
        const char *q = strstr(ba, "ios3ds_urlport=");
        if (q) port = atoi(q + 15);     /* test aid: 8080 is often taken on the host */
        wait_for_url = strstr(ba, "ios3ds_urlwait=1") != NULL;
    }
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
    if (!cf || !sbs) return 1;
    void *(*mkurl)(void *, const char *, long, unsigned, void *) =
        (void *(*)(void *, const char *, long, unsigned, void *))dlsym(cf, "CFURLCreateWithBytes");
    void (*rel)(void *) = (void (*)(void *))dlsym(cf, "CFRelease");
    int (*open_url)(void *, int) = (int (*)(void *, int))dlsym(sbs, "SBSOpenSensitiveURLAndUnlock");
    unsigned a, b, c, d;
    time_t t0 = time(NULL);
    char last[256] = "";
    if (!mkurl || !rel || !open_url || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 1;
    for (;;) {
        char buf[1024], *body;
        long n, got = 0;
        struct sin4 sa = { sizeof sa, 2, (uint16_t)((port >> 8) | ((port & 255) << 8)), a | b << 8 | c << 16 | d << 24, { 0 } };
        int s = socket(2, 1, 0);
        sleep(2);
        if (s < 0) continue;
        if (connect(s, &sa, sizeof sa) == 0 &&
            send(s, "GET /next HTTP/1.0\r\n\r\n", 22, 0) == 22)
            while (got < (long)sizeof(buf) - 1 && (n = recv(s, buf + got, sizeof(buf) - 1 - got, 0)) > 0)
                got += n;
        close(s);
        buf[got] = 0;
        if (!got && !last[0] && !wait_for_url && time(NULL) - t0 >= 40) {
            /* no answer for 40 s (a connect without a link can itself take
               75 s): open the first page anyway, as a user opening Safari
               before Wi-Fi is up would (emu/safari_test.py --nolink).
               Normal runs set ios3ds_urlwait=1 so that a slow DHCP never
               turns into a page load before the link is up. */
            snprintf(buf, sizeof(buf), "HTTP/1.0 200 OK\r\n\r\nhttp://%s:%d/p1.html", ip, port);
            got = (long)strlen(buf);
            printf("[urltest] no answer from %s:%d for 40 s: opening its first page anyway\n", ip, port);
        }
        if (!(body = strstr(buf, "\r\n\r\n"))) continue;
        body += 4;
        for (n = 0; body[n] && body[n] != '\r' && body[n] != '\n' && body[n] != ' '; n++) ;
        body[n] = 0;
        if (strncmp(body, "http", 4) || !strcmp(body, last) || strlen(body) >= sizeof(last)) continue;
        strcpy(last, body);
        void *u = mkurl(0, body, (long)strlen(body), 0x08000100 /* UTF-8 */, 0);
        int r = u ? open_url(u, 1) : -1;
        if (u) rel(u);
        printf("[urltest] open %s -> %d\n", body, r);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    const char *gate = "ios3ds_diag=";
    char g[64];
    if (argc >= 2 && !strcmp(argv[1], "-iobusy")) return iobusy(argc >= 3 ? atoi(argv[2]) : 0);
    if (argc >= 4 && !strcmp(argv[1], "-urlpoll")) return urlpoll(argv[2], atoi(argv[3]));
    /* "-g name": the job runs with boot-arg name=1 instead of ios3ds_diag=1,
       e.g. ios3ds_mem=1 for the [mem] report (diag=1's [smp] sampler prints
       the same lines after every round) */
    if (argc >= 4 && !strcmp(argv[1], "-g")) {
        snprintf(g, sizeof(g), "%s=", argv[2]);
        gate = g;
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: ios3ds-diag [-g bootarg] program [args...]\n");
        return 2;
    }
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return 0;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, gate);
    size_t gl = strlen(gate);
    if (!p || p[gl] == 0 || p[gl] == ' ') return 0;
    /* only a bare 0 means off: gps=0.5,... is a real value */
    if (p[gl] == '0' && (p[gl + 1] == 0 || p[gl + 1] == ' ')) return 0;
    execv(argv[1], argv + 1);
    perror(argv[1]);
    return 1;
}
