/*
 * ios3ds shim: the network mailbox as the read/write sysctl "kern.ios3ds_net"
 * (4096 opaque bytes = netio_t, boot/common/wifi_proto.h). ios3ds-wifid polls
 * it and bridges the frames to an IOUserEthernet interface.
 *
 * Built into shim.bin by scripts/mkboot.py build_shim(); registered from
 * shim_mdevstrategy() (kernel/shim/blk.c, first root-disk I/O = thread
 * context). No kernel patch is needed.
 *
 * Why one fixed-size RW opaque OID: sysctl_handle_opaque() (= sysctl_io_opaque
 * with changed == NULL) copies exactly arg2 bytes out and, if a new value was
 * supplied, exactly arg2 bytes in, without this file having to know struct
 * sysctl_req. We detect "a new value arrived" by the magic the caller put in.
 *
 * Kernel facts (8C148, xnu-1504.58.28), shared with tools/ios3ds-input:
 *   sysctl_register_oid   0x80159c54 (Thumb)
 *   sysctl_handle_opaque  0x80159242 (Thumb)
 *   sysctl__kern_children 0x8027f7b8
 *   struct sysctl_oid: 36-byte pre-OID2 layout
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "wifi_proto.h"

static inline void net_dclean(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* Only ever used on lines the ARM11 never writes, so nothing can be lost. */
void shim_l2_inv_kva(const volatile void *va, u32 len);    /* kernel/shim/entry.S: New 3DS L2 (docs/n3ds.md) */
static inline void net_dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    shim_l2_inv_kva(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

#define NET_DINVAL(p, len) net_dinval((volatile void *)(p), (u32)(len))
#define NET_DCLEAN(p, len) net_dclean((volatile void *)(p), (u32)(len))
#include "net_core.h"
#include "kpi.h"

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
_Static_assert(sizeof(struct sysctl_oid) == 36, "xnu-1504 arm sysctl_oid");

#define CTLFLAG_RW      0xC0000000
#define CTLTYPE_OPAQUE  5
#define OID_AUTO        (-1)
#define EBUSY           16

#define KPI(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define sysctl_register_oid   KPI(IOS3DS_KPI_SYSCTL_REGISTER_OID, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPI(IOS3DS_KPI_SYSCTL_HANDLE_OPAQUE, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_KERN_CHILDREN  ((void *)IOS3DS_KPI_SYSCTL__KERN_CHILDREN)

/* NET_VA is 4.2.1's mapping (virt base 0x80000000); Darwin 9/10 (2.2.1, 3.1.3)
   map physical 0x20000000 at 0xC0000000, so take the base from kpi.h. For
   4.2.1 this is NET_VA exactly. */
#undef NET_VA
#define NET_VA (IOS3DS_KPI_VIRT_BASE + (NET_PA - 0x20000000u))
#define NETMBOX ((volatile netmbox_t *)NET_VA)
#define NETIO   ((netio_t *)(NET_VA + NET_IO_OFF))

static volatile u32 net_busy;

static int net_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    /* The io buffer is single-instance and the copyout/copyin below can
       block, so a second caller is turned away instead of sharing it. */
    u32 cpsr, was;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
    was = net_busy;
    net_busy = 1;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    if (was) return EBUSY;

    netio_t *io = (netio_t *)arg1;
    net_core_fill(NETMBOX, io);
    int err = sysctl_handle_opaque(oidp, io, arg2, req);
    /* magic is still NETIO_MAGIC unless userland supplied a new value */
    if (!err) net_core_apply(NETMBOX, io);
    net_busy = 0;
    return err;
}

static struct sysctl_oid net_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RW | CTLTYPE_OPAQUE),
    .oid_arg1    = (void *)(NET_VA + NET_IO_OFF),
    .oid_arg2    = (int)NETIO_SIZE,
    .oid_name    = "ios3ds_net",
    .oid_handler = net_sysctl,
    .oid_fmt     = "S,ios3ds_net",
};

#define WLANMBOX ((volatile wlanmbox_t *)(NET_VA + WLAN_OFF))
static volatile u32 wlan_busy;

/* kern.ios3ds_wlan: the Wi-Fi control area for the Apple80211 bridge
   (tools/ios3ds-wifi/a80211); same fixed-size opaque scheme as above */
static int wlan_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    u32 cpsr, was;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
    was = wlan_busy;
    wlan_busy = 1;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    if (was) return EBUSY;

    wlanio_t *io = (wlanio_t *)arg1;
    wlan_core_fill(WLANMBOX, io);
    int err = sysctl_handle_opaque(oidp, io, arg2, req);
    if (!err) wlan_core_apply(WLANMBOX, io);
    wlan_busy = 0;
    return err;
}

static struct sysctl_oid wlan_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RW | CTLTYPE_OPAQUE),
    .oid_arg1    = (void *)(NET_VA + WLAN_IO_OFF),
    .oid_arg2    = (int)sizeof(wlanio_t),
    .oid_name    = "ios3ds_wlan",
    .oid_handler = wlan_sysctl,
    .oid_fmt     = "S,ios3ds_wlan",
};

/* Thread context only (takes the funnel and a rw-lock). Idempotent. */
void shim_net_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&net_oid);
    sysctl_register_oid(&wlan_oid);
}
