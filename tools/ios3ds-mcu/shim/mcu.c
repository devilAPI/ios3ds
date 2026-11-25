/*
 * ios3ds shim: the 3DS system controller (MCU) inside XNU. The ARM9 reads the
 * MCU (boot/arm9/mcu.c) into the MCU mailbox (boot/common/mcu_proto.h); this
 * file hands it to the kernel. See docs/mcu.md.
 *
 *   RTC     PEGetGMTTimeOfDay()      -> shim_mcu_get_gmt   (patch pe-get-gmt)
 *           IOKitInitializeTime's waitForService(IORTC) -> shim_mcu_wait_rtc
 *                                       (patch iortc-wait-mcu: skipped when
 *                                       the RTC is known, else the original
 *                                       2 s wait of iortc-wait-2s)
 *           PESetGMTTimeOfDay()      -> shim_mcu_set_gmt   (patch pe-set-gmt)
 *   halt    PEHaltRestart()'s gIOPlatform->haltRestart(type)
 *                                    -> shim_mcu_halt_restart (patch pe-halt-mcu):
 *                                       halt/restart = MCU power off by the ARM9
 *   battery an IOPMPowerSource ("ios3ds MCU battery"), created and refreshed
 *           from the sysctls below (thread context)
 *   gyro    an AppleLIS331DLH on /arm-io/i2c0/gyro whose vtable dispatches
 *           gyro events (patches gyro-usage / gyro-dispatch): CoreMotion's
 *           rotation rates from the ARM9's gyro samples (docs/mcu.md 11)
 *   sysctl  kern.ios3ds_mcu     RD, 256 bytes: the mailbox (mcubox_t)
 *           kern.ios3ds_mcuctl  RW, mcuctl_t: the daemon's requests
 *                                       (accelerometer on/off, LCD backlight)
 *
 * Kernel facts (iPod2,1 4.2.1 8C148; addresses from the kernelcache symbol
 * table, disassembly in docs/re-log.md section 30):
 *   PEGetGMTTimeOfDay 0x801c84d8, PESetGMTTimeOfDay 0x801c84f4,
 *   PEHaltRestart 0x801c98b0 (platform call at 0x801c98f6, vtable +0x388),
 *   IOKitInitializeTime 0x801a8004 (waitForService literal at 0x801a8030),
 *   gIOPlatform 0x8029d490 (vtable +0x390 getGMTTimeOfDay),
 *   IOPMPowerSource::powerSource 0x801bb624, IOService::attach 0x801adc54,
 *   IOService::registerService 0x801b0844, IOService::getPlatform 0x801ac744,
 *   IOService::getResourceService 0x801ac75c, ml_io_map 0x80067780.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "mcu_proto.h"
#include "backlight.h"
#include "accel_axes.h"
#include "kpi.h"

/* ---- kernel KPIs (Thumb unless noted) ---------------------------------- */
#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define IOLog                   KPT(IOS3DS_KPI_IOLOG, void, const char *, ...)
#define ml_io_map               KPT(IOS3DS_KPI_ML_IO_MAP, u32, u32, u32)
#define waitForService          KPT(IOS3DS_KPI_IO_WAITFORSERVICE, void *, void *, void *)
#define getPlatform             KPT(IOS3DS_KPI_IO_GETPLATFORM, void *, void)
#define getResourceService      KPT(IOS3DS_KPI_IO_GETRESOURCESERVICE, void *, void)
#define svc_attach              KPT(IOS3DS_KPI_IO_ATTACH, int, void *, void *)
#define svc_registerService     KPT(IOS3DS_KPI_IO_REGISTERSERVICE, void, void *, u32)
#define ps_powerSource          KPT(IOS3DS_KPI_PM_POWERSOURCE, void *, void)
#define ps_setMaxCapacity       KPT(IOS3DS_KPI_PM_SETMAXCAPACITY, void, void *, u32)
#define ps_setCurrentCapacity   KPT(IOS3DS_KPI_PM_SETCURRENTCAPACITY, void, void *, u32)
#define ps_setIsCharging        KPT(IOS3DS_KPI_PM_SETISCHARGING, void, void *, int)
#define ps_setExternalConnected KPT(IOS3DS_KPI_PM_SETEXTERNALCONNECTED, void, void *, int)
#define ps_setExternalChargeCapable KPT(IOS3DS_KPI_PM_SETEXTERNALCHARGE_CAPABLE, void, void *, int)
#define ps_setBatteryInstalled  KPT(IOS3DS_KPI_PM_SETBATTERYINSTALLED, void, void *, int)
#define ps_setVoltage           KPT(IOS3DS_KPI_PM_SETVOLTAGE, void, void *, u32)
#define ps_setPSProperty        KPT(IOS3DS_KPI_PM_SETPSProperty, void, void *, void *, void *)
#define ps_updateStatus         KPT(IOS3DS_KPI_PM_UPDATESTATUS, void, void *)
#define sym_withCStringNoCopy   KPT(IOS3DS_KPI_OSSYMBOL_WITHCSTRINGNOCOPY, void *, const char *)
#define num_withNumber          KPT(IOS3DS_KPI_OSNUMBER_WITHNUMBER, void *, u64, u32)
#define kOSBooleanTrue          (**(void ***)IOS3DS_KPI_KOSBOOLEAN_TRUE)   /* a C++ reference: &gOSBooleanTrue */
#define kOSBooleanFalse         (**(void ***)IOS3DS_KPI_KOSBOOLEAN_FALSE)
#define gIOPlatform             (*(void **)IOS3DS_KPI_GIO_PLATFORM)
#define data_bytes              KPT(IOS3DS_KPI_OSDATA_GETBYTESNOCOPY, const u8 *, void *)   /* OSData::getBytesNoCopy() */
#define data_length             KPT(IOS3DS_KPI_OSDATA_GETLENGTH, u32, void *)          /* OSData::getLength() */

struct sysctl_oid {
    void       *oid_parent;
    void       *oid_link;
    int         oid_number;
    int         oid_kind;
    void       *oid_arg1;
    int         oid_arg2;
    const char *oid_name;
    int       (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};
#define CTLFLAG_RD      0x80000000
#define CTLFLAG_WR      0x40000000
#define CTLTYPE_OPAQUE  5
#define OID_AUTO        (-1)
#define sysctl_register_oid   KPT(IOS3DS_KPI_SYSCTL_REGISTER_OID, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPT(IOS3DS_KPI_SYSCTL_HANDLE_OPAQUE, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_KERN_CHILDREN  ((void *)IOS3DS_KPI_SYSCTL__KERN_CHILDREN)

#define BOX ((volatile mcubox_t *)(SHIM_VA + SHIM_MCU_OFF))

void shim_l2_inv_kva(const volatile void *va, u32 len);    /* kernel/shim/entry.S: New 3DS L2 (docs/n3ds.md) */
static void dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    shim_l2_inv_kva(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static void dclean(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* the ARM9 status lines (0-3, 5) of the mailbox, fresh from RAM */
static int mcu_live(void)
{
    volatile mcubox_t *b = BOX;
    dinval(b, 128);
    dinval(&b->ack_seq, 32);
    return b->magic == MCU_MAGIC && b->version == MCU_VERSION && b->state == MCU_ST_OK;
}

static int rtc_known(void)
{
    return mcu_live() && (BOX->valid & MCUV_RTC) && BOX->rtc_utc;
}

/* ---- RTC ------------------------------------------------------------------ */

static int32_t user_delta;          /* settimeofday() minus the RTC, until reboot */
static int gmt_logged;

/* PEGetGMTTimeOfDay(): seconds since 1970 (UTC). Without an MCU RTC: the
   original body (gIOPlatform->getGMTTimeOfDay(), 0 without a platform). */
long shim_mcu_get_gmt(void)
{
    if (rtc_known()) {
        long t = (long)(BOX->rtc_utc + (u32)user_delta);
        if (!gmt_logged++)
            IOLog("ios3ds-mcu: calendar from the 3DS RTC: %ld (20%02x-%02x-%02x %02x:%02x:%02x RTC, offset %d s)\n",
                  t, BOX->rtc_raw[6], BOX->rtc_raw[5], BOX->rtc_raw[4], BOX->rtc_raw[2], BOX->rtc_raw[1],
                  BOX->rtc_raw[0], (int)BOX->rtc_offset);
        return t;
    }
    if (!gmt_logged++)
        IOLog("ios3ds-mcu: no 3DS RTC (MCU state %u): calendar from the platform expert as before\n",
              (unsigned)BOX->state);
    void *pe = gIOPlatform;
    if (!pe) return 0;
    return ((long (*)(void *))(*(void ***)pe)[(0x390 + IOS3DS_KPI_VT_BIAS) / 4])(pe);
}

/* PESetGMTTimeOfDay(secs): iOS set the clock (Settings, settimeofday). The
   3DS RTC is the console's own clock and is left alone; the difference is
   kept for later PEGetGMTTimeOfDay() calls (wake) until the next boot.
   AppleARMPE's version would block for 30 s waiting for an IORTC. */
void shim_mcu_set_gmt(long secs)
{
    if (rtc_known()) {
        user_delta = (int32_t)((u32)secs - BOX->rtc_utc);
        IOLog("ios3ds-mcu: iOS set the time to %ld (RTC %d s); the 3DS RTC is not written, "
              "the ARM9 saves the offset to sd:/ios3ds/rtc_offset.txt\n", secs, (int)user_delta);
        BOX->user_delta = user_delta;       /* hardware run14: the raw 3DS RTC said 2001 */
        BOX->user_delta_seq = BOX->user_delta_seq + 1;
        BOX->req_seq = BOX->req_seq + 1;
        dclean(&BOX->req_seq, 32);
    } else {
        IOLog("ios3ds-mcu: iOS set the time to %ld; no RTC to keep it\n", secs);
    }
}

/* IOKitInitializeTime(): waitForService(resourceMatching("IORTC"), {2 s}) runs
   right before clock_initialize_calendar(). Nothing publishes IORTC on the 3DS;
   with the MCU RTC the calendar does not depend on the platform expert any
   more, so the wait is skipped (the matching dictionary is released like
   waitForService would). Without it: the original call (iortc-wait-2s). */
void *shim_mcu_wait_rtc(void *matching, void *timeout)
{
    if (rtc_known()) {
        if (matching) ((void (*)(void *))(*(void ***)matching)[(0x14 + IOS3DS_KPI_VT_BIAS) / 4])(matching);  /* ->release() */
        IOLog("ios3ds-mcu: 3DS RTC present: not waiting for IORTC\n");
        return 0;
    }
    return waitForService(matching, timeout);
}

/* ---- halt / restart ------------------------------------------------------- */

/* PEHaltRestart(type), after IOKit's shutdown handlers ran and the file
   systems were unmounted; r0 = gIOPlatform, r1 = type. kPEHaltCPU 0,
   kPERestartCPU 1, kPEUPSDelayHaltCPU 3: the ARM9 powers the console off
   through the MCU (a reboot would land in Luma/the HOME Menu, not iOS).
   Anything else (panic restart, hang, sync) goes to the platform as before. */
int shim_mcu_halt_restart(void *pe, unsigned type)
{
    volatile mcubox_t *b = BOX;
    if ((type == 0 || type == 1 || type == 3) && mcu_live()) {
        IOLog("ios3ds-mcu: %s: asking the ARM9 to power off through the MCU\n",
              type == 1 ? "restart" : "halt");
        b->req_power = type == 1 ? MCU_PWR_REBOOT : MCU_PWR_OFF;
        b->req_seq = b->req_seq + 1;
        dclean(&b->req_seq, 32);
        /* the ARM9 acknowledges, syncs its log and cuts power; this CPU waits.
           ~2-3 s of spinning without an answer: fall back to the platform. */
        for (u32 i = 0; i < 20000000u; i++) {
            dinval(&b->ack_seq, 32);
            if (b->ack_seq == b->req_seq) {
                for (;;) __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));   /* wait for power-off */
            }
        }
        IOLog("ios3ds-mcu: no answer from the ARM9; platform halt\n");
    }
    if (!pe) return -1;
    return ((int (*)(void *, unsigned))(*(void ***)pe)[(0x388 + IOS3DS_KPI_VT_BIAS) / 4])(pe, type);
}

/* ---- battery: an IOPMPowerSource ------------------------------------------ */

static void *ps;
static u32 ps_seen = 0xFFFFFFFFu;
static void *sym_fully, *sym_rawv;

#if IOS3DS_KPI_MCU_PS_RETAIN
#define PS_RC(o) ((int (*)(void *))(*(void ***)(o))[(0x0c + IOS3DS_KPI_VT_BIAS) / 4])(o)
static int rc[3];
#endif
static void ps_update(void)
{
    volatile mcubox_t *b = BOX;
#if IOS3DS_KPI_MCU_NO_PMPS
    return;                     /* this firmware: no battery service (kpi.h) */
#endif
    if (!(b->valid & MCUV_BATT)) return;
    u32 key = b->batt_percent | (u32)b->flags << 8 | (u32)b->batt_volt << 16;
    if (ps && key == ps_seen) return;
    u32 pct = b->batt_percent, mv = b->batt_volt * 20u;
    int ext = (b->flags & MCUF_ADAPTER) != 0, chg = (b->flags & MCUF_CHARGING) != 0;
    if (!ps) {
        ps = ps_powerSource();
        if (!ps) return;
#if IOS3DS_KPI_MCU_PS_RETAIN
        rc[0] = PS_RC(ps);
#endif
        sym_fully = sym_withCStringNoCopy("FullyCharged");
        sym_rawv = sym_withCStringNoCopy("AppleRawBatteryVoltage");
        void *prov = getPlatform();
        if (!prov) prov = getResourceService();
        svc_attach(ps, prov);
#if IOS3DS_KPI_MCU_PS_RETAIN
        rc[1] = PS_RC(ps);
#endif
        ps_setBatteryInstalled(ps, 1);
        ps_setMaxCapacity(ps, 100);
    }
    ps_setCurrentCapacity(ps, pct);
    ps_setExternalConnected(ps, ext);
    ps_setExternalChargeCapable(ps, ext);
    ps_setIsCharging(ps, chg);
    ps_setVoltage(ps, mv);
    if (sym_fully) ps_setPSProperty(ps, sym_fully, (ext && !chg && pct >= 100) ? kOSBooleanTrue : kOSBooleanFalse);
    if (sym_rawv) {
        void *n = num_withNumber(mv, 32);
        if (n) {
            ps_setPSProperty(ps, sym_rawv, n);
            ((void (*)(void *))(*(void ***)n)[(0x14 + IOS3DS_KPI_VT_BIAS) / 4])(n);  /* release (dict keeps it) */
        }
    }
#if IOS3DS_KPI_MCU_PS_RETAIN
    rc[2] = PS_RC(ps);
#endif
    ps_updateStatus(ps);
    if (ps_seen == 0xFFFFFFFFu) {
        svc_registerService(ps, 0);
#if IOS3DS_KPI_MCU_PS_RETAIN
        IOLog("ps %p rc %d %d %d %d\n", ps, rc[0], rc[1], rc[2], PS_RC(ps));
#else
        IOLog("ios3ds-mcu: battery %u%%%s%s, %u mV: IOPMPowerSource registered\n", (unsigned)pct,
              ext ? ", charger" : "", chg ? ", charging" : "", (unsigned)mv);
#endif
    }
    ps_seen = key;
}

/* ---- LCD backlight ------------------------------------------------------- */
/* LCD registers (libn3ds include/arm11/drivers/lcd.h, 3dbrew "LCD Registers"):
   0x10202240 top-screen backlight PWM (brightness), 0x10202A40 bottom. Luma's
   screen init writes them from the ARM11; 0x10202000 is ARM11-only IO. */
#define LCD_PA          0x10202000u
#define LCD_ABL0        0x240u
#define LCD_ABL1        0xA40u
/* BL_PWM_MIN / BL_PWM_MAX and the iOS level floor are in backlight.h, shared
   with ios3ds-mcud (the daemon), and host-tested in tests/test_backlight.py. */

static vu32 *lcd;
static u32 bl_luma[2], bl_cur = 0xFFFFFFFFu;

static void backlight_set(u32 level)   /* level 0..1000 (iOS brightness x factor); 0 = off */
{
    if (!lcd) {
        u32 va = ml_io_map(LCD_PA, 0x1000);
        if (!va) return;
        lcd = (vu32 *)va;
        bl_luma[0] = lcd[LCD_ABL0 / 4];
        bl_luma[1] = lcd[LCD_ABL1 / 4];
        IOLog("ios3ds-mcu: LCD backlight as Luma left it: top 0x%x, bottom 0x%x\n",
              (unsigned)bl_luma[0], (unsigned)bl_luma[1]);
    }
    u32 v = ios_backlight_pwm(level);   /* 0 = screen off; any lit level >= BL_PWM_MIN */
    if (v == bl_cur) return;
    bl_cur = v;
    lcd[LCD_ABL0 / 4] = v;
    lcd[LCD_ABL1 / 4] = v;
    BOX->backlight = 1 + v;             /* the ARM9 uses it for shell close/open */
    dclean(&BOX->req_seq, 32);
}

static u32 acc_daemon, acc_driver;      /* who wants accelerometer samples */
static u32 gyro_driver;                 /* the gyro service's driver is powered */

static void acc_request(void)
{
    volatile mcubox_t *b = BOX;
    u32 want = acc_daemon || acc_driver;
    if (b->req_acc == want && b->req_gyro == gyro_driver) return;
    b->req_acc = want;
    b->req_gyro = gyro_driver;
    b->req_seq = b->req_seq + 1;
    dclean(&b->req_seq, 32);
}

/* ---- accelerometer: a LIS302DL on the emulated I2C bus ------------------- */
/*
 * The device tree keeps /arm-io/i2c0/accelerometer (tools/ios3ds-mcu/dt_mcu.py),
 * so Apple's AppleEmbeddedAccelerometer (class AppleLIS302DL, kext at
 * 0x8061a000) starts on it and publishes the IOHID accelerometer service.
 * kernel/shim/audio.c's shim_iic_transfer (AppleARMIICDevice transfer) hands
 * every transfer to this function first. Registers used by the driver
 * (re-log section 39): WHO_AM_I 0x0F (probe), CTRL_REG1 0x20 (bit 6 power),
 * CTRL_REG2 0x21 (bit 6 BOOT: must read back 0, else the driver panics after
 * 500 tries), CTRL_REG3 0x22, FF_WU_SRC_1 0x31, OUT_X/Y/Z 0x29/0x2B/0x2D
 * (signed 8 bit, read one by one; the driver scales by 1187/65536 g, i.e.
 * ~55 counts per g, then applies the DT 'orientation' flags: 4 = negate z).
 * Its data-ready interrupt never fires; it polls with its own timer at the
 * ReportInterval SpringBoard sets.
 */
#define ACC_I2C_ADDR    0x1D            /* = tools/ios3ds-mcu/dt_mcu.py */
#define GYRO_I2C_ADDR   0x6B            /* = dt_mcu.py GYRO_I2C_ADDR */
#define ACC_COUNTS_G    55
#define kIOReturnNoDevice 0xE00002C0

static struct { void *nub; int acc; } iic_nubs[6];
static u8 lis[2][0x40];                 /* register files: accelerometer, gyro */
static u32 acc_g;                       /* running mean of |a| in MCU units (1 g) */
static u32 acc_logged;

static u32 udiv(u32 n, u32 d)           /* no libgcc in the shim */
{
    u32 q = 0, r = 0;
    if (!d) return 0;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}

static u32 isqrt(u32 v)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* the I2C address from the nub's 'reg' property, cached per nub:
   1 = accelerometer, 2 = gyro, 0 = someone else */
static int nub_is_acc(void *nub)
{
    for (unsigned i = 0; i < sizeof iic_nubs / sizeof iic_nubs[0]; i++) {
        if (iic_nubs[i].nub == nub) return iic_nubs[i].acc;
        if (!iic_nubs[i].nub) {
            void *d = ((void *(*)(void *, const char *))(*(void ***)nub)[(0x9c + IOS3DS_KPI_VT_BIAS) / 4])(nub, "reg");
            int acc = 0;
            if (d && data_length(d) >= 4) {
                const u8 *p = data_bytes(d);
                acc = p && !p[1] && !p[2] && !p[3] ? (p[0] == ACC_I2C_ADDR) | (p[0] == GYRO_I2C_ADDR) << 1 : 0;
            }
            iic_nubs[i].nub = nub;
            iic_nubs[i].acc = acc;
            return acc;
        }
    }
    return 0;
}

/* newest MCU sample -> LIS302DL counts. The axis map (including the y sign
   fixed after the first hardware test) is shared with the host orientation
   test in boot/common/accel_axes.h; all four portrait/landscape poses,
   including the 180 degree one, are checked there. */
static void acc_sample(int8_t out[3])
{
    volatile mcubox_t *b = BOX;
    int v[3], g;
    int16_t raw[3];
    dinval(b->acc, 32);
    raw[0] = b->acc[0]; raw[1] = b->acc[1]; raw[2] = b->acc[2];
    ios_accel_axes(raw, v);
    g = (int)isqrt((u32)(v[0] * v[0]) + (u32)(v[1] * v[1]) + (u32)(v[2] * v[2]));
    if (g > 64)
        acc_g = acc_g ? acc_g - (acc_g >> 4) + ((u32)g >> 4) : (u32)g;
    for (int i = 0; i < 3; i++) {
        int c = 0;
        if (acc_g) {
            u32 m = udiv((u32)(v[i] < 0 ? -v[i] : v[i]) * ACC_COUNTS_G, acc_g);
            c = m > 127 ? 127 : (int)m;
            if (v[i] < 0) c = -c;
        }
        out[i] = (int8_t)c;
    }
    out[2] = (int8_t)-out[2];           /* the DT 'orientation' 4 negates z again */
}

/* ---- gyroscope: a second AppleEmbeddedAccelerometer driver ----------------
 * /arm-io/i2c0/gyro (dt_mcu.py) matches AppleLIS331DLH, a class the n72 does
 * not otherwise use. Two words of its vtable are patched (patches-mcu.json):
 * getPrimaryUsage (+0x3b4) returns 9, the usage CoreMotion's CLIoHidManager
 * looks for (page 0xff00), and dispatchAccelerometerEvent (+0x3c0) comes here
 * (shim +0x2C). The driver keeps doing the rest: IOHIDEventService, its
 * ReportInterval timer, power on/off through CTRL_REG1, which we turn into
 * req_gyro for the ARM9. Its own x/y/z (the emulated LIS331DLH registers, all
 * 0) are dropped: the event carries the ARM9's sample at full resolution. */
static u32 gyro_logged;

void shim_gyro_dispatch(void *svc, u32 ts_lo, u32 ts_hi, u32 x, u32 y, u32 z, u32 t, u32 st, u32 opt)
{
    volatile mcubox_t *b = BOX;
    (void)x; (void)y; (void)z; (void)t; (void)st;   /* the driver's own (emulated, zero) sample */
    dinval(b->gyro, 32);
    /* AppleEmbeddedHIDEventService::dispatchGyroEvent (IOHIDFamily 0x80546718):
       ts, x, y, z, type, subtype, 4 more fields, options */
    ((void (*)(void *, u32, u32, int32_t, int32_t, int32_t, u32, u32, u32, u32, u32, u32, u32))
        (*(void ***)svc)[(0x3c4 + IOS3DS_KPI_VT_BIAS) / 4])(svc, ts_lo, ts_hi, b->gyro[0], b->gyro[1], b->gyro[2], 0, 0, 0, 0, 0, 0, opt);
    if (++gyro_logged == 1 || gyro_logged == 1000)
        IOLog("ios3ds-mcu: gyro event #%u: %d %d %d deg/s/65536 (ARM9 sample %u)\n", (unsigned)gyro_logged,
              (int)b->gyro[0], (int)b->gyro[1], (int)b->gyro[2], (unsigned)b->gyro_samples);
}

/* AppleARMIICDevice transfer for the accelerometer and the gyro: -1 = not ours */
int shim_mcu_iic(void *nub, u32 op, u32 sublen, const u8 *sub, u32 len, u8 *buf)
{
    int kind = nub ? nub_is_acc(nub) : 0;
    if (!kind) return -1;
    volatile mcubox_t *b = BOX;
    if (kind == 2) {                    /* gyro behind an AppleLIS331DLH */
        u8 *r = lis[1];
        u32 st = b->gyro_state;
        if (!mcu_live() || (st != GYRO_ST_ITG && st != GYRO_ST_ST)) {
            if (!gyro_logged++)
                IOLog("ios3ds-mcu: no gyro for iOS (state %u)\n", (unsigned)st);
            return (int)kIOReturnNoDevice;
        }
        u32 a = sublen && sub ? (sub[sublen - 1] & 0x3F) : 0;
        for (u32 i = 0; i < len; i++, a = (a + 1) & 0x3F) {
            if (op == 2) {
                r[a] = a == 0x21 ? buf[i] & 0x7F : buf[i];      /* BOOT (bit 7) done at once */
                if (a == 0x20 && gyro_driver != ((buf[i] & 0xE0) != 0)) {
                    gyro_driver = !gyro_driver;                 /* PM bits: normal mode = powered */
                    acc_request();
                    IOLog("ios3ds-mcu: gyro %s\n", gyro_driver ? "on" : "off");
                }
            } else {
                buf[i] = a == 0x0F ? 0x32 : a == 0x27 ? 0x0F : r[a];   /* WHO_AM_I LIS331DLH, STATUS */
            }
        }
        return 0;
    }
    /* No MCU or mcu_acc=0: the register file is still answered (a failing
       transfer makes Apple's driver panic waiting for the BOOT bit, issue #7);
       the samples read as lying still, face up. */
    int hidden = !mcu_live() || b->acc_off;
    if (hidden && !acc_logged++)
        IOLog("ios3ds-mcu: accelerometer hidden from iOS (%s): still samples\n",
              b->acc_off ? "boot.cfg mcu_acc=0" : "no MCU");
    u32 a = sublen && sub ? (sub[sublen - 1] & 0x3F) : 0;
    int8_t s[3] = { 0, 0, 0 };
    int sampled = 0;
    for (u32 i = 0; i < len; i++, a = (a + 1) & 0x3F) {
        if (op == 2) {
            lis[0][a] = buf[i];
            if (a == 0x21) lis[0][a] &= (u8)~0xC0;      /* BOOT (LIS302DL bit 6, LIS331DLH bit 7): done at once */
            if (a == 0x20 && !hidden) {
                acc_driver = (buf[i] & 0xE0) != 0;      /* LIS302DL PD (bit 6) / LIS331DLH PM (bits 5-7) */
                acc_request();
                IOLog("ios3ds-mcu: accelerometer %s by AppleLIS302DL (CTRL_REG1 0x%02x)\n",
                      acc_driver ? "powered on" : "powered off", buf[i]);
            }
        } else {
            u8 v = lis[0][a];
            if (a == 0x0F) v = 0x3B;                    /* WHO_AM_I: LIS302DL */
            else if (a == 0x27) v = 0x0F;               /* STATUS: new X/Y/Z data */
            else if (a == 0x31 || a == 0x35) v = 0;     /* FF_WU_SRC: no events */
            else if (a == 0x29 || a == 0x2B || a == 0x2D) {
                if (!sampled++ && hidden) {
                    s[2] = -ACC_COUNTS_G;               /* still, face up */
                } else if (sampled == 1) {
                    static u32 served;
                    acc_sample(s);
                    if (++served == 1 || served == 100 || served == 1000 || served % 10000 == 0)
                        IOLog("ios3ds-mcu: accelerometer sample #%u: %d %d %d counts (|a| = %u MCU units)\n",
                              (unsigned)served, s[0], s[1], -s[2], (unsigned)acc_g);
                }
                v = (u8)s[(a - 0x29) >> 1];
            }
            buf[i] = v;
        }
    }
    if (acc_logged < 8 && op != 2 && a != 0x2A && a != 0x2C && a != 0x2E) {
        acc_logged++;
        IOLog("ios3ds-mcu: accelerometer read reg 0x%02x len %u\n", (unsigned)(a - len) & 0x3F, (unsigned)len);
    }
    return 0;
}

/* ---- power saving: no VBlank interrupts while iOS' screen is off ---------- */
/* The ARM9 sets mcubox screen_off once iOS has switched its (fake) CLCD
   controller off after having shown the display (boot/arm9/powersave.c).
   Then the top-screen VBlank (GIC 42) only wakes the ARM11 60 times a second
   for nothing: the CLCD frame IRQ is not injected while the controller is
   off, and the GPU mirror has want = 0. It is masked at the distributor on
   the next VBlank and unmasked from the timer interrupt or a sysctl call as
   soon as the CLCD is on again (or the ARM9 says the screen is on).
   Called from shim_gpu_irq_hook (kernel/shim/gpu_glue.c) for ids 29 / 42,
   and with id 0 from thread context. */
#define GIC_PDC0        42
#define GICD_ISENABLER1 ((vu32 *)(0xBFF00000u + 0x1104u))
#define GICD_ICENABLER1 ((vu32 *)(0xBFF00000u + 0x1184u))
extern u32 cfg_clcd_pa;                 /* entry.S */
static u32 vbl_masked, vbl_masks;

void shim_mcu_irq_hook(u32 id)
{
    volatile mcubox_t *b = BOX;
    if (!cfg_clcd_pa) return;
    dinval(&b->ack_seq, 32);            /* line 5: ps_on, screen_off */
    if (b->magic != MCU_MAGIC) return;
    vu32 *clcd = (vu32 *)(cfg_clcd_pa - 0x1FF00000u + 0xBFD00000u);
    int on = (clcd[0] & 1) || !b->screen_off || !b->ps_on;
    if (id == GIC_PDC0 && !on && !vbl_masked) {
        *GICD_ICENABLER1 = 1u << (GIC_PDC0 - 32);
        vbl_masked = 1;
        vbl_masks++;
    } else if (vbl_masked && on) {
        *GICD_ISENABLER1 = 1u << (GIC_PDC0 - 32);
        vbl_masked = 0;
    }
}

/* ---- sysctls ------------------------------------------------------------- */

typedef struct {
    u32 acc;            /* 1 = accelerometer wanted */
    u32 backlight;      /* 1 + level 0..1000; 0 = leave the LCD alone */
    u32 _r[2];
} mcuctl_t;

static mcuctl_t ctl;


static int mcu_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    (void)arg1; (void)arg2;
    shim_mcu_irq_hook(0);               /* VBlank back on if the screen is (thread context) */
    if (mcu_live()) ps_update();
    return sysctl_handle_opaque(oidp, (void *)BOX, sizeof(mcubox_t), req);
}

static int mcuctl_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    int r = sysctl_handle_opaque(oidp, arg1, arg2, req);
    if (r) return r;
    volatile mcubox_t *b = BOX;
    if (!mcu_live()) return 0;
    (void)b;
    acc_daemon = ctl.acc != 0;
    acc_request();
    if (ctl.backlight) backlight_set(ctl.backlight - 1);
    return 0;
}

static struct sysctl_oid mcu_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_OPAQUE),
    .oid_arg1    = (void *)(SHIM_VA + SHIM_MCU_OFF),
    .oid_arg2    = (int)sizeof(mcubox_t),
    .oid_name    = "ios3ds_mcu",
    .oid_handler = mcu_sysctl,
    .oid_fmt     = "S,ios3ds_mcu",
};

static struct sysctl_oid mcuctl_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLFLAG_WR | CTLTYPE_OPAQUE),
    .oid_arg1    = &ctl,
    .oid_arg2    = (int)sizeof(ctl),
    .oid_name    = "ios3ds_mcuctl",
    .oid_handler = mcuctl_sysctl,
    .oid_fmt     = "S,ios3ds_mcuctl",
};

/* thread context, idempotent (blk.c: first md0 request) */
void shim_mcu_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&mcu_oid);
    sysctl_register_oid(&mcuctl_oid);
}
