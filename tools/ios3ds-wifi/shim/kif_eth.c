/*
 * ios3ds shim, iOS 2.x only: a kernel Ethernet interface ("en0") for the
 * Wi-Fi bridge. 5H11a's kernelcache has no IOUserEthernet (and no tun/utun),
 * so ios3ds-wifid cannot create the interface from userland as on 3.x/4.x.
 * This file registers one with the Darwin 9 network KPI (ifnet_allocate /
 * ifnet_attach / ifnet_input, the "ether" family functions for demux and
 * framing, bpfattach for IPConfiguration's DHCP) and exchanges its frames
 * with the daemon through the opaque sysctl kern.ios3ds_eth (kif_proto.h).
 *
 * Built only when scripts/kpi.py defines IOS3DS_KPI_KIF (versions.py
 * "kernel_ifnet"), and linked into the shim's GPU part (link.ld: *kif*.o), so
 * the 4.2.1 shim is unchanged. Thread context only: the sysctl handler runs
 * in the daemon's syscall, the output callback in whatever thread sends.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "kpi.h"
#include "wifi_proto.h"
#include "kif_proto.h"

#ifdef IOS3DS_KPI_KIF

typedef void *ifnet_t;
typedef void *mbuf_t;

/* addresses from kpi.py carry the Thumb bit already */
#define K(name, ret, ...) ((ret (*)(__VA_ARGS__))(IOS3DS_KPI_KIF_##name))
#define ifnet_allocate      K(IFNET_ALLOCATE, int, const void *, ifnet_t *)
#define ifnet_attach        K(IFNET_ATTACH, int, ifnet_t, const void *)
#define ifnet_input         K(IFNET_INPUT, int, ifnet_t, mbuf_t, const void *)
#define ifnet_set_flags     K(IFNET_SET_FLAGS, int, ifnet_t, u16, u16)
#define ifnet_set_mtu       K(IFNET_SET_MTU, int, ifnet_t, u32)
#define ifnet_set_hdrlen    K(IFNET_SET_HDRLEN, int, ifnet_t, u8)
#define ifnet_set_addrlen   K(IFNET_SET_ADDRLEN, int, ifnet_t, u8)
#define ifnet_set_baudrate  K(IFNET_SET_BAUDRATE, int, ifnet_t, u32, u32)
#define ifnet_event         K(IFNET_EVENT, int, ifnet_t, void *)
#define bpfattach           K(BPFATTACH, void, ifnet_t, u32, u32)
#define bpf_tap_in          K(BPF_TAP_IN, void, ifnet_t, u32, mbuf_t, void *, u32)
#define bpf_tap_out         K(BPF_TAP_OUT, void, ifnet_t, u32, mbuf_t, void *, u32)
#define mbuf_gethdr         K(MBUF_GETHDR, int, int, int, mbuf_t *)
#define mbuf_mclget         K(MBUF_MCLGET, int, int, int, mbuf_t *)
#define mbuf_data           K(MBUF_DATA, void *, mbuf_t)
#define mbuf_setdata        K(MBUF_SETDATA, int, mbuf_t, void *, u32)
#define mbuf_pkthdr_setlen  K(MBUF_PKTHDR_SETLEN, void, mbuf_t, u32)
#define mbuf_pkthdr_len     K(MBUF_PKTHDR_LEN, u32, mbuf_t)
#define mbuf_pkthdr_setrcvif K(MBUF_PKTHDR_SETRCVIF, int, mbuf_t, ifnet_t)
#define mbuf_pkthdr_setheader K(MBUF_PKTHDR_SETHEADER, void, mbuf_t, void *)
#define mbuf_copydata       K(MBUF_COPYDATA, int, mbuf_t, u32, u32, void *)
#define mbuf_freem          K(MBUF_FREEM, void, mbuf_t)
#define IOMalloc            K(IOMALLOC, void *, u32)

#define KPI(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define sysctl_register_oid   KPI(IOS3DS_KPI_SYSCTL_REGISTER_OID, void, void *)
#define sysctl_handle_opaque  KPI(IOS3DS_KPI_SYSCTL_HANDLE_OPAQUE, int, void *, void *, int, void *)
#define kprintf               ((int (*)(const char *, ...))(IOS3DS_KPI_PRINTF | 1))

#define MBUF_WAITOK     0
#define MBUF_DONTWAIT   1
#define MBUF_TYPE_DATA  1
#define DLT_EN10MB      1
#define IFF_BROADCAST   0x2
#define IFF_RUNNING     0x40
#define IFF_SIMPLEX     0x800
#define IFF_MULTICAST   0x8000
#define EOPNOTSUPP      102
#define EBUSY           16
#define ENOBUFS         55

/* Darwin 9 (xnu-1228) struct ifnet_init_params, checked against 5H11a's
   ifnet_allocate (output +0x18 .. broadcast_len +0x48) */
struct ifnet_init_params {
    const void *uniqueid;
    u32 uniqueid_len;
    const char *name;
    u32 unit;
    u32 family;             /* IFNET_FAMILY_ETHERNET = 2 */
    u32 type;               /* IFT_ETHER = 6 */
    void *output;
    void *demux, *add_proto, *del_proto, *check_multi, *framer;
    void *softc;
    void *ioctl, *set_bpf_tap, *detach;
    void *event;
    const void *broadcast_addr;
    u32 broadcast_len;
};

struct sysctl_oid {
    void *oid_parent, *oid_link;
    int oid_number, oid_kind;
    void *oid_arg1;
    int oid_arg2;
    const char *oid_name;
    int (*oid_handler)(struct sysctl_oid *, void *, int, void *);
    const char *oid_fmt;
};

#define QLEN 64u
static mbuf_t q[QLEN];
static volatile u32 q_head, q_tail;         /* seq numbers; q[seq % QLEN] */
static volatile u32 q_drops;
static ifnet_t kif;
static u32 kif_link;
static kif_t *kio;
static volatile u32 kif_busy;
static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static inline u32 irq_off(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
    return cpsr;
}
static inline void irq_restore(u32 cpsr) { __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory"); }

static int kif_output(ifnet_t ifp, mbuf_t m)
{
    bpf_tap_out(ifp, DLT_EN10MB, m, 0, 0);
    u32 s = irq_off();
    if (!kif_link || q_tail - q_head >= QLEN) {
        q_drops++;
        irq_restore(s);
        mbuf_freem(m);
        return 0;
    }
    q[q_tail % QLEN] = m;
    q_tail++;
    irq_restore(s);
    return 0;
}

/* SIOCGIFMEDIA answers the link state (IPConfiguration / KernelEventMonitor
   read it); flags, multicast and address changes need nothing from us */
#define SIOCSIFFLAGS    0x80206910u
#define SIOCADDMULTI    0x80206931u
#define SIOCDELMULTI    0x80206932u
#define SIOCGIFMEDIA    0xC0286938u
#define SIOCSIFMTU      0x80206934u
struct ifmediareq { char name[16]; int current, mask, status, active, count; int *ulist; };
static int kif_ioctl(ifnet_t ifp, u32 cmd, void *data)
{
    (void)ifp;
    switch (cmd) {
    case SIOCGIFMEDIA: {
        struct ifmediareq *r = data;
        r->current = r->active = 0x20 /* IFM_ETHER */ | 0 /* IFM_AUTO */;
        r->mask = 0;
        r->status = 1 /* IFM_AVALID */ | (kif_link ? 2 /* IFM_ACTIVE */ : 0);
        r->count = 0;
        return 0;
    }
    case SIOCSIFFLAGS: case SIOCADDMULTI: case SIOCDELMULTI:
        return 0;
    }
    return EOPNOTSUPP;
}

/* KEV_DL_LINK_ON / OFF for KernelEventMonitor (State:/Network/Interface/en0/Link) */
static void kif_link_event(u32 on)
{
    u32 ev[12];
    for (u32 i = 0; i < 12; i++) ev[i] = 0;
    ev[0] = sizeof ev;              /* total_size */
    ev[1] = 1;                      /* KEV_VENDOR_APPLE */
    ev[2] = 1;                      /* KEV_NETWORK_CLASS */
    ev[3] = 2;                      /* KEV_DL_SUBCLASS */
    ev[5] = on ? 13 : 12;           /* KEV_DL_LINK_ON / KEV_DL_LINK_OFF */
    ev[6] = 2;                      /* net_event_data.if_family = ETHERNET */
    ev[7] = 0;                      /* if_unit */
    ((char *)&ev[8])[0] = 'e';
    ((char *)&ev[8])[1] = 'n';
    ifnet_event(kif, ev);
}

static u8 kif_mac[6];
static void kif_attach(const u8 *m)
{
    const u8 *mac = kif_mac;
    for (u32 i = 0; i < 6; i++) kif_mac[i] = m[i];
    static const char name[] = "en";
    struct ifnet_init_params p;
    u8 *b = (u8 *)&p;
    for (u32 i = 0; i < sizeof p; i++) b[i] = 0;
    p.uniqueid = mac;
    p.uniqueid_len = 6;
    p.name = name;
    p.unit = 0;
    p.family = 2;
    p.type = 6;
    p.output = (void *)kif_output;
    p.ioctl = (void *)kif_ioctl;
    p.demux = (void *)IOS3DS_KPI_KIF_ETHER_DEMUX;
    p.add_proto = (void *)IOS3DS_KPI_KIF_ETHER_ADD_PROTO;
    p.del_proto = (void *)IOS3DS_KPI_KIF_ETHER_DEL_PROTO;
    p.check_multi = (void *)IOS3DS_KPI_KIF_ETHER_CHECK_MULTI;
    p.framer = (void *)IOS3DS_KPI_KIF_ETHER_FRAMEOUT;
    p.broadcast_addr = bcast;
    p.broadcast_len = 6;
    static ifnet_t ifp;             /* kept: a second allocate with our id is EBUSY */
    int err = 0;
    if (!ifp && ((err = ifnet_allocate(&p, &ifp)) || !ifp)) {
        kprintf("[kif] ifnet_allocate failed: %d\n", err);
        ifp = 0;
        return;
    }
    ifnet_set_mtu(ifp, 1500);
    ifnet_set_hdrlen(ifp, 14);
    ifnet_set_addrlen(ifp, 6);
    ifnet_set_baudrate(ifp, 54000000, 0);
    ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST | IFF_RUNNING,
                    IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST | IFF_RUNNING);
    u8 sdl[20];
    for (u32 i = 0; i < sizeof sdl; i++) sdl[i] = 0;
    sdl[0] = sizeof sdl;            /* sdl_len */
    sdl[1] = 18;                    /* AF_LINK */
    sdl[4] = 6;                     /* IFT_ETHER */
    sdl[6] = 6;                     /* sdl_alen */
    for (u32 i = 0; i < 6; i++) sdl[8 + i] = mac[i];
    err = ifnet_attach(ifp, sdl);
    if (err) {
        kprintf("[kif] ifnet_attach failed: %d\n", err);
        return;
    }
    bpfattach(ifp, DLT_EN10MB, 14);
    kif = ifp;
    kprintf("[kif] en0 attached (Darwin 9 ifnet, kern.ios3ds_eth)\n");
}

static void kif_input(const u8 *f, u32 len)
{
    mbuf_t m = 0;
    if (mbuf_gethdr(MBUF_WAITOK, MBUF_TYPE_DATA, &m) || !m) return;
    if (mbuf_mclget(MBUF_WAITOK, MBUF_TYPE_DATA, &m)) {
        mbuf_freem(m);
        return;
    }
    u8 *d = mbuf_data(m);
    for (u32 i = 0; i < len; i++) d[i] = f[i];
    mbuf_setdata(m, d, len);
    mbuf_pkthdr_setlen(m, len);
    mbuf_pkthdr_setrcvif(m, kif);
    bpf_tap_in(kif, DLT_EN10MB, m, 0, 0);
    /* DLIL wants the frame header aside and the data at the payload */
    mbuf_pkthdr_setheader(m, d);
    mbuf_setdata(m, d + 14, len - 14);
    mbuf_pkthdr_setlen(m, len - 14);
    u32 st[8] = { 1, len, 0, 0, 0, 0, 0, 0 };
    ifnet_input(kif, m, st);
}

static void kif_fill(kif_t *io)
{
    io->magic = KIF_MAGIC;
    io->version = KIF_VERSION;
    io->flags = io->link = 0;
    io->state = (kif ? KIF_ST_ATTACHED : 0) | (kif_link ? KIF_ST_LINK : 0);
    io->drops = q_drops;
    io->unit = 0;
    u32 s = irq_off();
    u32 head = q_head, tail = q_tail;
    irq_restore(s);
    io->seq = head;
    u32 off = 0, n = 0;
    for (u32 i = head; i != tail; i++) {
        mbuf_t m = q[i % QLEN];
        u32 len = mbuf_pkthdr_len(m);
        if (len < NET_FRAME_MIN || len > NET_FRAME_MAX) len = NET_FRAME_MIN;  /* keeps seq in step */
        u32 need = 2u + ((len + 1u) & ~1u);
        if (KIF_DATA - off < need) break;
        io->data[off] = (u8)len;
        io->data[off + 1] = (u8)(len >> 8);
        mbuf_copydata(m, 0, len, io->data + off + 2);
        off += need;
        n++;
    }
    io->nframes = n;
    io->used = off;
}

static void kif_apply(kif_t *io)
{
    if (io->magic != KIF_CMD || io->version != KIF_VERSION) return;
    if ((io->flags & KIF_F_ATTACH) && !kif) kif_attach(io->mac);
    if (!kif) return;
    /* acknowledged output frames: free them */
    for (;;) {
        u32 s = irq_off();
        if (q_head == q_tail || (int)(io->seq - q_head) <= 0) {
            irq_restore(s);
            break;
        }
        mbuf_t m = q[q_head % QLEN];
        q_head++;
        irq_restore(s);
        mbuf_freem(m);
    }
    if (io->flags & KIF_F_LINK) {
        u32 l = io->link ? 1 : 0;
        if (l != kif_link) {
            kif_link = l;
            kif_link_event(l);
        }
    }
    uint32_t off = 0, len;
    const u8 *p;
    for (u32 i = 0; i < io->nframes && kif_link &&
                    (p = netio_get(io->data, io->used > KIF_DATA ? KIF_DATA : io->used, &off, &len)); i++)
        kif_input(p, len);
}

static int kif_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    (void)arg1;
    u32 s = irq_off(), was = kif_busy;
    kif_busy = 1;
    irq_restore(s);
    if (was) return EBUSY;
    kif_fill(kio);
    int err = sysctl_handle_opaque(oidp, kio, arg2, req);
    if (!err) kif_apply(kio);
    kif_busy = 0;
    return err;
}

static struct sysctl_oid kif_oid = {
    .oid_parent  = (void *)IOS3DS_KPI_SYSCTL__KERN_CHILDREN,
    .oid_number  = -1,
    .oid_kind    = (int)(0xC0000000u | 5),     /* CTLFLAG_RW | CTLTYPE_OPAQUE */
    .oid_arg2    = (int)KIF_SIZE,
    .oid_name    = "ios3ds_eth",
    .oid_handler = kif_sysctl,
    .oid_fmt     = "S,ios3ds_eth",
};

/* from shim_net_register() (thread context, once) */
void shim_kif_register(void)
{
    kio = IOMalloc(KIF_SIZE);
    if (!kio) return;
    kif_oid.oid_arg1 = kio;
    sysctl_register_oid(&kif_oid);
}

#endif /* IOS3DS_KPI_KIF */
