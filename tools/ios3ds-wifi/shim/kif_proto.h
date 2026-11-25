/*
 * kern.ios3ds_eth: the kernel Ethernet interface for iOS 2.x (5H11a has no
 * IOUserEthernet). Shared by the shim side (kif_eth.c) and ios3ds-wifid's
 * 2.x build. One fixed-size opaque sysctl buffer, like kern.ios3ds_net:
 *
 *   read  (kernel -> user): status + the head of the output queue (frames the
 *          stack sent on en0), packed with netio_put(); oseq = sequence
 *          number of the first one. Frames stay queued until acknowledged.
 *   write (user -> kernel): magic KIF_CMD, flags (attach with mac[], set the
 *          link state), ack = sequence number up to which the user consumed
 *          the output queue, and nframes received frames for en0's input.
 *
 * A read+write call returns the queue as it was *before* the write applied.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

#define KIF_SYSCTL_NAME "kern.ios3ds_eth"
#define KIF_MAGIC       0x3248544Bu     /* 'KTH2': a status the kernel filled */
#define KIF_CMD         0x4448544Bu     /* 'KTHD': a command from userland */
#define KIF_VERSION     1u
#define KIF_SIZE        4096u

#define KIF_F_ATTACH    1u              /* create en0 with mac[] (once) */
#define KIF_F_LINK      2u              /* link = 0/1 */

/* kif_t.state (kernel) */
#define KIF_ST_ATTACHED 1u
#define KIF_ST_LINK     2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;         /* user: KIF_F_*; kernel: 0 */
    uint32_t link;          /* user: wanted link state */
    uint32_t state;         /* kernel: KIF_ST_* */
    uint32_t seq;           /* kernel: seq of the first queued frame; user: ack */
    uint32_t nframes;       /* frames in data[] */
    uint32_t used;          /* bytes used in data[] */
    uint32_t drops;         /* kernel: output frames dropped (queue full) */
    uint8_t  mac[6];
    uint8_t  unit;          /* kernel: the unit number (en<unit>) */
    uint8_t  pad;
    uint8_t  data[KIF_SIZE - 44];
} kif_t;
_Static_assert(sizeof(kif_t) == KIF_SIZE, "kif_t size");
#define KIF_DATA (KIF_SIZE - 44u)
