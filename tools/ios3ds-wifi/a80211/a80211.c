/*
 * ios3ds-a80211.dylib: the Apple80211 "driver" surface for iOS 4's WiFiManager.
 *
 * configd's WiFiManager plugin (the Wi-Fi brain behind Settings > Wi-Fi and
 * the status bar) only works with Apple's IO80211 drivers: it looks for an
 * IO80211Interface in IOKit, checks that its parent conforms to
 * IO80211Controller, and then talks to it through the Apple80211 API, which
 * is socket ioctls SIOCGA80211/SIOCSA80211 plus an 802.11 kernel event
 * socket. Our interface is a plain IOUserEthernet en0 (ios3ds-wifid), so this
 * library is inserted into configd (DYLD_INSERT_LIBRARIES, rootfs recipe)
 * and interposes exactly those calls:
 *
 *   IOServiceMatching("IO80211Interface")       -> our IOEthernetInterface,
 *     + IOServiceAddMatchingNotification           delivered once en0 has its
 *                                                  BSD name (see below)
 *   IOObjectConformsTo(x, "IO80211Controller")  -> true for IOUserEthernetController
 *   IORegistryEntryCreateCFProperties(ctrl)     -> + the IO80211 keys it reads
 *   ioctl(SIOCGIFMEDIA, en0)                    -> IFM_IEEE80211
 *   ioctl(SIOC[GS]A80211, en0)                  -> answered here from
 *                                                  sysctl kern.ios3ds_wlan
 *   PF_SYSTEM event socket (802.11 subclass)    -> a socketpair we feed with
 *                                                  scan-done / link events
 *   SCPreferencesGetValue (logging keys)        -> IOS3DS_WIFI_LOG=1|2: WiFiManager's
 *                                                  own log (and every request) on
 *                                                  the console
 *
 * Every request type and layout comes from reverse engineering WiFiManager's
 * own Apple80211 code (docs/wifi.md section 10, re-log 52). Anything unknown is
 * logged once and answered EOPNOTSUPP, like a driver without that feature.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>

#include "wifi_proto.h"

/* ---- libSystem (stub SDK: Darwin arm32 declared here) ------------------- */
int ioctl(int, unsigned long, ...);
int socket(int, int, int);
int socketpair(int, int, int, int[2]);
int dup2(int, int);
int sysctl(int *, unsigned, void *, size_t *, void *, size_t);
int sysctlnametomib(const char *, int *, size_t *);
ssize_t send(int, const void *, size_t, int);
time_t time(time_t *);
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct sockaddr_dl { uint8_t sdl_len, sdl_family; uint16_t sdl_index; uint8_t sdl_type, sdl_nlen, sdl_alen, sdl_slen; char sdl_data[12]; };
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr, *ifa_netmask, *ifa_dstaddr;
    void *ifa_data;
};
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
#define AF_LINK         18
#define AF_UNIX         1
#define SOCK_DGRAM      2
#define PF_SYSTEM       32
#define SIOCGIFMEDIA    0xC0286938u     /* _IOWR('i', 56, struct ifmediareq) */
#define SIOCSA80211     0x802069C8u     /* _IOW('i', 200, struct apple80211req) */
#define SIOCGA80211     0xC02069C9u     /* _IOWR('i', 201, struct apple80211req) */
#define SIOCSKEVFILT    0x800C6502u     /* _IOW('e', 2, struct kev_request) */
#define EOPNOTSUPP_D    102

struct ifmediareq {
    char ifm_name[16];
    int ifm_current, ifm_mask, ifm_status, ifm_active, ifm_count;
    int *ifm_ulist;
};
#define IFM_IEEE80211   0x80
#define IFM_AUTO        0
#define IFM_AVALID      1
#define IFM_ACTIVE      2

struct apple80211req {
    char     req_if_name[16];
    int      req_type;
    int      req_val;
    uint32_t req_len;
    void    *req_data;
};

/* ---- CoreFoundation / IOKit ------------------------------------------------- */
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef unsigned int io_object_t, io_registry_entry_t, mach_port_t;
typedef int kern_return_t;
typedef unsigned int IOOptionBits;
typedef unsigned char Boolean;
extern const CFAllocatorRef kCFAllocatorDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
Boolean IOObjectConformsTo(io_object_t object, const char *className);
kern_return_t IORegistryEntryCreateCFProperties(io_registry_entry_t entry, CFMutableDictionaryRef *properties,
                                                CFAllocatorRef allocator, IOOptionBits options);
kern_return_t IORegistryEntrySetCFProperty(io_registry_entry_t entry, CFStringRef name, CFTypeRef value);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, unsigned);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
void CFRelease(CFTypeRef);
Boolean CFEqual(CFTypeRef, CFTypeRef);
typedef const struct __SCPreferences *SCPreferencesRef;
CFTypeRef SCPreferencesGetValue(SCPreferencesRef, CFStringRef);
extern const CFTypeRef kCFBooleanTrue;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef double CFAbsoluteTime, CFTimeInterval;
typedef struct IONotificationPort *IONotificationPortRef;
typedef void (*IOServiceMatchingCallback)(void *refcon, io_object_t iterator);
extern const CFStringRef kCFRunLoopCommonModes;
extern const mach_port_t kIOMasterPortDefault;
CFRunLoopRef CFRunLoopGetCurrent(void);
void CFRunLoopWakeUp(CFRunLoopRef);
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef, CFAbsoluteTime, CFTimeInterval, unsigned, long,
                                       void (*)(CFRunLoopTimerRef, void *), void *);
void CFRunLoopAddTimer(CFRunLoopRef, CFRunLoopTimerRef, CFStringRef);
CFTypeRef CFRetain(CFTypeRef);
kern_return_t IOServiceAddMatchingNotification(IONotificationPortRef, const char *, CFDictionaryRef,
                                               IOServiceMatchingCallback, void *, io_object_t *);
kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, io_object_t *);
CFMutableDictionaryRef IOBSDNameMatching(mach_port_t, uint32_t, const char *);
kern_return_t IOObjectRelease(io_object_t);
char *getenv(const char *);
#define kCFStringEncodingUTF8 0x08000100u
typedef const struct __CFURL *CFURLRef;
typedef struct __CFReadStream *CFReadStreamRef;
typedef struct __CFWriteStream *CFWriteStreamRef;
CFReadStreamRef CFReadStreamCreateWithFile(CFAllocatorRef, CFURLRef);
Boolean CFURLGetFileSystemRepresentation(CFURLRef, Boolean, uint8_t *, long);
void CFStreamCreatePairWithSocket(CFAllocatorRef, int, CFReadStreamRef *, CFWriteStreamRef *);

/* ---- logging ------------------------------------------------------------------ */
static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    int fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

/* ---- kern.ios3ds_wlan ------------------------------------------------------------ */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int mib[8];
static size_t miblen;
static wlanio_t io;                 /* last status read (under lock) */

static int wlan_read(void)
{
    if (!miblen) {
        miblen = 8;
        if (sysctlnametomib(WLAN_SYSCTL_NAME, mib, &miblen) != 0) { miblen = 0; return -1; }
    }
    for (int tries = 0; tries < 50; tries++) {
        size_t len = sizeof io;
        if (sysctl(mib, (unsigned)miblen, &io, &len, NULL, 0) == 0)
            return io.magic == WLANIO_MAGIC && io.version == WLAN_VERSION ? 0 : -1;
        if (errno != 16 /* EBUSY */) return -1;
        usleep(1000);
    }
    return -1;
}

static int wlan_post(const wlan_cmd_t *c)
{
    static wlanio_t w;
    if (!miblen && wlan_read() != 0) return -1;
    memset(&w, 0, sizeof w);
    w.magic = WLANIO_CMD;
    w.version = WLAN_VERSION;
    w.cmd = *c;
    for (int tries = 0; tries < 50; tries++) {
        if (sysctl(mib, (unsigned)miblen, NULL, NULL, &w, sizeof w) == 0) return 0;
        if (errno != 16) return -1;
        usleep(1000);
    }
    return -1;
}

/* ---- which interface is ours ---------------------------------------------------- */
static char ourif[16];

static int is_ours(const char *name)
{
    if (!ourif[0]) {
        pthread_mutex_lock(&lock);
        if (wlan_read() == 0 && (io.st.mac[0] | io.st.mac[1] | io.st.mac[2] | io.st.mac[3] |
                                 io.st.mac[4] | io.st.mac[5])) {
            struct ifaddrs *list = NULL, *i;
            if (getifaddrs(&list) == 0) {
                for (i = list; i; i = i->ifa_next) {
                    const struct sockaddr_dl *dl = (const struct sockaddr_dl *)i->ifa_addr;
                    if (dl && dl->sdl_family == AF_LINK && dl->sdl_alen == 6 &&
                        !memcmp(dl->sdl_data + dl->sdl_nlen, io.st.mac, 6)) {
                        snprintf(ourif, sizeof ourif, "%s", i->ifa_name);
                        say("[a80211] our 802.11 interface is %s", ourif);
                    }
                }
                freeifaddrs(list);
            }
        }
        pthread_mutex_unlock(&lock);
    }
    return ourif[0] && !strncmp(name, ourif, 16);
}

/* ---- events: the 802.11 kernel event socket ---------------------------------------- */
/* WiFiManager opens socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT) and filters
   {vendor 1, class 6 (IEEE 802.11), subclass 1}. We swap such a socket for
   one end of a datagram socketpair and post kern_event_msg records:
   total_size, vendor, class, subclass, id, event_code, ifname[16] (40 bytes,
   no payload: WiFiManager treats a payload as a CF object). */
#define EV_MAX 8
static int ev_wr[EV_MAX];
static uint32_t ev_id;
enum { EV_POWER = 1, EV_SSID = 2, EV_BSSID = 3, EV_LINK = 4, EV_ASSOC_DONE = 9, EV_SCAN_DONE = 10 };

/* iOS 3.x's Apple80211 has no PF_SYSTEM socket: Apple80211EventMonitoringInit
   asks the driver for a device name (GET 23), opens "/dev/<name>" with
   CFReadStreamCreateWithFile and reads one record per callback,
   {u32 event, u32 payload length, payload}; a read that does not return
   exactly 8 + length bytes is dropped. WiFiManager's CFReadStreamCreateWithFile
   for our name is answered with the read end of a datagram socketpair, so each
   record arrives as one read. */
#define EV3_NAME "ios3ds-a80211ev"
static int ev3_wr[EV_MAX];

static void post_event(uint32_t code)
{
    uint32_t r3[2] = { code, 0 };
    for (int i = 0; i < EV_MAX; i++)
        if (ev3_wr[i] > 0 && send(ev3_wr[i], r3, sizeof r3, 0) < 0 && errno != 35 /* EAGAIN */) {
            close(ev3_wr[i]);
            ev3_wr[i] = 0;
        }
    uint32_t m[10];
    memset(m, 0, sizeof m);
    m[0] = 40; m[1] = 1; m[2] = 6; m[3] = 1; m[4] = ++ev_id; m[5] = code;
    memcpy(&m[6], ourif, 16);
    for (int i = 0; i < EV_MAX; i++)
        if (ev_wr[i] > 0 && send(ev_wr[i], m, sizeof m, 0) < 0 && errno != 35 /* EAGAIN */) {
            close(ev_wr[i]);
            ev_wr[i] = 0;
        }
}

static int take_event_socket(int fd)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return -1;
    if (dup2(sv[0], fd) < 0) { close(sv[0]); close(sv[1]); return -1; }
    close(sv[0]);
    fcntl(sv[1], 4 /* F_SETFL */, 4 /* O_NONBLOCK */);
    for (int i = 0; i < EV_MAX; i++)
        if (ev_wr[i] <= 0) { ev_wr[i] = sv[1]; return 0; }
    close(ev_wr[0]);                /* oldest first */
    memmove(ev_wr, ev_wr + 1, sizeof(ev_wr) - sizeof(ev_wr[0]));
    ev_wr[EV_MAX - 1] = sv[1];
    return 0;
}

/* ---- the "driver" state ---------------------------------------------------------- */
static int power = 1;               /* soft radio switch (Settings > Wi-Fi on/off) */
static unsigned scan_cursor;        /* next GET 11 entry */
static int scan_wanted;             /* SET 10 posted, waiting for a new scan_seq */
static uint32_t scan_seq_at_req;
static time_t scan_since;
static int assoc_result;            /* GET 21: 0 pending, 1 ok, 2.. failed */
static int assoc_pending;
static time_t assoc_since;
static uint8_t assoc_ssid[32], assoc_ssid_len;
static uint32_t assoc_seq;          /* command sequence number of the join */
static uint32_t assoc_link_seq;     /* link_seq when it was posted */
static int assoc_was_up;

static int up(void) { return io.st.state == WLAN_ST_UP; }

static uint32_t chan_of(uint32_t mhz)
{
    return mhz == 2484 ? 14 : (mhz >= 2412 && mhz <= 2472) ? (mhz - 2407) / 5 : (mhz > 5000 ? (mhz - 5000) / 5 : 0);
}

/* struct scan_result (0x8C, WiFiManager's parser at 0x23628) from one BSS */
static void fill_result(uint8_t *r, uint32_t cap, const wlan_bss_t *b)
{
    /* iOS 4: 140 bytes, u16 IE length at 0x86, IE pointer at 0x88.
       iOS 3.x: 144 bytes, u16 IE length at 0x88, IE pointer at 0x8C
       (3.1.3 WiFiManager+1f174); the fields before 0x86 are the same. */
    uint32_t len_off = cap >= 0x90 ? 0x88 : 0x86, ptr_off = cap >= 0x90 ? 0x8C : 0x88;
    uint8_t *ie = *(uint8_t **)(r + ptr_off);
    uint16_t ie_cap = *(uint16_t *)(r + len_off);
    memset(r, 0, ptr_off);
    *(uint32_t *)(r + 0x04) = 1;
    *(uint32_t *)(r + 0x08) = chan_of(b->channel_mhz);
    *(uint32_t *)(r + 0x0C) = 2 | 8;                /* 2.4 GHz, 20 MHz */
    *(int16_t *)(r + 0x10) = b->noise;
    *(int16_t *)(r + 0x12) = b->rssi;
    *(int16_t *)(r + 0x14) = (int16_t)b->beacon_int;
    *(uint16_t *)(r + 0x16) = b->caps;
    memcpy(r + 0x18, b->bssid, 6);
    static const uint32_t rates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    r[0x1E] = 12;
    for (int i = 0; i < 12; i++) *(uint32_t *)(r + 0x20 + 4 * i) = rates[i] / 2;
    r[0x5C] = b->ssid_len;
    memcpy(r + 0x5D, b->ssid, b->ssid_len);
    uint16_t n = b->ie_len;
    if (cap < ptr_off + 4 || !ie) n = 0;
    if (n > ie_cap) n = ie_cap;
    /* WiFiManager on iOS 4 cannot handle the SAE AKM in a WPA2+WPA3 RSN IE;
       show it a WPA2-only view. The ARM9 still sees the original IE and
       joins it as WPA2-PSK (docs/wifi.md). */
    if (n) {
        memcpy(ie, b->ie, n);
        n = (uint16_t)wlan_ie_wpa2_view(ie, n);
    }
    *(uint16_t *)(r + len_off) = n;
}

static const wlan_bss_t *current_bss(void)
{
    static wlan_bss_t cur;
    for (unsigned i = 0; i < io.st.nscan && i < WLAN_SCAN_MAX; i++)
        if (!memcmp(io.bss[i].bssid, io.st.bssid, 6)) return &io.bss[i];
    memset(&cur, 0, sizeof cur);        /* not in the last scan: what we know */
    memcpy(cur.bssid, io.st.bssid, 6);
    cur.ssid_len = io.st.ssid_len;
    memcpy(cur.ssid, io.st.ssid, 32);
    cur.channel_mhz = io.st.channel_mhz;
    cur.rssi = io.st.rssi;
    cur.noise = io.st.noise;
    cur.caps = 0x0401 | (io.st.security ? 0x10 : 0);
    cur.beacon_int = 100;
    return &cur;
}

static uint8_t seen[256];           /* request types already logged */
static int wm_log;                  /* IOS3DS_WIFI_LOG: 1 WiFiManager's log, 2 + every request */

static int fail(int e) { errno = e; return -1; }

static int a80211_get(struct apple80211req *r)
{
    uint8_t *d = r->req_data;
    uint32_t len = r->req_len;
    int t = r->req_type;
    if (wlan_read() != 0) return fail(6 /* ENXIO */);
    switch (t) {
    case 12:                        /* CARD_CAPABILITIES: bitmap (len 4), anything (len 100) */
        if (!d || !len) return fail(22);
        memset(d, 0, len);
        /* TKIP (1), AES-CCM (3), WPA2 (14): what the ARM9 supplicant does.
           3.1.3's WiFiManager and IPConfiguration also ask with len 3 (the
           bitmap as bytes), so write it bytewise. */
        {
            uint32_t caps = 1u << 1 | 1u << 3 | 1u << 14;
            for (uint32_t i = 0; i < len && i < 4; i++) d[i] = (uint8_t)(caps >> (8 * i));
        }
        return 0;
    case 19:                        /* POWER {version, num_radios, state[4]} */
        if (!d || len < 12) return fail(22);
        memset(d, 0, len);
        ((uint32_t *)d)[0] = 1;
        ((uint32_t *)d)[1] = 1;
        ((uint32_t *)d)[2] = power && io.st.state != WLAN_ST_OFF;
        return 0;
    case 13:                        /* STATE: INIT 0, SCAN 1, AUTH 2, ASSOC 3, RUN 4 */
        r->req_val = up() ? 4 : io.st.state == WLAN_ST_JOINING ? 2 : 0;
        return 0;
    case 15:                        /* OP_MODE: station */
        r->req_val = 1;
        return 0;
    case 8:                         /* RATE (Mbit/s) */
        if (!up()) return fail(6);
        r->req_val = 54;
        return 0;
    case 5:                         /* POWERSAVE: off */
        r->req_val = 0;
        return 0;
    case 14:                        /* PHY_MODE {version, supported, active}: 11b|11g, 11g */
        if (!d || len < 12) return fail(22);
        ((uint32_t *)d)[0] = 1; ((uint32_t *)d)[1] = 2 | 4; ((uint32_t *)d)[2] = 4;
        return 0;
    case 1:                         /* SSID: raw bytes, req_len = length */
        if (!up() || !d) return fail(6);
        if (len > io.st.ssid_len) len = io.st.ssid_len;
        memcpy(d, io.st.ssid, len);
        r->req_len = len;
        return 0;
    case 9:                         /* BSSID: 6 raw bytes */
        if (!up() || !d || len < 6) return fail(6);
        memcpy(d, io.st.bssid, 6);
        return 0;
    case 4:                         /* CHANNEL {version, ch_version, channel, flags} */
        if (!up() || !d || len < 16) return fail(6);
        ((uint32_t *)d)[0] = 1; ((uint32_t *)d)[1] = 1;
        ((uint32_t *)d)[2] = chan_of(io.st.channel_mhz); ((uint32_t *)d)[3] = 2 | 8;
        return 0;
    case 16: case 17: {             /* RSSI / NOISE: {version, radios, unit, ctl[4], ctl_agr, ...} */
        if (!up() || !d || len < 0x20) return fail(6);
        int v = t == 16 ? io.st.rssi : io.st.noise;
        memset(d, 0, len);
        ((uint32_t *)d)[0] = 1; ((uint32_t *)d)[1] = 1; ((uint32_t *)d)[2] = 0;   /* dBm */
        ((int32_t *)d)[3] = v;
        ((int32_t *)d)[7] = v;
        return 0;
    }
    case 2:                         /* AUTH_TYPE {version, lower, upper} */
        if (!up() || !d || len < 12) return fail(6);
        ((uint32_t *)d)[0] = 1; ((uint32_t *)d)[1] = 1; ((uint32_t *)d)[2] = io.st.security ? 8 : 0;
        return 0;
    case 21:                        /* ASSOCIATE_RESULT */
        r->req_val = assoc_result;
        return 0;
    case 27: {                      /* SUPPORTED_CHANNELS {version, num, {ver, ch, flags}[64]} */
        if (!d || len < 8 + 13 * 12) return fail(22);
        memset(d, 0, len);
        uint32_t *u = (uint32_t *)d;
        u[0] = 1; u[1] = 13;
        for (int i = 0; i < 13; i++) { u[2 + 3 * i] = 1; u[3 + 3 * i] = i + 1; u[4 + 3 * i] = 2 | 8; }
        return 0;
    }
    case 29:                        /* DEAUTH {version, reason, ea[6]} */
        if (!d || len < 16) return fail(22);
        memset(d, 0, len);
        ((uint32_t *)d)[1] = io.st.disc_code;
        return 0;
    case 32: {                      /* RATE_SET {version, u16 count, {ver, rate, flags}[15]} */
        static const uint32_t rates[] = { 1, 2, 5, 6, 9, 11, 12, 18, 24, 36, 48, 54 };
        if (!d || len < 8 + 12 * 12) return fail(22);
        memset(d, 0, len);
        *(uint32_t *)d = 1;
        *(uint16_t *)(d + 4) = 12;
        for (int i = 0; i < 12; i++) ((uint32_t *)(d + 8))[3 * i + 1] = rates[i];
        return 0;
    }
    case 43: case 44:               /* DRIVER / HARDWARE version: a raw string */
        if (!d || !len) return fail(22);
        snprintf((char *)d, len, "%s", t == 43 ? "ios3ds-a80211 1 (ARM9 AR6014 driver)" : "Atheros AR6014");
        return 0;
    case 11:                        /* SCAN_RESULT: one per call, EIO at the end */
        if (!d || len < 0x8C) return fail(22);
        if (scan_cursor >= io.st.nscan || scan_cursor >= WLAN_SCAN_MAX) return fail(5 /* EIO */);
        fill_result(d, len, &io.bss[scan_cursor++]);
        return 0;
    case 201:                       /* current network as a scan_result */
        if (!up() || !d || len < 0x8C) return fail(6);
        fill_result(d, len, current_bss());
        return 0;
    case 23:                        /* 3.x: event device name, opened as /dev/<name> */
        if (!d || len < sizeof EV3_NAME) return fail(22);
        memcpy(d, EV3_NAME, sizeof EV3_NAME);
        return 0;
    case 204:                       /* RSSI bounds {version, min, max, threshold} */
        if (!d || len < 16) return fail(22);
        ((int32_t *)d)[0] = 1; ((int32_t *)d)[1] = -95; ((int32_t *)d)[2] = -30; ((int32_t *)d)[3] = -85;
        return 0;
    }
    return fail(EOPNOTSUPP_D);
}

static int a80211_set(struct apple80211req *r)
{
    uint8_t *d = r->req_data;
    wlan_cmd_t c;
    memset(&c, 0, sizeof c);
    if (wlan_read() != 0) return fail(6);
    switch (r->req_type) {
    case 66:                        /* 3.x: event mask for the event device: all welcome */
        return 0;
    case 10:                        /* SCAN_REQ */
        if (!power || io.st.state == WLAN_ST_OFF) return fail(6);
        if (io.st.state == WLAN_ST_STARTING) return fail(16);   /* client retries EBUSY */
        c.cmd = WLAN_CMD_SCAN;
        scan_seq_at_req = io.st.scan_seq;
        scan_wanted = 1;
        scan_since = time(NULL);
        scan_cursor = 0;
        say("[a80211] scan requested");
        return wlan_post(&c) == 0 ? 0 : fail(6);
    case 20: {                      /* ASSOCIATE (0x180) */
        if (!d || r->req_len < 0x7C) return fail(22);
        uint32_t sl = *(uint32_t *)(d + 0x0C), klen = *(uint32_t *)(d + 0x3C), cipher = *(uint32_t *)(d + 0x40);
        if (!sl || sl > 32) return fail(22);
        c.cmd = WLAN_CMD_JOIN;
        c.ssid_len = (uint8_t)sl;
        memcpy(c.ssid, d + 0x10, sl);
        if (klen == 32 && cipher == 6) {    /* APPLE80211_CIPHER_PMK: WPA(2)-PSK */
            c.security = WLAN_SEC_WPA2;
            memcpy(c.pmk, d + 0x48, 32);
        } else if (klen) {
            say("[a80211] associate: key type %u / %u bytes not supported (WEP?)", cipher, klen);
            return fail(EOPNOTSUPP_D);
        }
        say("[a80211] associate \"%.*s\": req_len %u, ssid_len %u, key_len %u, cipher %u, auth %u/%u, word44 %u",
            (int)sl, (const char *)d + 0x10, r->req_len, sl, klen, cipher,
            *(uint16_t *)(d + 6), *(uint16_t *)(d + 8), *(uint32_t *)(d + 0x44));
        assoc_ssid_len = (uint8_t)sl;
        memcpy(assoc_ssid, d + 0x10, sl);
        assoc_result = 0;
        assoc_pending = 1;
        assoc_since = time(NULL);
        assoc_seq = io.cmd_seq + 1;         /* the shim numbers commands in order */
        assoc_link_seq = io.st.link_seq;
        assoc_was_up = io.st.state == WLAN_ST_UP;
        say("[a80211] associate \"%.*s\" (%s, auth %u/%u)", (int)sl, (const char *)d + 0x10,
            c.security ? "WPA2-PSK, PMK from WiFiManager" : "open", *(uint16_t *)(d + 6), *(uint16_t *)(d + 8));
        return wlan_post(&c) == 0 ? 0 : fail(6);
    }
    case 22:                        /* DISASSOCIATE */
        c.cmd = WLAN_CMD_LEAVE;
        assoc_pending = 0;
        say("[a80211] disassociate");
        return wlan_post(&c) == 0 ? 0 : fail(6);
    case 19: {                      /* POWER {version, num, state[]} */
        int on = d && r->req_len >= 12 && ((uint32_t *)d)[2];
        if (on != power) {
            power = on;
            say("[a80211] radio %s", on ? "on" : "off");
            if (!on) {
                c.cmd = WLAN_CMD_LEAVE;
                wlan_post(&c);
            }
            post_event(EV_POWER);
        }
        return 0;
    }
    }
    return 0;                       /* other settings: accepted, nothing to do */
}

typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
int dladdr(const void *, Dl_info *);
static int a80211_ioctl(int set, struct apple80211req *r)
{
    int t = r->req_type;
    pthread_mutex_lock(&lock);
    int rc = set ? a80211_set(r) : a80211_get(r);
    int e = errno;
    pthread_mutex_unlock(&lock);
    if (wm_log > 1) {
        void **fp = __builtin_frame_address(0);
        Dl_info di;
        void *lr = fp && fp[0] ? ((void **)fp[0])[1] : 0;     /* ioctl's caller */
        const char *b = lr && dladdr(lr, &di) && di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        say("[a80211] %s %d len %u -> %d/%d (%s+%lx)", set ? "SET" : "GET", t, r->req_len, rc, rc ? e : 0,
            b ? b + 1 : "?", b ? (unsigned long)((char *)lr - (char *)di.dli_fbase) : 0ul);
    }
    if (t >= 0 && t < 256 && !(seen[t] & (set ? 2 : 1))) {
        seen[t] |= set ? 2 : 1;
        say("[a80211] first %s type %d (len %u): %s", set ? "SET" : "GET", t, r->req_len,
            rc == 0 ? "ok" : e == EOPNOTSUPP_D ? "unsupported" : "error");
    }
    errno = e;
    return rc;
}

/* ---- the interface notification -------------------------------------------------
   WiFiManager arms an IOServiceFirstMatch notification for IO80211Interface and,
   for each match, reads the interface's "BSD Name" (31 tries, 10 ms apart) or
   gives up for good ("Could not obtain IO80211Interface bsd name"). Our
   interface is published by ios3ds-wifid long before configd's InterfaceNamer
   names it (20+ s, it waits for IOKit to go quiet), so the real notification
   would always come too early. Instead the client gets an iterator that
   matches nothing, and once en0 exists the watcher thread schedules a timer
   on the run loop that client's notification port is scheduled on, which
   hands its callback an iterator with the named interface
   (IOBSDNameMatching). More than one client in configd asks (the order
   differs from boot to boot), so every registration is remembered. */
#define NOTE_MAX 6
static struct note {
    IONotificationPortRef port;
    IOServiceMatchingCallback cb;
    void *refcon;
    CFRunLoopRef rl;                /* from CFRunLoopAddSource(port's source) */
    CFRunLoopRef cur;               /* the registering thread's, if rl never shows up */
    int sent;
} notes[NOTE_MAX];
static CFMutableDictionaryRef iface_match[NOTE_MAX];   /* dicts we returned for IO80211Interface */
static unsigned iface_match_n;

CFTypeRef IONotificationPortGetRunLoopSource(IONotificationPortRef);
void IONotificationPortDestroy(IONotificationPortRef);
void CFRunLoopAddSource(CFRunLoopRef, CFTypeRef, CFStringRef);

static void note_fire(CFRunLoopTimerRef t, void *info)
{
    struct note n;
    pthread_mutex_lock(&lock);
    n = notes[(uintptr_t)info];
    pthread_mutex_unlock(&lock);
    if (!n.cb) return;
    io_object_t it = 0;
    CFMutableDictionaryRef m = IOBSDNameMatching(kIOMasterPortDefault, 0, ourif);
    if (!m || IOServiceGetMatchingServices(kIOMasterPortDefault, m, &it) != 0 || !it) {
        say("[a80211] no IOKit service for %s", ourif);
        return;
    }
    say("[a80211] handing %s to an IO80211Interface client (refcon %p)", ourif, n.refcon);
    n.cb(n.refcon, it);
    IOObjectRelease(it);
}

static void notes_schedule(void)      /* under lock */
{
    if (!ourif[0]) return;
    for (uintptr_t i = 0; i < NOTE_MAX; i++) {
        struct note *n = &notes[i];
        if (!n->cb || n->sent) continue;
        if (!n->rl) n->rl = n->cur, n->cur = NULL;
        if (!n->rl) continue;
        n->sent = 1;
        CFRunLoopTimerRef t = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0, 0, 0,
                                                   note_fire, (void *)i);
        CFRunLoopAddTimer(n->rl, t, kCFRunLoopCommonModes);
        CFRelease(t);
        CFRunLoopWakeUp(n->rl);
    }
}

static int is_iface_match(CFDictionaryRef m)
{
    for (unsigned i = 0; i < iface_match_n; i++)
        if (iface_match[i] == m) {
            iface_match[i] = iface_match[--iface_match_n];
            return 1;
        }
    return 0;
}

static kern_return_t my_IOServiceAddMatchingNotification(IONotificationPortRef port, const char *type,
                                                         CFDictionaryRef match, IOServiceMatchingCallback cb,
                                                         void *refcon, io_object_t *it)
{
    pthread_mutex_lock(&lock);
    int ours = match && is_iface_match(match);
    pthread_mutex_unlock(&lock);
    if (!ours)
        return IOServiceAddMatchingNotification(port, type, match, cb, refcon, it);
    kern_return_t r = IOServiceAddMatchingNotification(port, type, IOServiceMatching("IOS3DSNoSuchClass"),
                                                       cb, refcon, it);
    CFRelease(match);               /* the call consumes one reference */
    if (r != 0) return r;
    pthread_mutex_lock(&lock);
    struct note *n = NULL;
    for (int i = 0; i < NOTE_MAX && !n; i++)
        if (notes[i].port == port || !notes[i].cb) n = &notes[i];
    if (n) {
        if (n->rl && n->port != port) { CFRelease(n->rl); n->rl = NULL; }
        if (n->cur) CFRelease(n->cur);
        n->cur = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
        n->port = port;
        n->cb = cb;
        n->refcon = refcon;
        n->sent = 0;
        /* a port already on a run loop: that run loop; otherwise CFRunLoopAddSource tells */
    }
    pthread_mutex_unlock(&lock);
    say("[a80211] %s for IO80211Interface (refcon %p): deferred until the interface has a BSD name",
        type, refcon);
    return r;
}

static void my_CFRunLoopAddSource(CFRunLoopRef rl, CFTypeRef src, CFStringRef mode)
{
    CFRunLoopAddSource(rl, src, mode);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NOTE_MAX; i++)
        if (notes[i].cb && !notes[i].rl && src == IONotificationPortGetRunLoopSource(notes[i].port))
            notes[i].rl = (CFRunLoopRef)CFRetain(rl);
    pthread_mutex_unlock(&lock);
}

static void my_IONotificationPortDestroy(IONotificationPortRef port)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NOTE_MAX; i++)
        if (notes[i].port == port) {
            if (notes[i].rl) CFRelease(notes[i].rl);
            if (notes[i].cur) CFRelease(notes[i].cur);
            memset(&notes[i], 0, sizeof notes[i]);
        }
    pthread_mutex_unlock(&lock);
    IONotificationPortDestroy(port);
}

/* ---- display state -----------------------------------------------------------------
   WiFiManager keeps auto-join off while notify state
   com.apple.iokit.hid.displayStatus says the display is off ("Device display
   is OFF", "Disabling automatic association"). The HID event system posts it
   on a real iPod; on ios3ds nobody does, so it read "off" forever and a known
   network was only joined with Settings open. The watcher mirrors
   SpringBoard's com.apple.springboard.hasBlankedScreen into it (on unless
   blanked) and posts the change. */
uint32_t notify_register_check(const char *name, int *token);
uint32_t notify_get_state(int token, uint64_t *state);
uint32_t notify_set_state(int token, uint64_t state);
uint32_t notify_post(const char *name);
#define DISPLAY_NOTE "com.apple.iokit.hid.displayStatus"

static void display_mirror(void)
{
    static int blank_tok = -1, disp_tok = -1;
    static uint64_t last = 2;
    uint64_t blanked = 0, on;
    if (blank_tok < 0 && notify_register_check("com.apple.springboard.hasBlankedScreen", &blank_tok)) blank_tok = -1;
    if (disp_tok < 0 && notify_register_check(DISPLAY_NOTE, &disp_tok)) disp_tok = -1;
    if (blank_tok < 0 || disp_tok < 0 || notify_get_state(blank_tok, &blanked)) return;
    on = !blanked;
    if (on == last) return;
    last = on;
    notify_set_state(disp_tok, on);
    notify_post(DISPLAY_NOTE);
    say("[a80211] display %s (SpringBoard) -> %s", on ? "on" : "blanked", DISPLAY_NOTE);
}

/* status poller: turns ARM9 state changes into 802.11 events */
static void *watcher(void *arg)
{
    uint32_t link_seq = 0, state = 0;
    int first = 1;
    for (;;) {
        usleep(250000);
        display_mirror();
        if (!ourif[0]) { is_ours(""); continue; }
        pthread_mutex_lock(&lock);
        notes_schedule();
        int ok = wlan_read() == 0;
        uint32_t ev[4], nev = 0;
        if (ok) {
            /* WiFiManager's synchronous scan waits for SCAN_DONE without a
               timeout: always send one, after at most 8 s */
            if (scan_wanted && (io.st.scan_seq != scan_seq_at_req || time(NULL) - scan_since > 8)) {
                if (io.st.scan_seq == scan_seq_at_req)
                    say("[a80211] scan timed out (ARM9 state %u): reporting the old list", io.st.state);
                else
                    say("[a80211] scan done: %u networks", io.st.nscan);
                scan_wanted = 0;
                scan_cursor = 0;
                ev[nev++] = EV_SCAN_DONE;
            }
            if (assoc_pending) {
                /* judge only what the ARM9 did after it took the join command
                   (WiFiManager sends a disassociate first: the status still
                   says idle, or up on the old network, for a poll or two) */
                int acked = (int32_t)(io.st.ack_seq - assoc_seq) >= 0;
                int same = io.st.ssid_len == assoc_ssid_len && !memcmp(io.st.ssid, assoc_ssid, assoc_ssid_len);
                if (acked && io.st.state == WLAN_ST_UP && same && (!assoc_was_up || io.st.link_seq != assoc_link_seq)) {
                    assoc_result = 1;
                } else if ((acked && (io.st.state == WLAN_ST_IDLE || io.st.result < 0)) ||
                           time(NULL) - assoc_since > 30) {
                    assoc_result = io.st.disc_reason ? 3 : 9;   /* -3907 / timeout */
                }
                if (assoc_result) {
                    assoc_pending = 0;
                    say("[a80211] association %s (result %d)", assoc_result == 1 ? "done" : "failed", assoc_result);
                    ev[nev++] = EV_ASSOC_DONE;
                }
            }
            if (!first && (io.st.link_seq != link_seq || (io.st.state == WLAN_ST_UP) != (state == WLAN_ST_UP))) {
                ev[nev++] = EV_LINK;
                if (io.st.state == WLAN_ST_UP) { ev[nev++] = EV_SSID; ev[nev++] = EV_BSSID; }
                say("[a80211] link %s", io.st.state == WLAN_ST_UP ? "up" : "down");
            }
            if (first && io.st.state == WLAN_ST_UP) ev[nev++] = EV_LINK;
            link_seq = io.st.link_seq;
            state = io.st.state;
            first = 0;
        }
        pthread_mutex_unlock(&lock);
        for (uint32_t i = 0; i < nev; i++) post_event(ev[i]);
    }
    return NULL;
}

/* ---- interposed functions --------------------------------------------------------- */
static int my_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (req == SIOCGIFMEDIA && arg && is_ours(((struct ifmediareq *)arg)->ifm_name)) {
        struct ifmediareq *m = arg;
        m->ifm_current = m->ifm_active = IFM_IEEE80211 | IFM_AUTO;
        m->ifm_mask = 0;
        m->ifm_status = IFM_AVALID | IFM_ACTIVE;
        m->ifm_count = 0;
        return 0;
    }
    if ((req == SIOCGA80211 || req == SIOCSA80211) && arg &&
        is_ours(((struct apple80211req *)arg)->req_if_name))
        return a80211_ioctl(req == SIOCSA80211, arg);
    if (req == SIOCSKEVFILT && arg) {
        const uint32_t *k = arg;
        if (k[0] == 1 && k[1] == 6 && k[2] == 1) {     /* vendor Apple, class 802.11 */
            pthread_mutex_lock(&lock);
            int r = take_event_socket(fd);
            pthread_mutex_unlock(&lock);
            if (r == 0) return 0;
        }
    }
    return ioctl(fd, req, arg);
}

static CFMutableDictionaryRef my_IOServiceMatching(const char *name)
{
    if (name && !strcmp(name, "IO80211Interface")) {
        CFMutableDictionaryRef m = IOServiceMatching("IOEthernetInterface");
        pthread_mutex_lock(&lock);
        if (iface_match_n < NOTE_MAX) iface_match[iface_match_n++] = m;
        pthread_mutex_unlock(&lock);
        return m;
    }
    if (name && !strcmp(name, "IO80211Controller"))
        return IOServiceMatching("IOUserEthernetController");
    return IOServiceMatching(name);
}

static Boolean my_IOObjectConformsTo(io_object_t o, const char *cls)
{
    if (cls && !strcmp(cls, "IO80211Controller"))
        return IOObjectConformsTo(o, "IOUserEthernetController");
    if (cls && !strcmp(cls, "IO80211Interface"))
        return IOObjectConformsTo(o, "IOEthernetInterface");
    return IOObjectConformsTo(o, cls);
}

static kern_return_t my_IORegistryEntryCreateCFProperties(io_registry_entry_t e, CFMutableDictionaryRef *p,
                                                          CFAllocatorRef a, IOOptionBits o)
{
    kern_return_t r = IORegistryEntryCreateCFProperties(e, p, a, o);
    if (r == 0 && p && *p && IOObjectConformsTo(e, "IOUserEthernetController")) {
        static const char *const kv[][2] = {
            { "IO80211HardwareVersion", "ios3ds AR6014 (ARM9 driver)" },
            { "IO80211DriverVersion", "ios3ds-a80211 1" },
            { "IO80211CountryCode", "X0" },
            { "IO80211Locale", "ETSI" },
        };
        for (unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++) {
            CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, kv[i][0], kCFStringEncodingUTF8);
            CFStringRef v = CFStringCreateWithCString(kCFAllocatorDefault, kv[i][1], kCFStringEncodingUTF8);
            CFDictionarySetValue(*p, k, v);
            CFRelease(k);
            CFRelease(v);
        }
    }
    return r;
}

static kern_return_t my_IORegistryEntrySetCFProperty(io_registry_entry_t e, CFStringRef n, CFTypeRef v)
{
    kern_return_t r = IORegistryEntrySetCFProperty(e, n, v);
    if (r != 0 && (IOObjectConformsTo(e, "IOEthernetInterface") ||
                   IOObjectConformsTo(e, "IOUserEthernetController")))
        return 0;                   /* IO80211InterfaceIPAddress & co: accepted, unused */
    return r;
}

/* IOS3DS_WIFI_LOG=1 (or 2) in configd's environment: WiFiManager's own log
   (LoggingEnabled/LoggingFileEnabled/LoggingFile in com.apple.wifi.plist)
   goes to /dev/console */
static CFStringRef k_log, k_logfile_on, k_logfile, v_console;

static CFTypeRef my_SCPreferencesGetValue(SCPreferencesRef p, CFStringRef key)
{
    if (wm_log && key) {
        if (CFEqual(key, k_log) || CFEqual(key, k_logfile_on)) return kCFBooleanTrue;
        if (CFEqual(key, k_logfile)) return v_console;
    }
    return SCPreferencesGetValue(p, key);
}

static CFReadStreamRef my_CFReadStreamCreateWithFile(CFAllocatorRef a, CFURLRef url)
{
    uint8_t path[256];
    if (url && CFURLGetFileSystemRepresentation(url, 1, path, sizeof path) &&
        !strcmp((char *)path, "/dev/" EV3_NAME)) {
        int sv[2];
        CFReadStreamRef rs = NULL;
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return NULL;
        CFStreamCreatePairWithSocket(a, sv[0], &rs, NULL);
        if (!rs) { close(sv[0]); close(sv[1]); return NULL; }
        fcntl(sv[1], 4 /* F_SETFL */, 4 /* O_NONBLOCK */);
        pthread_mutex_lock(&lock);
        int i;
        for (i = 0; i < EV_MAX && ev3_wr[i] > 0; i++) {}
        if (i == EV_MAX) { close(ev3_wr[0]); memmove(ev3_wr, ev3_wr + 1, sizeof(ev3_wr) - sizeof(ev3_wr[0])); i = EV_MAX - 1; }
        ev3_wr[i] = sv[1];
        pthread_mutex_unlock(&lock);
        say("[a80211] 3.x event stream opened");
        return rs;
    }
    return CFReadStreamCreateWithFile(a, url);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *rep, *orig; } interpose[] = {
    { (const void *)my_ioctl, (const void *)ioctl },
    { (const void *)my_IOServiceMatching, (const void *)IOServiceMatching },
    { (const void *)my_IOServiceAddMatchingNotification, (const void *)IOServiceAddMatchingNotification },
    { (const void *)my_CFRunLoopAddSource, (const void *)CFRunLoopAddSource },
    { (const void *)my_IONotificationPortDestroy, (const void *)IONotificationPortDestroy },
    { (const void *)my_IOObjectConformsTo, (const void *)IOObjectConformsTo },
    { (const void *)my_IORegistryEntryCreateCFProperties, (const void *)IORegistryEntryCreateCFProperties },
    { (const void *)my_IORegistryEntrySetCFProperty, (const void *)IORegistryEntrySetCFProperty },
    { (const void *)my_SCPreferencesGetValue, (const void *)SCPreferencesGetValue },
    { (const void *)my_CFReadStreamCreateWithFile, (const void *)CFReadStreamCreateWithFile },
};

const char *getprogname(void);
int unsetenv(const char *);

__attribute__((constructor)) static void init(void)
{
    pthread_t t;
    const char *lg = getenv("IOS3DS_WIFI_LOG");
    if (lg && (*lg == '1' || *lg == '2')) {
        k_log = CFStringCreateWithCString(kCFAllocatorDefault, "LoggingEnabled", kCFStringEncodingUTF8);
        k_logfile_on = CFStringCreateWithCString(kCFAllocatorDefault, "LoggingFileEnabled", kCFStringEncodingUTF8);
        k_logfile = CFStringCreateWithCString(kCFAllocatorDefault, "LoggingFile", kCFStringEncodingUTF8);
        v_console = CFStringCreateWithCString(kCFAllocatorDefault, "/dev/console", kCFStringEncodingUTF8);
        wm_log = *lg - '0';
    }
    unsetenv("DYLD_INSERT_LIBRARIES");      /* configd's children get the plain system */
    if (strcmp(getprogname(), "configd")) return;
    say("[a80211] loaded into configd (pid %d)", getpid());
    pthread_create(&t, NULL, watcher, NULL);
}
