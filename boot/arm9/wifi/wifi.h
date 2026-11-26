/*
 * ios3ds ARM9 Wi-Fi: public interface of boot/arm9/wifi (see docs/wifi.md).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"
#include "wifi_proto.h"

/* libc subset: the monitor's own (boot/common/util.h) or the host's for tests */
#ifdef WIFI_HOST_TEST
#include <string.h>
#else
#include "util.h"
#endif

/* ---- sd:/ios3ds/wifi.cfg ---------------------------------------------- */
enum { WIFI_SEC_OPEN = 0, WIFI_SEC_WPA2 = 1 };
enum { WIFI_BE_AUTO = 0, WIFI_BE_OFF, WIFI_BE_AR6014, WIFI_BE_QEMU, WIFI_BE_LOOP };

typedef struct {
    char ssid[33];          /* 1..32 bytes */
    u8   ssid_len;
    char psk[65];           /* passphrase (8..63 chars) or 64 hex digits = PMK */
    u8   security;          /* WIFI_SEC_* (default: WPA2 if psk given, else open) */
    u8   backend;           /* WIFI_BE_* */
    u8   have_pmk;          /* pmk[] valid (from 64 hex digits or pmk=) */
    u8   pmk[32];
    u8   have_mac;          /* mac= override (QEMU / loop backends only) */
    u8   mac[6];
    char fw[64];            /* firmware container, default 0:/ios3ds/wifi-fw.bin */
    u32  verbose;           /* 1 = log every state change, 2 = also frames */
    u8   diag;              /* [wifi-diag] bring-up log (default 1; diag=0 turns it off) */
    u8   fwmain;            /* main firmware type taken from nwm.code: 4 (default) or 1 */
    char nettest[64];       /* "<url> [dns name]": iOS self-test after DHCP (docs/wifi.md) */
} wifi_cfg_t;

/* Parse the text of wifi.cfg (modified in place). Returns 0, or the 1-based
   number of the first bad line negated. Unknown keys are ignored. */
int wifi_cfg_parse(char *txt, wifi_cfg_t *out);
/* Check a parsed config; returns NULL if usable, else a message. */
const char *wifi_cfg_check(const wifi_cfg_t *c);

/* ---- mailbox, ARM9 side (wifi_ring.c; pure, host-tested) --------------- */
void net_mbox_init(volatile netmbox_t *m, u32 backend, const u8 mac[6]);
/* ARM9 -> iOS. Returns 0, -1 = ring full (counted), -2 = bad length. */
int  net_mbox_push_rx(volatile netmbox_t *m, const u8 *frame, u32 len);
/* iOS -> ARM9. Copies the oldest pending frame; returns its length, 0 = none.
   The slot is released by net_mbox_tx_done(). */
u32  net_mbox_peek_tx(volatile netmbox_t *m, u8 *frame, u32 cap);
void net_mbox_tx_done(volatile netmbox_t *m);

/* ---- 802.3 <-> AR6K WMI data frames (wifi_frame.c; pure, host-tested) --- */
#define WMI_DATA_HDR    2u      /* u8 info; u8 info2 ("00 1C" in dsiwifi; RX: s8 rssi, u8 info) */
#define LLC_SNAP_LEN    8u
/* Ethernet II frame -> { wmi hdr, dst, src, be16 length, AA AA 03 00 00 00,
   ethertype, payload }. Returns the output length, or 0 if it does not fit. */
u32 wifi_frame_to_wmi(const u8 *eth, u32 len, u8 *out, u32 cap);
/* The reverse (firmware -> iOS). Accepts LLC/SNAP (RFC 1042 and bridge-tunnel)
   and passes frames that are already Ethernet II. Returns the Ethernet frame
   length, or 0 for anything that cannot be converted. *ethertype is set. */
u32 wifi_frame_from_wmi(const u8 *wmi, u32 len, u8 *out, u32 cap, u16 *ethertype);
/* HTC RX trailer (ath6kl htc.h): records of {id, len, payload}; record 1 is a
   list of {endpoint, credits}. Applies the reports to credits[8] (saturating)
   and returns the number of credit units, 0 for a malformed/unknown trailer. */
#define WMI_HTC_REC_CREDITS 1u
u32 wifi_htc_credits(const u8 *trailer, u32 len, u8 credits[8]);

/* ---- backends ---------------------------------------------------------- */
typedef struct {
    const char *name;
    u32 id;                                         /* NET_BACKEND_* */
    int (*init)(const wifi_cfg_t *cfg, u8 mac[6]);  /* 0 = present */
    /* Advance the driver by one bounded step; returns a NET_ST_* state. */
    u32 (*poll)(void);
    int (*tx)(const u8 *eth, u32 len);              /* 0 = sent, <0 = busy/error */
    u32 (*rx)(u8 *eth, u32 cap);                    /* frame length, 0 = none */
    int (*last_error)(void);
} wifi_backend_t;

extern const wifi_backend_t wifi_backend_ar6014;    /* ar6k.c */
extern const wifi_backend_t wifi_backend_vnic;      /* vnic.c */
extern const wifi_backend_t wifi_backend_loop;      /* wifi.c */

/* ---- NWM .code -> firmware container (nwmcode.c, docs/wifi.md section 3) ---- */
enum { NWM_E_NOPOOL = -1, NWM_E_SIZES = -2, NWM_E_NOMAIN = -3, NWM_E_BLZ = -4, NWM_E_TOOBIG = -5 };
enum { NWM_SDATA = 1, NWM_SCODE = 2, NWM_DB = 3, NWM_T1 = 4, NWM_T4 = 5, NWM_T5 = 6, NWM_NBLK = 7 };
enum { NWM_NEWER = 1, NWM_OLDER = 2 };
typedef struct {
    u32 in_size, code_size;     /* file size; size after BLZ (= in_size if it was plain) */
    u8  blz, layout, main_type, db_end_is_stub;
    u32 text_base, pool_off;
    u32 main_size[3];           /* type1, type4, type5 (0 = not referenced) */
    u32 blk_size[4], blk_crc[4];/* container order: stub data, stub code, database, main */
} nwm_info_t;
/* code: len bytes of NWM's .code in a buffer of cap bytes (BLZ is undone in
   place). main_type 1/4/5. Writes the 'I3WF' container to out. 0 or NWM_E_*. */
int nwm_extract(u8 *code, u32 len, u32 cap, u32 main_type, u8 *out, u32 outcap, u32 *outlen,
                nwm_info_t *info);
const char *nwm_strerror(int e);
u32 nwm_crc32(const u8 *p, u32 n);

/* ---- monitor hooks (wifi.c) -------------------------------------------- */
/* Once, after the SD card is mounted and before the jump into XNU: reads
   sd:/ios3ds/wifi.cfg, derives the PMK, initialises the mailbox at NET_PA and
   starts the backend. Never fails the boot: without a config the mailbox
   just reports NET_ST_OFF. */
void wifi_init(u32 scratch_pa, u32 scratch_len);
/* Every monitor-loop iteration: one driver step + move pending frames.
   Returns true if it did work (the loop then skips its idle delay). */
bool wifi_poll(void);
/* Opt-in idle policy (boot.cfg power_opt=1): ask the AR6014 for WMI REC_POWER
   (1) while iOS' screen is off and restore MAX_PERFORMANCE (2) on wake. QEMU's
   paravirtual NIC ignores WMI; the log line still shows the request. */
void wifi_idle(bool off);
