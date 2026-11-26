/*
 * Atheros AR6014 ("DWM-W028", the Old 3DS Wi-Fi chip) bring-up on the ARM9:
 * power, SDIO, BMI bootloader, firmware upload, HTC/WMI handshake, connect,
 * WPA2 4-way handshake, 802.3 data path.
 *
 * STATUS: SKELETON, NEVER RUN ON HARDWARE. It compiles, every wait is bounded
 * and every step logs, so the first console runs tell us where it stops.
 * Sources for each step are in docs/wifi.md section 2-4 (GBATEK "DSi Atheros
 * Wifi", dsiwifi (MIT, (c) 2021 Max Thomas) for the SDIO/BMI/WMI sequences,
 * Linux ath6kl for the WMI structures).
 *
 * The chip has no usable ROM-only mode: BMI_DONE jumps to RAM (0x927000), so
 * the console's own firmware parts must be uploaded first. They come from
 * sd:/ios3ds/wifi-fw.bin (tools/ios3ds-wifi/nwmfw.py) or straight from
 * sd:/ios3ds/nwm.code, the user's own NWM system module code that the
 * GodMode9 script tools/ios3ds-wifi/ios3ds-wifi-fw.gm9 copies off the console
 * (nwmcode.c; GBATEK "3DS Files - Module NWM"). Nothing of it is in this
 * repository.
 *
 * [wifi-diag] lines (wifi.cfg diag=1, the default) log every bring-up step
 * with its time since power-up, so one hardware run shows where it stops;
 * docs/wifi.md section 6 maps each line to the assumption it settles.
 *
 * Power-up follows libn3ds WIFI_init (CFG11_WIFICNT, GPIO 2_2 = SDIO select,
 * GPIO 3_12 = nRESET, ~2 ms); the BMI register setup before the upload and the
 * HTC service list follow dsiwifi wifi_card_wlan_init / htc_handle_pkt.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"
#include "sdio.h"
#include "wpa2.h"
#include "log.h"

void waitcycles(u32 n);
extern u32 wifi_scratch_pa, wifi_scratch_len;      /* wifi.c */
#define NWM_CODE_PATH "0:/ios3ds/nwm.code"

/* ---- 3DS-side power and reset ------------------------------------------ */
#define CFG11_WIFICNT   (*(vu8 *)0x10140180u)   /* bit0: enable the Wi-Fi subsystem */
#define GPIO_WIFI_MODE  (*(vu16 *)0x10147014u)  /* GPIO 2_2 (REG_GPIO2_DAT2): 0 = SDIO/Atheros, 1 = DS radio */
#define GPIO_WIFI_EN    (*(vu16 *)0x10147028u)  /* GPIO 3_12 (REG_GPIO3_DAT2): Atheros nRESET, 1 = run */
#define REG_PRNG        (*(vu32 *)0x10011000u)  /* ARM9 PRNG source */
#define REG_TIMER0      (*(vu16 *)0x10003000u)
/* the monitor's free-running clock (boot/arm9/main.c clk_start(): timers 2+3
   cascaded, prescaler 1024 = 65457 Hz), running before wifi_poll() is called */
#define REG_T9_VAL2     (*(vu16 *)0x10003008u)
#define REG_T9_VAL3     (*(vu16 *)0x1000300Cu)

/* ---- AR6014 (GBATEK "Internal Memory Map") ------------------------------- */
#define CHIP_ID_AR6014      0x0D000001u
#define HOST_INTEREST       0x00520000u
#define HI_DSET_LIST_HEAD   0x18
#define HI_BOARD_DATA_INIT  0x58
#define HI_MBOX_IO_BLOCK_SZ 0x6C
#define HI_MBOX_ISR_YIELD   0x74
#define SOC_RESET_CONTROL   0x00004000u     /* bit8: warm reset into the BMI ROM */
#define SOC_CHIP_ID         0x000040ECu
#define SOC_WLAN_SYS_SLEEP  0x000040C4u
#define SOC_CLOCK_CONTROL   0x00004028u
#define SOC_REG_4020        0x00004020u
#define SOC_SCRATCH0        0x000180C0u
/* function 1 registers */
#define F1_HOST_INT_STATUS  0x400
#define F1_RX_LOOKAHEAD_VALID 0x405
#define F1_RX_LOOKAHEAD0    0x408
#define F1_INT_STATUS_EN    0x418
#define F1_SCRATCH0         0x460
#define F1_COUNT4           0x450
#define F1_WINDOW_DATA      0x474
#define F1_WINDOW_WRITE_ADDR 0x478
#define F1_WINDOW_READ_ADDR 0x47C
#define F1_MBOX0_BYTE       0xFF            /* any address in the MBOX0 window */
#define F1_MBOX0_END        0x4000          /* block transfers end here */

enum { BMI_DONE = 1, BMI_WRITE_MEMORY = 3, BMI_EXECUTE = 4, BMI_READ_SOC_REG = 6,
       BMI_WRITE_SOC_REG = 7, BMI_GET_TARGET_ID = 8, BMI_LZ_START = 0xD, BMI_LZ_DATA = 0xE };

/* HTC endpoint 0 is the control endpoint; the others are handed out by the
   firmware in CONNECT_SERVICE responses (ath6kl htc_conn_service_resp), in
   practice 1 = WMI control, 2..5 = data BE/BK/VI/VO */
enum { PKT_HTC = 0 };
#define HTC_FLG_RX_TRAILER 0x02
#define HTC_FLG_NEED_CREDIT 0x01    /* ask the target to return credits */
#define HTC_REC_CREDITS 1           /* trailer record id (ath6kl htc.h) */
enum { HTC_READY = 1, HTC_CONN_SVC = 2, HTC_SETUP_COMPLETE = 4 };
enum { WMI_CONNECT = 0x0001, WMI_DISCONNECT = 0x0003, WMI_SYNCHRONIZE = 0x0004,
       WMI_START_SCAN = 0x0007, WMI_SET_BSS_FILTER = 0x0009, WMI_SET_PROBED_SSID = 0x000A,
       WMI_SET_DISC_TIMEOUT = 0x000D, WMI_SET_POWER_MODE = 0x0012,
       WMI_ADD_CIPHER_KEY = 0x0016, WMI_TARGET_ERROR_BITMASK = 0x0022,
       WMI_SET_KEEPALIVE = 0x003D, WMI_SET_HEARTBEAT_TIMEOUT = 0x0047,
       WMI_GET_CHANNEL_LIST = 0x000E,              /* the reply comes back with the same id */
       WMI_EV_READY = 0x1001, WMI_EV_CONNECT = 0x1002, WMI_EV_DISCONNECT = 0x1003,
       WMI_EV_BSSINFO = 0x1004, WMI_EV_CMD_ERROR = 0x1005, WMI_EV_REG_DOMAIN = 0x1006,
       WMI_EV_SCAN_COMPLETE = 0x100A };
/* WMI crypto types as the DSi/3DS firmware uses them (GBATEK "WMI Connect
   Functions": NONE 1, WEP 2, TKIP 3, AES 4 - not the ath6kl bit values) */
enum { CRYPT_NONE = 1, CRYPT_TKIP = 3, CRYPT_AES = 4 };
/* WMI authMode as sent by dsiwifi: WPA-PSK 3, WPA2-PSK 5 (GBATEK) */
enum { AUTH_NONE = 1, AUTH_WPA_PSK = 3, AUTH_WPA2_PSK = 5 };

/* ---- sd:/ios3ds/wifi-fw.bin (written by tools/ios3ds-wifi/nwmfw.py) ------ */
#define FW_MAGIC 0x46573349u    /* 'I3WF' */
enum { FW_STUB_DATA = 1, FW_STUB_CODE = 2, FW_DATABASE = 3, FW_MAIN_LZ = 4 };
typedef struct { u32 kind, dst, off, len; } fw_part_t;
typedef struct { u32 magic, version, nparts, chip_id; fw_part_t part[8]; } fw_hdr_t;
/* Main.type4 is up to 0xA5EB bytes (GBATEK); 64 KiB of ARM9 RAM, used only
   until the upload is done */
static u8 fw_buf[64 * 1024] __attribute__((aligned(4)));
static u32 fw_size;

/* ---- driver state -------------------------------------------------------- */
static struct {
    u32 state;
    int err;
    const wifi_cfg_t *cfg;
    u8 mac[6], bssid[6];
    u32 step, part, part_off;       /* firmware upload cursor */
    wpa2_t wpa;
    u16 channel;                    /* MHz of the AP found by the scan, 0 = unknown */
    u8 found, group_crypt;          /* scan result; WMI group crypto type */
    u8 rxframe[NET_FRAME_MAX];      /* one converted frame waiting for rx() */
    u32 rxlen;
    u8 ep_wmi, ep_data;             /* endpoints the firmware assigned */
    /* HTC credits (ath6kl htc_mbox.c): the target owns a pool of credits of
       cred_sz bytes; a message consumes ceil(padded_len / cred_sz) of them and
       the target returns them in the RX trailer of the messages it sends. */
    u8  ep_credits[8], ep_cred_per_msg[8];
    u16 ep_max_msg[8];
    u32 cred_sz, cred_pool;
    u32 credit_rpt, credit_ret, credit_used, credit_low, credit_busy;
    u32 credit_refund;              /* credits returned after a failed mailbox write */
    u32 ep0_refill;                 /* HTC control credits funded locally (issue #11) */
    u32 defer_drop;                 /* packets that could not be queued at all */
    u32 defer_full;                 /* waits stopped with the deferred ring full */
    u32 wmi_busy;                   /* WMI control sends queued for lack of credits */
    u32 eapol_busy;                 /* EAPOL replies queued for lack of credits */
    u8  credit_log;
    u32 t_state;                    /* now_ms() when the current wait started */
    u32 reconnect_at;               /* now_ms() for the next WMI_CONNECT, 0 = none */
    u8 scan_done, auth_fails;
    u8 byte_mode;                   /* 1: mailbox through CMD52 (the FIFO self-test failed) */
    u8 verbose, switching, from_ios;
    u8 nonet, rescan;               /* NO_NETWORK_AVAIL failures in a row; a directed rescan is running */
    u8 cancelling;                  /* our WMI_DISCONNECT that ends the firmware's own retries */
    u8 rsn_mode;                    /* RSN_MODE_* of the target AP (scan), see wpa2_rsn_parse */
    u8 wpa1_ap;                     /* AP offers WPA v1 (vendor IE) with TKIP pairwise */
    u16 disc_reason, disc_code;
    u32 link_seq;
    /* [wifi-diag] */
    u8 diag;
    u32 t0;                         /* now_ms() at power-up: diag time base */
    u32 t_part, t_scan, boot_polls, poll_max;
    u32 ev_logged, scans_logged, nbssinfo;
    u8 summary_done;
} A;

static u32 now_ms(void)
{
    u16 hi, lo, hi2;
    do {
        hi = REG_T9_VAL3;
        lo = REG_T9_VAL2;
        hi2 = REG_T9_VAL3;
    } while (hi != hi2);
    return ((u32)hi << 16 | lo) / 65u;      /* 65.457 ticks per ms; 0.7% slow is fine here */
}

static u8 pkt[0x680] __attribute__((aligned(4)));   /* mailbox packet scratch */

#define DIAG(fmt, ...) do { if (A.diag) logp("[wifi-diag] %6u ms " fmt "\n", now_ms() - A.t0, ##__VA_ARGS__); } while (0)

static const char *st_name(u32 s)
{
    static const char *const n[] = { "off", "power", "bmi", "fwload", "boot", "htc", "wmi",
                                     "scan", "auth", "up", "failed", "idle" };
    return s < ARRAY_SIZE(n) ? n[s] : "?";
}

/* up to 24 bytes as hex, for the diag log */
static const char *hex(const u8 *p, u32 n)
{
    static char b[24 * 2 + 4];
    static const char d[] = "0123456789abcdef";
    u32 i, o = 0;
    for (i = 0; i < n && i < 24; i++) { b[o++] = d[p[i] >> 4]; b[o++] = d[p[i] & 15]; }
    if (i < n) { b[o++] = '.'; b[o++] = '.'; }
    b[o] = 0;
    return b;
}

/* The network to be on: from wifi.cfg at boot, or from iOS (WLAN_CMD_JOIN).
   have = 0: stay idle (no wifi.cfg, or iOS said "leave"). */
static struct {
    char ssid[33];
    u8 ssid_len, security, have;
    u8 pmk[32];
} T;

/* every BSS seen in scans since the last user scan (docs/wifi.md 10) */
static wlan_bss_t scan_tab[WLAN_SCAN_MAX];
static u32 nscan, scan_seq;
static u8 user_scan;                /* a WLAN_CMD_SCAN is running */

static int soc_read(u32 addr, u32 *v);
static void wmi_queue_clear(void);
static u32 eapol_pending_len;       /* also cleared by fail() */

/* A bring-up step failed: log it, park the chip in reset (nRESET low), and
   stay "failed". iOS boots on; Settings shows Wi-Fi as unavailable. */
static u32 fail(int err, const char *what)
{
    u32 st = A.state;
    A.err = err;
    A.state = NET_ST_FAILED;
    wmi_queue_clear();
    eapol_pending_len = 0;
    log_err("[wifi] %s failed: %d (sdio status %08x)\n", what, err, sdio_last_status());
    DIAG("FAILED in stage %s: %s, error %d, sdio status %08x", st_name(st), what, err, sdio_last_status());
    if (A.diag && st >= NET_ST_BOOT && st <= NET_ST_WMI) {
        /* the host interest area (0x520000) says how far the firmware got */
        u32 w[32];
        int r = 0;
        for (u32 i = 0; i < 32 && r >= 0; i++) r = soc_read(HOST_INTEREST + 4 * i, &w[i]);
        if (r >= 0)
            for (u32 i = 0; i < 32; i += 8)
                DIAG("host interest +%02x: %08x %08x %08x %08x %08x %08x %08x %08x", 4 * i,
                     w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
        else
            DIAG("host interest unreadable (%d)", r);
    }
    GPIO_WIFI_EN = (u16)(GPIO_WIFI_EN & ~1u);
    DIAG("summary: Wi-Fi stopped at stage %s; chip held in reset; iOS and md0 are not affected", st_name(st));
    return A.state;
}

/* ---- function 1 helpers --------------------------------------------------- */
static int f1_r32(u32 addr, u32 *v)
{
    u32 x = 0;
    for (int i = 0; i < 4; i++) {
        int b = sdio_r8(1, addr + i);
        if (b < 0) return b;
        x |= (u32)b << (8 * i);
    }
    *v = x;
    return 0;
}

static int f1_w32(u32 addr, u32 v)
{
    for (int i = 3; i >= 0; i--) {      /* MSB first: the LSB write triggers */
        int r = sdio_w8(1, addr + i, (u8)(v >> (8 * i)));
        if (r < 0) return r;
    }
    return 0;
}

static int soc_read(u32 addr, u32 *v)
{
    int r = f1_w32(F1_WINDOW_READ_ADDR, addr);
    return r < 0 ? r : f1_r32(F1_WINDOW_DATA, v);
}

static int soc_write(u32 addr, u32 v)
{
    int r = f1_w32(F1_WINDOW_DATA, v);
    return r < 0 ? r : f1_w32(F1_WINDOW_WRITE_ADDR, addr);
}

/* ---- BMI (ROM bootloader) -------------------------------------------------- */
static int bmi_ready(void)
{
    for (int i = 0; i < 20000; i++) {       /* GBATEK: wait for LOCAL_COUNT[4] != 0 */
        int c = sdio_r8(1, F1_COUNT4);
        if (c < 0) return c;
        if (c) return 0;
        waitcycles(335);
    }
    return SDIO_ERR_TIMEOUT;
}

static int bmi_put(const u8 *p, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        int r = sdio_w8(1, F1_MBOX0_BYTE, p[i]);
        if (r < 0) return r;
    }
    return 0;
}

static int bmi_w32(u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    return bmi_put(b, 4);
}

static int bmi_r32(u32 *v)
{
    u32 x = 0;
    for (int i = 0;; i++) {
        int la = sdio_r8(1, F1_RX_LOOKAHEAD_VALID);
        if (la < 0) return la;
        if (la & 1) break;
        if (i > 20000) return SDIO_ERR_TIMEOUT;
        waitcycles(335);
    }
    for (int i = 0; i < 4; i++) {
        int b = sdio_r8(1, F1_MBOX0_BYTE);
        if (b < 0) return b;
        x |= (u32)b << (8 * i);
    }
    *v = x;
    return 0;
}

static int bmi_cmd(u32 cmd, u32 a, u32 b, int nargs)
{
    int r = bmi_ready();
    if (r == 0) r = bmi_w32(cmd);
    if (r == 0 && nargs > 0) r = bmi_w32(a);
    if (r == 0 && nargs > 1) r = bmi_w32(b);
    return r;
}

static int bmi_read_soc(u32 addr, u32 *v)
{
    int r = bmi_cmd(BMI_READ_SOC_REG, addr, 0, 1);
    return r < 0 ? r : bmi_r32(v);
}

static int bmi_write_soc(u32 addr, u32 v) { return bmi_cmd(BMI_WRITE_SOC_REG, addr, v, 2); }

/* one 32-bit BMI_WRITE_MEMORY (host interest words while the ROM runs) */
static int bmi_write_word(u32 addr, u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    int r = bmi_cmd(BMI_WRITE_MEMORY, addr, 4, 2);
    return r < 0 ? r : bmi_put(b, 4);
}

/* one BMI_WRITE_MEMORY / BMI_LZ_DATA chunk (n <= 0x1F0, padded to 4) */
static int bmi_chunk(u32 cmd, u32 addr, const u8 *p, u32 n)
{
    static const u8 zero[4];
    u32 padded = (n + 3) & ~3u;
    int r = cmd == BMI_WRITE_MEMORY ? bmi_cmd(cmd, addr, padded, 2) : bmi_cmd(cmd, padded, 0, 1);
    if (r == 0) r = bmi_put(p, n);
    if (r == 0 && padded != n) r = bmi_put(zero, padded - n);
    return r;
}

/* ---- mailbox packets (HTC framing) ------------------------------------------ */

/* ---- HTC credits -----------------------------------------------------------
   The target tells us in HTC_READY how many credits of how many bytes it has
   (ath6kl htc_ready_msg: cred_cnt, cred_sz). A message consumes
   ceil(padded_len / cred_sz) credits, at least one; the target returns credits
   in the trailer of the messages it sends us (htc_credit_report: eid, credits).
   Writing without credits overruns the firmware's mailbox ("Mbox Full" on the
   AR6014), so the data path queues its frame and the control path waits for a
   report instead. The initial distribution follows ath6kl: every endpoint gets
   enough for one maximum-sized message, the rest goes to the data endpoint. */
static int mbox_recv(u32 *type);
static int pump_msg(u32 type, int n);
static int defer_msg(u32 type, const u8 *body, int n);

/* A burst of WMI control messages (connect, pairwise/group key, synchronize)
   can exhaust the endpoint's credits. The bounded credit wait may return
   BUSY, so queue control messages in order and retry them on the next monitor
   pass; dropping one (typically the group key) breaks DHCP and rekeys. */
#define WMI_QUEUE_MAX 8
static struct { u8 len; u8 b[72]; } WQ[WMI_QUEUE_MAX];
static u8 wq_head, wq_count;

static int mbox_send(u32 type, u32 ack, u32 ctl, const u8 *body, u32 len, int wait);
static void wmi_queue_clear(void) { wq_head = wq_count = 0; }

static void wmi_queue_put(const u8 *b, u32 len)
{
    u8 i = (u8)((wq_head + wq_count) % WMI_QUEUE_MAX);
    WQ[i].len = (u8)len;
    memcpy(WQ[i].b, b, len);
    wq_count++;
}

static int wmi_queue_flush(void)
{
    while (wq_count) {
        u8 i = wq_head;
        int r = mbox_send(A.ep_wmi, 1, 0, WQ[i].b, WQ[i].len, 0);
        if (r == SDIO_ERR_BUSY) return r;       /* credits may return in this pass */
        wq_head = (u8)((wq_head + 1) % WMI_QUEUE_MAX);
        wq_count--;
        if (r < 0) return r;                    /* hard error: the rest stay queued */
    }
    return 0;
}

static u32 credit_need(u32 padded)
{
    return padded > A.cred_sz ? (padded + A.cred_sz - 1) / A.cred_sz : 1;
}

static void credit_assign(u32 ep, u32 max_msg)
{
    if (ep > 7) return;
    u32 per = max_msg > A.cred_sz ? (max_msg + A.cred_sz - 1) / A.cred_sz : 1;
    if (per < 1) per = 1;
    A.ep_cred_per_msg[ep] = (u8)per;
    A.ep_max_msg[ep] = (u16)max_msg;
    u32 take = per < A.cred_pool ? per : A.cred_pool;
    A.ep_credits[ep] += take;
    A.cred_pool -= take;
    DIAG("htc: endpoint %u: %u credit(s) of %u bytes (max message %u), %u left in the pool",
         ep, take, A.cred_sz, max_msg, A.cred_pool);
}

static void credit_init(u32 n, u32 sz)
{
    memset(A.ep_credits, 0, sizeof(A.ep_credits));
    memset(A.ep_cred_per_msg, 0, sizeof(A.ep_cred_per_msg));
    memset(A.ep_max_msg, 0, sizeof(A.ep_max_msg));
    A.cred_sz = sz ? sz : 1664;
    A.cred_pool = n;
    A.credit_rpt = A.credit_ret = A.credit_used = A.credit_low = A.credit_busy = 0;
    A.credit_refund = A.ep0_refill = A.defer_drop = A.defer_full = A.wmi_busy = A.eapol_busy = 0;
    A.credit_log = 0;
    wmi_queue_clear();
    credit_assign(0, A.cred_sz);            /* the control endpoint exists already */
}

/* one RX trailer: records of (id, len, data...); id 1 = credit reports */
static void credit_report(const u8 *t, u32 n)
{
    u32 got = wifi_htc_credits(t, n, A.ep_credits);
    if (!got) return;
    A.credit_rpt += got;
    A.credit_ret += got;
    if (A.credit_log < 16) {
        A.credit_log++;
        DIAG("htc: credit report: +%u credit(s) = %u on ep1/%u on ep2, returned in total %u",
             got, A.ep_credits[A.ep_data ? A.ep_data : 1], A.ep_credits[2], A.credit_ret);
    }
}

/* A credit wait must not re-enter the WMI/HTC state machine (we may already
   be inside its handler). Keep whole packets in a queue and run them after
   the current step, from pump() below. Credit records are already applied by
   mbox_recv() when it strips the trailer. A full ring is not an excuse to
   drop a packet: the caller stops draining and leaves it in the hardware
   mailbox for the next pump(). 16 x 1664 bytes is ~26 KiB of ARM9 RAM. */
#define DEFER_MAX 16u
static struct { u8 type; u16 len; u8 body[0x680]; } D[DEFER_MAX];
static u8 d_head, d_count;

/* 0 = queued, <0 = the packet stays in the mailbox (and is counted). */
static int defer_msg(u32 type, const u8 *body, int n)
{
    if (n <= 0 || (u32)n > sizeof(D[0].body) || d_count >= DEFER_MAX) {
        A.defer_drop++;
        if (A.credit_log < 16) {
            A.credit_log++;
            DIAG("htc: deferred ring full (%u packets), leaving the mailbox packet pending", DEFER_MAX);
        }
        return SDIO_ERR_BUSY;
    }
    u8 i = (u8)((d_head + d_count) % DEFER_MAX);
    D[i].type = (u8)type;
    D[i].len = (u16)n;
    memcpy(D[i].body, body, (u32)n);
    d_count++;
    return 0;
}

static int pump_msg(u32 type, int n)
{
    return defer_msg(type, pkt + 6, n);
}

/* One monitor step may spend at most ~20 ms here (20 x the 1 ms wait below):
   md0, the GPU mirror and input must keep being polled. A control message
   that cannot get credits returns BUSY to its caller, which retries on the
   next pass; data frames never call this at all. */
#define CREDIT_WAIT_POLLS 20u
static int credit_wait(u32 ep, u32 need)
{
    /* Check the ring BEFORE reading: a packet already read from the hardware
       mailbox cannot be put back. A full ring means the next pump() sees it. */
    for (u32 i = 0; i < CREDIT_WAIT_POLLS && A.ep_credits[ep] < need && d_count < DEFER_MAX; i++) {
        u32 type;
        int n = mbox_recv(&type);
        if (n > 0) {
            if (pump_msg(type, n) < 0) break;    /* oversized: leave it in the mailbox */
        } else if (n < 0) return n;
        else waitcycles(3350);
    }
    if (A.ep_credits[ep] < need) {
        if (d_count >= DEFER_MAX) {
            A.defer_full++;
            if (A.credit_log < 16) {
                A.credit_log++;
                DIAG("htc: deferred ring full (%u packets): leaving the mailbox for pump()", DEFER_MAX);
            }
        }
        A.credit_busy++;
        if (A.credit_log < 16)
            DIAG("htc: tx waits for credits on endpoint %u (%u < %u)", ep, A.ep_credits[ep], need);
        return SDIO_ERR_BUSY;
    }
    return 0;
}

static int mbox_send(u32 type, u32 ack, u32 ctl, const u8 *body, u32 len, int wait)
{
    if (len + 6 > sizeof(pkt)) return SDIO_ERR_ARG;
    u32 total = (len + 6 + SDIO_BLOCK - 1) & ~(SDIO_BLOCK - 1);
    u32 need = 0;
    int r = 0, asked = 0;
    if (type < 8) {
        need = credit_need(total);
        if (type == PKT_HTC && A.ep_credits[type] < need) {
            /* The Type4 firmware on a real New 3DS answers CONNECT_SERVICE
               (status 0, endpoint 1) but does not credit EP0 back: the next
               control message waited ~17 ms, failed with SDIO_ERR_BUSY (-5)
               and left iOS with no Wi-Fi interface or Settings toggle
               (issue #11). dsiwifi sends all five CONNECT_SERVICE messages
               plus SETUP_COMPLETE back-to-back, with no credit wait, on the
               same chip. They are a fixed handful of short control messages,
               so fund EP0 locally. A firmware/model that does return the
               credit simply tops EP0 up again. */
            A.ep_credits[type] = (u8)need;
            A.ep0_refill++;
            DIAG("htc: endpoint 0 credit not returned: funding %u for this control message", need);
        }
        if (A.ep_credits[type] < need) {
            if (!wait) {
                A.credit_busy++;
                return SDIO_ERR_BUSY;       /* caller keeps the frame queued */
            }
            r = credit_wait(type, need);
            if (r < 0) return r;
        }
        A.ep_credits[type] -= (u8)need;
        A.credit_used += need;
        if (A.ep_credits[type] < A.ep_cred_per_msg[type]) {
            ack |= HTC_FLG_NEED_CREDIT;     /* ask the target to give them back */
            A.credit_low++;
            asked = 1;
        }
    }
    memset(pkt, 0, total);
    pkt[0] = (u8)type; pkt[1] = (u8)ack;
    pkt[2] = (u8)len; pkt[3] = (u8)(len >> 8);
    pkt[4] = (u8)ctl; pkt[5] = (u8)(ctl >> 8);
    if (len) memcpy(pkt + 6, body, len);
    if (A.byte_mode) {                  /* last byte at the window end = end of message */
        for (u32 i = 0; i < total; i++)
            if ((r = sdio_w8(1, F1_MBOX0_END - total + i, pkt[i])) < 0) goto failed;
    } else {
        r = sdio_write_blocks(1, F1_MBOX0_END - total, pkt, total);
    }
    if (r >= 0) return r;

failed:
    /* The target never saw the message, so it owes us no replacement report:
       put the credits back now, as ath6kl returns cred_used on write failure. */
    if (need) {
        u32 back = need > 255u - A.ep_credits[type] ? 255u - A.ep_credits[type] : need;
        A.ep_credits[type] = (u8)(A.ep_credits[type] + back);
        A.credit_used -= need < A.credit_used ? need : A.credit_used;
        A.credit_refund += need;
        if (asked && A.credit_low) A.credit_low--;
        if (A.credit_log < 16) {
            A.credit_log++;
            DIAG("htc: mailbox write failed (%d): returned %u credits to endpoint %u", r, need, type);
        }
    }
    return r;
}

static int wmi_send(u32 id, const void *params, u32 len)
{
    u8 b[72];
    if (len + 2 > sizeof(b)) return SDIO_ERR_ARG;
    b[0] = (u8)id; b[1] = (u8)(id >> 8);
    if (len) memcpy(b + 2, params, len);
    /* flag 1: credit update wanted; WMI waits briefly, then queues in order */
    if (wq_count) {
        if (wq_count >= WMI_QUEUE_MAX) return SDIO_ERR_BUSY;
        A.wmi_busy++;
        wmi_queue_put(b, len + 2);
        return 0;
    }
    int r = mbox_send(A.ep_wmi, 1, 0, b, len + 2, 1);
    if (r == SDIO_ERR_BUSY && wq_count < WMI_QUEUE_MAX) {
        A.wmi_busy++;
        wmi_queue_put(b, len + 2);
        return 0;
    }
    return r;
}

/* Read one packet into pkt[]. Returns the payload length (bytes after the
   6-byte header, trailer removed), 0 = nothing pending, < 0 = error. */
static int mbox_recv(u32 *type)
{
    u32 la = 0;
    int st = sdio_r8(1, F1_HOST_INT_STATUS);
    if (st < 0) return st;
    if (!(st & 1)) return 0;
    int r = f1_r32(F1_RX_LOOKAHEAD0, &la);
    if (r < 0) return r;
    u32 len = la >> 16;
    u32 total = (len + 6 + SDIO_BLOCK - 1) & ~(SDIO_BLOCK - 1);
    if (total > sizeof(pkt)) {
        log_warn("[wifi] oversized mailbox packet (lookahead %08x)\n", la);
        return SDIO_ERR_ARG;
    }
    if (A.byte_mode) {
        for (u32 i = 0; i < total; i++) {
            int b = sdio_r8(1, F1_MBOX0_END - total + i);
            if (b < 0) return b;
            pkt[i] = (u8)b;
        }
    } else if ((r = sdio_read_blocks(1, F1_MBOX0_END - total, pkt, total)) < 0) {
        return r;
    }
    *type = pkt[0];
    /* credit / lookahead records at the end (ath6kl HTC_FLG_RX_TRAILER) */
    u32 trailer = (pkt[1] & HTC_FLG_RX_TRAILER) ? pkt[4] : 0;
    if (trailer > len) trailer = 0;
    if (trailer) credit_report(pkt + 6 + len - trailer, trailer);
    return trailer < len ? (int)(len - trailer) : 0;
}

/* ---- firmware upload ---------------------------------------------------------- */
static const fw_part_t *fw_find(u32 kind)
{
    const fw_hdr_t *h = (const fw_hdr_t *)fw_buf;
    for (u32 i = 0; i < h->nparts && i < 8; i++)
        if (h->part[i].kind == kind) return &h->part[i];
    return NULL;
}

static int fw_check(void)
{
    const fw_hdr_t *h = (const fw_hdr_t *)fw_buf;
    if (fw_size < sizeof(*h) || h->magic != FW_MAGIC || h->version != 1 || h->nparts > 8) return -100;
    for (u32 i = 0; i < h->nparts; i++)
        if (h->part[i].off > fw_size || h->part[i].len > fw_size - h->part[i].off) return -101;
    if (!fw_find(FW_STUB_DATA) || !fw_find(FW_STUB_CODE) || !fw_find(FW_DATABASE) || !fw_find(FW_MAIN_LZ))
        return -102;
    return 0;
}

/* Upload order (GBATEK "DSi SD/MMC Firmware Wifi Firmware" + dsiwifi): stub
   data, stub code, execute the stub (it reads the calibration EEPROM), main
   (LZ, decompressed by the ROM), database, then point hi_dset_list_head at it.
   One chunk per call so the monitor loop keeps serving block I/O. */
static int fw_step(void)
{
    static const u8 order[] = { FW_STUB_DATA, FW_STUB_CODE, 0xFF, FW_MAIN_LZ, FW_DATABASE };
    if (A.part >= sizeof(order)) return 1;
    u32 kind = order[A.part];
    int r = 0;
    if (kind == 0xFF) {
        const fw_part_t *c = fw_find(FW_STUB_CODE);
        u32 ret = 0;
        u32 t = now_ms();
        r = bmi_cmd(BMI_EXECUTE, c->dst + 0x400000u, c->dst, 2);    /* code mirror, arg as NWM */
        if (r == 0) r = bmi_r32(&ret);
        log_ok("[wifi] stub executed: r=%d ret=%08x\n", r, ret);
        DIAG("fw: BMI_EXECUTE %08x (stub, reads the calibration EEPROM) returned %08x in %u ms: %d",
             c->dst + 0x400000u, ret, now_ms() - t, r);
        A.part++;
        return r;
    }
    const fw_part_t *p = fw_find(kind);
    if (A.part_off == 0) {
        logp("[wifi] fw part %u: %u bytes -> %08x\n", kind, p->len, p->dst);
        A.t_part = now_ms();
        if (kind == FW_MAIN_LZ && (r = bmi_cmd(BMI_LZ_START, p->dst, 0, 1)) < 0) return r;
    }
    u32 n = p->len - A.part_off;
    if (n > 0x100) n = 0x100;
    if (n) {
        r = bmi_chunk(kind == FW_MAIN_LZ ? BMI_LZ_DATA : BMI_WRITE_MEMORY,
                      p->dst + A.part_off, fw_buf + p->off + A.part_off, n);
        if (r < 0) return r;
        A.part_off += n;
    }
    if (A.part_off >= p->len) {
        static const char *const what[] = { "?", "stub data", "stub code", "database", "main (BMI_LZ_DATA)" };
        DIAG("fw: part %u %s: %u bytes -> %08x in %u ms", kind, what[kind < 5 ? kind : 0], p->len, p->dst,
             now_ms() - A.t_part);
        if (kind == FW_DATABASE) r = bmi_write_word(HOST_INTEREST + HI_DSET_LIST_HEAD, p->dst);
        A.part++;
        A.part_off = 0;
    }
    return r;
}

/* ---- WMI ------------------------------------------------------------------------ */
static int wmi_connect(void)
{
    int wpa = T.security == WIFI_SEC_WPA2;
    struct __attribute__((packed)) {
        u8 networkType, dot11AuthMode, authMode, pairwiseCryptoType, pairwiseCryptoLen,
           groupCryptoType, groupCryptoLen, ssidLength;
        u8 ssid[32];
        u16 channel;        /* MHz from our scan; 0 + zero BSSID = let the firmware pick */
        u8 bssid[6];
        u32 ctrl_flags;
    } p;
    memset(&p, 0, sizeof(p));
    p.networkType = 1;                          /* infrastructure */
    p.dot11AuthMode = 1;                        /* open system */
    p.authMode = wpa ? (A.wpa1_ap ? AUTH_WPA_PSK : AUTH_WPA2_PSK) : AUTH_NONE;
    p.pairwiseCryptoType = wpa ? (A.wpa1_ap ? CRYPT_TKIP : CRYPT_AES) : CRYPT_NONE;
    p.groupCryptoType = wpa ? (A.wpa1_ap ? CRYPT_TKIP : A.group_crypt) : CRYPT_NONE;
    p.ssidLength = T.ssid_len;
    memcpy(p.ssid, T.ssid, T.ssid_len);
    if (A.found) {
        p.channel = A.channel;
        memcpy(p.bssid, A.bssid, 6);
        /* ath6kl CONNECT_ASSOC_POLICY_USER | CONNECT_PROFILE_MATCH_DONE: this BSSID,
           the host already matched its security. The firmware's own matching
           answered NO_NETWORK_AVAIL for an AP our scans see fine (run21/22:
           FRITZ!Box on channel 13; WPA2/WPA3 mixed RSN IEs are a suspect). */
        if ((A.nonet & 1) || A.rsn_mode == RSN_MODE_MIXED) p.ctrl_flags = 0x0001 | 0x0008;
    }
    /* dsiwifi's pre-connect settings: max performance power mode, 5 s
       disconnect timeout, no keepalive */
    u8 v = 2;
    wmi_send(WMI_SET_POWER_MODE, &v, 1);
    v = 5;
    wmi_send(WMI_SET_DISC_TIMEOUT, &v, 1);
    v = 0;
    wmi_send(WMI_SET_KEEPALIVE, &v, 1);
    return wmi_send(WMI_CONNECT, &p, sizeof(p));
}

static int wmi_add_key(u32 idx, u32 usage, const u8 *key, u32 keylen, const u8 *rsc)
{
    struct __attribute__((packed)) {
        u8 keyIndex, keyType, keyUsage, keyLength;
        u8 keyRSC[8], key[32];
        u8 key_op_ctrl;
    } p;
    memset(&p, 0, sizeof(p));
    p.keyIndex = (u8)idx; p.keyUsage = (u8)usage; p.keyLength = (u8)keylen;
    p.keyType = keylen == 32 ? CRYPT_TKIP : CRYPT_AES;
    if (rsc) memcpy(p.keyRSC, rsc, 8);
    memcpy(p.key, key, keylen < 16 ? keylen : 16);
    if (keylen == 32) {                 /* TKIP: the firmware wants the MIC keys swapped (dsiwifi) */
        memcpy(p.key + 16, key + 24, 8);
        memcpy(p.key + 24, key + 16, 8);
    }
    p.key_op_ctrl = 3;
    return wmi_send(WMI_ADD_CIPHER_KEY, &p, sizeof(p));
}

static void link_up(void)
{
    u8 one = 1;
    wmi_send(WMI_SYNCHRONIZE, &one, 1);
    A.state = NET_ST_UP;
    A.auth_fails = 0;
    A.nonet = 0;
    DIAG("htc: credits so far: used %u, returned %u, reports %u, low-credit flags %u, waits %u, EP0 refills %u",
         A.credit_used, A.credit_ret, A.credit_rpt, A.credit_low, A.credit_busy, A.ep0_refill);
    log_ok("[wifi] link up: %s, AP %02x:%02x:%02x:%02x:%02x:%02x\n", T.ssid,
           A.bssid[0], A.bssid[1], A.bssid[2], A.bssid[3], A.bssid[4], A.bssid[5]);
}

static void snonce(u8 out[32])
{
    /* No TRNG on the ARM9: hash the boot ROM's PRNG register, timer and
       everything the chip told us so far. Good enough for a nonce. */
    sha1_ctx c;
    u8 d[20];
    u32 v[8];
    for (int half = 0; half < 2; half++) {
        for (int i = 0; i < 8; i++) v[i] = REG_PRNG ^ (u32)REG_TIMER0 << 16 ^ sdio_last_status();
        sha1_init(&c);
        sha1_update(&c, (const u8 *)v, sizeof(v));
        sha1_update(&c, A.mac, 6);
        sha1_update(&c, A.bssid, 6);
        sha1_update(&c, out, 16);
        sha1_final(&c, d);
        memcpy(out + 16 * half, d, 16);
    }
}

/* An EAPOL reply is a data-frame send: never wait for credits in the monitor
   step (on_eapol() is called from pump()). wpa2_rx_eapol() has already
   advanced the handshake when it produced the reply, so keep the framed
   reply here and retry it once per pass until the target reports credits. */
#define EAPOL_OUT_MAX (2 + 14 + 8 + 160)
static u8 eapol_pending[EAPOL_OUT_MAX];

static int eapol_send(const u8 *out, u32 n)
{
    int r = n ? mbox_send(A.ep_data, 0, 0, out, n, 0) : SDIO_ERR_ARG;
    if (r == SDIO_ERR_BUSY && n <= sizeof(eapol_pending)) {
        memcpy(eapol_pending, out, n);
        eapol_pending_len = n;
        A.eapol_busy++;
    }
    return r;
}

static void eapol_pending_step(void)
{
    if (!eapol_pending_len) return;
    if (A.state != NET_ST_AUTH && A.state != NET_ST_UP) {
        eapol_pending_len = 0;              /* disconnected: AP retries the handshake */
        return;
    }
    int r = mbox_send(A.ep_data, 0, 0, eapol_pending, eapol_pending_len, 0);
    if (r == 0) {
        eapol_pending_len = 0;
    } else if (r != SDIO_ERR_BUSY) {
        log_warn("[wifi] queued EAPOL reply failed: %d\n", r);
        eapol_pending_len = 0;
    }
}

static void on_eapol(const u8 *eth, u32 len)
{
    u8 frame[14 + 160], out[EAPOL_OUT_MAX];
    u32 rlen = 0;
    /* PTK inputs straight from the frame: the authenticator is its source,
       the supplicant its destination (not what the CONNECT event parsing says) */
    if (len >= 14 + 7 && !(eth[14 + 5] & 0x01) && (eth[14 + 6] & 0x80)) {      /* message 1: ACK, no MIC */
        if (memcmp(eth + 6, A.wpa.ap, 6)) {
            log_warn("[wifi] EAPOL source %02x:%02x:%02x:%02x:%02x:%02x != BSSID from CONNECT: using the source\n",
                     eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);
            memcpy(A.wpa.ap, eth + 6, 6);
            memcpy(A.bssid, eth + 6, 6);
        }
        if (!(eth[0] & 1) && memcmp(eth, A.wpa.sta, 6)) {
            log_warn("[wifi] EAPOL destination %02x:%02x:%02x:%02x:%02x:%02x != our MAC: using it\n",
                     eth[0], eth[1], eth[2], eth[3], eth[4], eth[5]);
            memcpy(A.wpa.sta, eth, 6);
        }
        DIAG("eapol m1: key info %02x%02x, descriptor %u, replay %02x%02x%02x%02x%02x%02x%02x%02x, key data %u",
             eth[14 + 5], eth[14 + 6], eth[14 + 4], eth[14 + 9], eth[14 + 10], eth[14 + 11], eth[14 + 12],
             eth[14 + 13], eth[14 + 14], eth[14 + 15], eth[14 + 16],
             len >= 14 + 99 ? (u32)eth[14 + 97] << 8 | eth[14 + 98] : 0);
    }
    int ev = wpa2_rx_eapol(&A.wpa, eth + 14, len - 14, frame + 14, sizeof(frame) - 14, &rlen);
    if (ev < 0) {
        log_warn("[wifi] EAPOL-Key rejected: %d\n", ev);
        return;
    }
    if (ev == WPA2_EV_NONE) return;
    memcpy(frame, A.bssid, 6);
    memcpy(frame + 6, A.mac, 6);
    frame[12] = 0x88; frame[13] = 0x8E;
    u32 n = wifi_frame_to_wmi(frame, 14 + rlen, out, sizeof(out));
    int r = eapol_send(out, n);     /* data endpoint: queue on BUSY, never wait */
    if (ev == WPA2_EV_GTK && A.state == NET_ST_UP)
        logp("[wifi] group key renewed by the AP (index %u): %d\n", A.wpa.gtk_idx, r);
    else
        logp("[wifi] EAPOL event %d, reply %u bytes: %d\n", ev, rlen, r);
    if (ev == WPA2_EV_KEYS || ev == WPA2_EV_GTK) {
        if (ev == WPA2_EV_KEYS)
            wmi_add_key(0, 0, A.wpa.tk, A.wpa.wpa1 ? 32 : 16, NULL);           /* pairwise */
        wmi_add_key(A.wpa.gtk_idx, 1, A.wpa.gtk, A.wpa.gtk_len, A.wpa.rsc);     /* group */
        if (A.state != NET_ST_UP) link_up();
    }
}

/* SET_BSS_FILTER(1 = all BSSs) + a specific probed SSID (hidden networks) +
   START_SCAN on all channels; parameter layouts as in dsiwifi */
static int scan_start(void)
{
    struct __attribute__((packed)) { u8 filter, a1; u16 a2; u32 ieMask; } f = { 1, 0, 0, 0 };
    struct __attribute__((packed)) { u8 index, flag, len; u8 ssid[32]; } ps;
    struct __attribute__((packed)) {
        u32 forceFgScan, isLegacy, homeDwellTime, forceScanInterval;
        u8 scanType, numChannels; u16 channelList;
    } sc = { 0, 0, 20, 0, 0, 0, 0 };
    memset(&ps, 0, sizeof(ps));
    if (T.have) {                               /* probe for it too (hidden networks) */
        ps.flag = 1;                            /* SPECIFIC_SSID_FLAG */
        ps.len = T.ssid_len;
        memcpy(ps.ssid, T.ssid, T.ssid_len);
    }
    int r = wmi_send(WMI_SET_BSS_FILTER, &f, sizeof(f));
    if (r >= 0) r = wmi_send(WMI_SET_PROBED_SSID, &ps, sizeof(ps));
    if (r >= 0) r = wmi_send(WMI_START_SCAN, &sc, sizeof(sc));
    A.scan_done = 0;
    A.t_state = A.t_scan = now_ms();
    A.nbssinfo = 0;
    DIAG("scan: SET_BSS_FILTER 1 (all), SET_PROBED_SSID \"%s\", START_SCAN all channels (home dwell 20 ms): %d",
         T.have ? T.ssid : "", r);
    return r;
}

/* WMI_BSSINFO_EVENT: u16 channel, u8 frametype, u8 snr, s16 rssi, u8 bssid[6],
   u32 ieMask, then the beacon/probe-response body (8 timestamp, 2 interval,
   2 capability, IEs). Remember our SSID's channel, BSSID and group cipher. */
static void scan_record(const u8 *d, u32 n)
{
    const u8 *ie = d + 28, *end = d + n;
    wlan_bss_t *b = NULL;
    u32 i;
    for (i = 0; i < nscan; i++)
        if (!memcmp(scan_tab[i].bssid, d + 6, 6)) b = &scan_tab[i];
    int fresh = !b;
    if (!b) {
        if (nscan >= WLAN_SCAN_MAX) return;
        b = &scan_tab[nscan++];
    }
    memset(b, 0, sizeof(*b));
    memcpy(b->bssid, d + 6, 6);
    b->channel_mhz = (u16)(d[0] | d[1] << 8);
    b->rssi = (int16_t)(d[4] | d[5] << 8);
    b->noise = (int16_t)(b->rssi - (int8_t)d[3]);
    b->beacon_int = (u16)(d[24] | d[25] << 8);
    b->caps = (u16)(d[26] | d[27] << 8);
    if (b->caps & 0x10) b->flags |= WLAN_BSS_PRIVACY;
    for (const u8 *p = ie; p + 2 <= end && p + 2 + p[1] <= end; p += 2 + p[1]) {
        if (p[0] == 0 && p[1] <= 32 && !b->ssid_len) {
            b->ssid_len = p[1];
            memcpy(b->ssid, p + 2, p[1]);
        }
        if (p[0] == 0x30) b->flags |= WLAN_BSS_RSN;
        if (p[0] == 0xDD && p[1] >= 6 && p[2] == 0x00 && p[3] == 0x50 && p[4] == 0xF2 && p[5] == 1)
            b->flags |= WLAN_BSS_WPA;
    }
    /* WLAN_IE_MAX is small: home routers send 400+ IE bytes (country with one
       triplet per channel, HT/VHT caps, vendor IEs). Copy the IEs iOS and our
       supplicant need first (SSID, rates, DS, RSN, WPA, extended rates), then
       the rest while room is left. Before, a long country/HT IE ahead of the
       RSN IE pushed it out: iOS saw a privacy bit without RSN/WPA, took the
       network for WEP and sent the passphrase as a WEP key (issue 1). */
    u32 dropped = 0;
    for (int pass = 0; pass < 2; pass++)
        for (const u8 *p = ie; p + 2 <= end && p + 2 + p[1] <= end; p += 2 + p[1]) {
            int key = p[0] == 0 || p[0] == 1 || p[0] == 3 || p[0] == 0x30 || p[0] == 50 ||
                      (p[0] == 0xDD && p[1] >= 6 && p[2] == 0x00 && p[3] == 0x50 && p[4] == 0xF2 && p[5] == 1);
            if (key != !pass) continue;
            if ((u32)b->ie_len + 2 + p[1] <= WLAN_IE_MAX) {
                memcpy(b->ie + b->ie_len, p, 2 + p[1]);
                b->ie_len = (u8)(b->ie_len + 2 + p[1]);
            } else if (key) {
                dropped++;
            }
        }
    if (dropped && fresh && A.scans_logged < 3)
        DIAG("scan: %u needed IE(s) did not fit in %u bytes", dropped, WLAN_IE_MAX);
    /* the first three scans: one line per network (BSSINFO layout, RSSI
       units and channels are model assumptions, docs/wifi.md 6.1) */
    if (fresh && A.scans_logged < 3) {
        char ssid[33];
        u32 mhz = b->channel_mhz, ch = mhz == 2484 ? 14 : mhz >= 2412 && mhz < 2484 ? (mhz - 2407) / 5 : 0;
        memcpy(ssid, b->ssid, b->ssid_len);
        ssid[b->ssid_len] = 0;
        DIAG("scan: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x %u MHz (ch %u) rssi %d snr %u %s, %s, %u IE bytes",
             ssid, b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], mhz, ch,
             b->rssi, d[3], d[2] == 1 ? "beacon" : d[2] == 2 ? "probe resp" : "frame ?",
             (b->flags & WLAN_BSS_RSN) ? "RSN" : (b->flags & WLAN_BSS_WPA) ? "WPA" :
             (b->flags & WLAN_BSS_PRIVACY) ? "WEP" : "open", n - 28);
    }
}

static void on_bssinfo(const u8 *d, u32 n)
{
    A.nbssinfo++;
    if (n < 28) {
        DIAG("scan: short BSSINFO (%u bytes): %s", n, hex(d, n));
        return;
    }
    scan_record(d, n);
    if (!T.have) return;
    const u8 *ie = d + 28, *end = d + n;
    int match = 0, group = CRYPT_AES, rsn = 0, wpa1 = 0;
    while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        if (ie[0] == 0 && ie[1] == T.ssid_len && !memcmp(ie + 2, T.ssid, ie[1])) match = 1;
        if (ie[0] == 0x30 && ie[1] >= 6) {                /* RSN group suite 00-0F-AC-xx */
            rsn = 1;
            group = ie[7] == 2 ? CRYPT_TKIP : CRYPT_AES;
        }
        if (!rsn && ie[0] == 0xDD && ie[1] >= 10 && !memcmp(ie + 2, "\x00\x50\xF2\x01", 4))
            wpa1 = 1;                                    /* WPA v1 vendor IE */
        ie += 2 + ie[1];
    }
    if (!match || (A.found && d[3] <= A.found)) return;   /* keep the strongest (by SNR) */
    A.found = d[3] ? d[3] : 1;
    A.channel = (u16)(d[0] | d[1] << 8);
    memcpy(A.bssid, d + 6, 6);
    A.wpa1_ap = (u8)(wpa1 && !rsn);                       /* mixed WPA+WPA2: use RSN */
    if (A.wpa1_ap) {
        group = CRYPT_TKIP;                               /* WPA v1 is TKIP pairwise */
        DIAG("scan: WPA v1 vendor IE, TKIP pairwise");
    }
    A.group_crypt = (u8)group;
}

static void target_lookup(void);

static void do_connect(void)
{
    if (A.found && T.security == WIFI_SEC_WPA2 && A.rsn_mode == RSN_MODE_WPA3_ONLY) {
        /* SAE / required PMF: not in the AR6014 firmware, and no WPA2 fallback offered */
        log_err("[wifi] \"%s\" is WPA3-only (or requires protected management frames): the 3DS Wi-Fi chip "
                "can't join it. Set the router to \"WPA2 + WPA3\" or \"WPA2\".\n", T.ssid);
        T.have = 0;
        A.state = NET_ST_IDLE;
        A.err = -95;
        return;
    }
    if (A.found && A.rsn_mode == RSN_MODE_MIXED)
        logp("[wifi] \"%s\" is a WPA2 + WPA3 network: joining with WPA2-PSK\n", T.ssid);
    int r = wmi_connect();
    logp("[wifi] connect \"%s\" (%s%s), %s: %d\n", T.ssid,
         T.security == WIFI_SEC_WPA2 ? (A.wpa1_ap ? "WPA-PSK/TKIP" : "WPA2-PSK/CCMP") : "open",
         A.group_crypt == CRYPT_TKIP ? ", TKIP group" : "",
         A.found ? "seen in scan" : "not seen in scan, firmware picks", r);
    A.state = r < 0 ? NET_ST_FAILED : NET_ST_SCAN;
    A.err = r;
    A.scan_done = 1;                            /* scan over: no scan timeout */
    A.reconnect_at = 0;
    A.t_state = now_ms();
}

static void on_wmi(const u8 *b, u32 len)
{
    if (len < 2) return;
    u32 id = b[0] | (u32)b[1] << 8;
    const u8 *d = b + 2;
    u32 n = len - 2;
    switch (id) {
    case WMI_EV_READY: {
        if (n >= 6) memcpy(A.mac, d, 6);
        log_ok("[wifi] WMI ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
               A.mac[0], A.mac[1], A.mac[2], A.mac[3], A.mac[4], A.mac[5]);
        DIAG("wmi: WMI_READY %u ms after HTC setup, %u bytes: %s (MAC, phy capability%s)",
             now_ms() - A.t_state, n, hex(d, n), n > 7 ? ", more" : "");
        if (A.diag) {
            int r = wmi_send(WMI_GET_CHANNEL_LIST, NULL, 0);
            DIAG("wmi: GET_CHANNEL_LIST sent: %d", r);
        }
        u32 v = 0x7F;
        wmi_send(WMI_TARGET_ERROR_BITMASK, &v, 4);
        v = 0;                                  /* GBATEK: timeout 0 = no heartbeat (Type4 firmware needs this) */
        wmi_send(WMI_SET_HEARTBEAT_TIMEOUT, &v, 4);
        A.group_crypt = CRYPT_AES;
        if (!A.summary_done) {
            A.summary_done = 1;
            DIAG("summary: bring-up OK in %u ms (power, SDIO, BMI, firmware, HTC, WMI)", now_ms() - A.t0);
        }
        if (!T.have) {
            logp("[wifi] idle: no network configured, waiting for iOS (Settings > Wi-Fi)\n");
            A.state = NET_ST_IDLE;
            break;
        }
        int r = scan_start();
        logp("[wifi] scanning for \"%s\": %d\n", T.ssid, r);
        A.state = r < 0 ? NET_ST_FAILED : NET_ST_SCAN;
        A.err = r;
        break;
    }
    case WMI_EV_BSSINFO:
        on_bssinfo(d, n);
        break;
    case WMI_EV_SCAN_COMPLETE:
        if (A.scans_logged < 10)
            DIAG("scan: SCAN_COMPLETE after %u ms, status %s, %u BSSINFO events, %u networks",
                 now_ms() - A.t_scan, n >= 4 ? hex(d, 4) : "-", A.nbssinfo, nscan);
        A.scans_logged++;
        if (user_scan) {
            user_scan = 0;
            scan_seq++;
            logp("[wifi] scan done: %u networks\n", nscan);
        }
        if (A.state == NET_ST_SCAN && !A.scan_done) {
            A.cancelling = 0;
            /* use what this scan found (run22: a join that had to scan first
               still connected "not seen in scan" with the BSSID unknown) */
            if (T.have) target_lookup();
            if (A.rescan) {                     /* after NO_NETWORK_AVAIL: every other try the */
                A.rescan = 0;                   /* firmware picks the AP, except for WPA2+WPA3 */
                if (!(A.nonet & 1) && A.rsn_mode != RSN_MODE_MIXED) A.found = 0;
            }
            if (T.have) do_connect();
        }
        break;
    case WMI_EV_CONNECT:
        /* u16 channel; u8 bssid[6]; u16 listen, beacon; u32 nw_type; u8 beacon_ie_len,
           assoc_req_len, assoc_resp_len; assoc_info[] (ath6kl struct wmi_connect_event) */
        if (n >= 8) memcpy(A.bssid, d + 2, 6);
        logp("[wifi] associated, channel %u MHz, BSSID %02x:%02x:%02x:%02x:%02x:%02x\n",
             n >= 2 ? d[0] | (u32)d[1] << 8 : 0, A.bssid[0], A.bssid[1], A.bssid[2], A.bssid[3],
             A.bssid[4], A.bssid[5]);
        if (T.security == WIFI_SEC_WPA2) {
            u8 sn[32];
            memset(sn, 0, sizeof(sn));
            snonce(sn);
            if (A.wpa1_ap) wpa1_start(&A.wpa, T.pmk, A.mac, A.bssid, sn);
            else wpa2_start(&A.wpa, T.pmk, A.mac, A.bssid, sn);
            /* our RSN / WPA IE as the firmware put it into the association request */
            if (n >= 19) {
                u32 bl = d[16], ql = d[17], rl = d[18];
                const u8 *q = d + 19 + bl;
                DIAG("connect: beacon IEs %u, assoc req %u, assoc resp %u bytes", bl, ql, rl);
                if (19 + bl + ql <= n) {
                    for (u32 start = 4; start <= 10 && !A.wpa.own_ie_len; start += 6)   /* assoc / reassoc */
                        for (u32 i = start; i + 2 <= ql; i += 2 + q[i + 1]) {
                            if (i + 2 + q[i + 1] > ql) break;
                            if ((q[i] == (A.wpa1_ap ? 0xDD : 0x30)) && (u32)(q[i + 1] + 2) <= sizeof(A.wpa.own_ie) &&
                                (!A.wpa1_ap || (q[i + 1] >= 10 &&
                                 !memcmp(q + i + 2, "\x00\x50\xF2\x01", 4)))) {
                                A.wpa.own_ie_len = (u8)(q[i + 1] + 2);
                                memcpy(A.wpa.own_ie, q + i, A.wpa.own_ie_len);
                                break;
                            }
                        }
                }
                if (A.wpa.own_ie_len) {
                    char hx[2 * 40 + 1];
                    for (u32 i = 0; i < A.wpa.own_ie_len; i++) snprintf(hx + 2 * i, 3, "%02x", A.wpa.own_ie[i]);
                    DIAG("connect: our association RSN IE %s (used in message 2)", hx);
                } else {
                    DIAG("connect: no RSN IE found in the association request: message 2 uses our own");
                }
            }
            A.wpa.group_cipher = A.group_crypt == CRYPT_TKIP ? 2 : 4;   /* must match our association IE */
            A.state = NET_ST_AUTH;
        } else {
            link_up();
        }
        break;
    case WMI_EV_DISCONNECT:
        /* u16 802.11 reason/status, u8 bssid[6], u8 WMI reason (ath6kl) */
        A.disc_reason = n >= 9 ? d[8] : 0;
        A.disc_code = n >= 2 ? (u16)(d[0] | d[1] << 8) : 0;
        A.link_seq++;
        if (A.state == NET_ST_AUTH && ++A.auth_fails == 3)
            log_err("[wifi] the WPA2 handshake failed 3 times: wrong password?\n");
        if (A.cancelling && A.disc_reason == 3) {   /* the answer to our cancel: rescan runs */
            A.cancelling = 0;
            break;
        }
        if (!T.have) {                          /* iOS asked to leave (or never joined) */
            logp("[wifi] disconnected (WMI reason %u): idle\n", A.disc_reason);
            A.state = NET_ST_IDLE;
            break;
        }
        if (A.switching) {                      /* leaving the old network for a new one */
            A.switching = 0;
            A.state = NET_ST_SCAN;
            A.scan_done = 1;
            A.reconnect_at = 1;
            break;
        }
        if (A.auth_fails >= 3 && A.from_ios) {  /* a wrong password typed in Settings: give up */
            log_warn("[wifi] disconnected (WMI reason %u, 802.11 code %u): giving up on \"%s\"\n",
                     A.disc_reason, A.disc_code, T.ssid);
            T.have = 0;
            A.state = NET_ST_IDLE;
            break;
        }
        if (A.disc_reason == 1 && A.state == NET_ST_SCAN) {
            /* NO_NETWORK_AVAIL: the firmware did not find the AP (run21: a weak AP
               on channel 13 failed every blind retry). Search for it by name
               again, then connect: odd tries to the BSSID just seen, even tries
               let the firmware pick. After 8 in a row, back off to 5 s. */
            A.nonet++;
            if (A.nonet <= 8 || !(A.nonet & 3)) {
                log_warn("[wifi] \"%s\" not found by the firmware (try %u): searching for it, then %s\n",
                         T.ssid, A.nonet, A.nonet & 1 ? "connecting to the BSSID seen" : "letting the firmware pick");
                A.rescan = 1;
                A.reconnect_at = 0;
                /* the firmware keeps retrying this connect on its own and then
                   ignores START_SCAN (run22: no scan results after the first
                   failure, not even for other networks): cancel it first */
                A.cancelling = 1;
                wmi_send(WMI_DISCONNECT, &(u32){0}, 4);
                if (scan_start() >= 0) break;
                A.rescan = 0;
            }
        }
        log_warn("[wifi] disconnected (WMI reason %u, 802.11 code %u), reconnecting in 5 s\n",
                 A.disc_reason, A.disc_code);
        A.state = NET_ST_SCAN;
        A.scan_done = 1;
        A.reconnect_at = now_ms() + 5000;
        if (!A.reconnect_at) A.reconnect_at = 1;
        break;
    case WMI_EV_REG_DOMAIN:
        DIAG("wmi: REG_DOMAIN %s", hex(d, n));
        break;
    case WMI_EV_CMD_ERROR:
        /* u16 command id, u8 error (ath6kl wmi_cmd_error_event) */
        log_warn("[wifi] WMI command %04x rejected by the firmware (error %u)\n",
                 n >= 2 ? d[0] | (u32)d[1] << 8 : 0, n >= 3 ? d[2] : 0);
        break;
    case WMI_GET_CHANNEL_LIST: {
        /* u8 reserved, u8 count, u16 MHz[count]: as a 2.4 GHz channel mask */
        u32 cnt = n >= 2 ? d[1] : 0, mask = 0, other = 0;
        for (u32 i = 0; i < cnt && 2 + 2 * i + 1 < n; i++) {
            u32 mhz = d[2 + 2 * i] | (u32)d[3 + 2 * i] << 8;
            if (mhz == 2484) mask |= 1u << 14;
            else if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) mask |= 1u << ((mhz - 2407) / 5);
            else other++;
        }
        DIAG("wmi: channel list: %u channels, 2.4 GHz mask %04x (bit n = channel n), %u others", cnt, mask, other);
        break;
    }
    default:
        if (A.verbose) logp("[wifi] WMI event %04x, %u bytes\n", id, n);
        else if (A.ev_logged < 24) DIAG("wmi: event %04x, %u bytes: %s", id, n, hex(d, n));
        A.ev_logged++;
        break;
    }
}

static void on_htc(const u8 *b, u32 len)
{
    if (len < 2) return;
    u32 id = b[0] | (u32)b[1] << 8;
    if (id != HTC_READY) return;
    /* connect the control + 4 data services, then SETUP_COMPLETE (dsiwifi htc.twl.c) */
    static const u8 svc[5][8] = {
        { HTC_CONN_SVC, 0, 0, 1, 0, 0, 0, 0 }, { HTC_CONN_SVC, 0, 1, 1, 5, 0, 0, 0 },
        { HTC_CONN_SVC, 0, 2, 1, 5, 0, 0, 0 }, { HTC_CONN_SVC, 0, 3, 1, 5, 0, 0, 0 },
        { HTC_CONN_SVC, 0, 4, 1, 5, 0, 0, 0 },
    };
    static const u8 done[2] = { HTC_SETUP_COMPLETE, 0 };
    u8 eid[5] = { 0 };
    int r = 0;
    u32 ncred = len >= 6 ? (u32)b[2] | (u32)b[3] << 8 : 0;
    u32 csize = len >= 6 ? (u32)b[4] | (u32)b[5] << 8 : 0;
    logp("[wifi] HTC ready: %u credits of %u bytes\n", ncred, csize);
    DIAG("htc: HTC_READY %u ms after the firmware started, %u bytes: %s", now_ms() - A.t_state, len, hex(b, len));
    credit_init(ncred, csize);
    for (int i = 0; i < 5 && r >= 0; i++) {
        u32 type;
        r = mbox_send(PKT_HTC, 0, 0, svc[i], 8, 1);
        int t;
        for (t = 0; r >= 0 && t < 2000; t++) {          /* its response */
            int got = mbox_recv(&type);
            if (got < 0) { r = got; break; }
            /* CONNECT_SERVICE_RESP: id 3, service, status, endpoint, max size */
            const u8 *q = pkt + 6;
            if (got >= 6 && type == PKT_HTC && q[0] == 3 && q[2] == svc[i][2] && q[3] == svc[i][3]) {
                if (q[4] != 0 || q[5] == 0 || q[5] > 7) r = -120 - q[4];
                eid[i] = q[5];
                u32 maxmsg = got >= 8 ? (u32)q[6] | (u32)q[7] << 8 : A.cred_sz;
                credit_assign(q[5], maxmsg);
                DIAG("htc: service %02x%02x -> status %u, endpoint %u, max message %u (%u polls)",
                     q[3], q[2], q[4], q[5], maxmsg, t);
                break;
            }
            if (got == 0) waitcycles(3350);             /* nothing, or a credit report */
        }
        if (r >= 0 && t >= 2000) r = SDIO_ERR_TIMEOUT;
    }
    A.ep_wmi = eid[0];
    A.ep_data = eid[1];
    /* ath6kl gives every endpoint one credit and the rest to the BE data
       endpoint; the target model uses the same split. */
    if (r >= 0 && A.cred_pool && A.ep_data) {
        A.ep_credits[A.ep_data] = (u8)((u32)A.ep_credits[A.ep_data] + A.cred_pool);
        DIAG("htc: endpoint %u gets the remaining %u credits (%u total)", A.ep_data, A.cred_pool,
             A.ep_credits[A.ep_data]);
        A.cred_pool = 0;
    }
    if (r >= 0) r = mbox_send(PKT_HTC, 0, 0, done, 2, 1);
    if (r >= 0) r = f1_w32(F1_INT_STATUS_EN, 0x010300D1);
    logp("[wifi] HTC services connected: %d (WMI endpoint %u, data %u/%u/%u/%u)\n", r,
         eid[0], eid[1], eid[2], eid[3], eid[4]);
    A.state = r < 0 ? NET_ST_FAILED : NET_ST_WMI;
    A.t_state = now_ms();
    A.err = r;
    if (r < 0) {
        A.state = NET_ST_HTC;
        fail(r, "HTC service setup");
    }
}

/* One decoded mailbox message. Data frames end up in A.rxframe. */
static void pump_body(u32 type, const u8 *body, u32 n)
{
    if (type == PKT_HTC) {
        on_htc(body, n);
    } else if (type == A.ep_wmi) {
        on_wmi(body, n);
    } else if (A.ep_wmi) {                              /* the data endpoints */
        static u8 eth[NET_FRAME_MAX];
        u16 et = 0;
        u32 len = wifi_frame_from_wmi(body, n, eth, sizeof(eth), &et);
        if (!len) return;
        if (et == 0x888E) {
            /* also while up: group key rekeys (and PTK rekeys) arrive then; ignoring
               them got us deauthenticated with 802.11 reason 16 (hardware run 23) */
            if (T.security == WIFI_SEC_WPA2 && (A.state == NET_ST_AUTH || A.state == NET_ST_UP))
                on_eapol(eth, len);
        } else if (A.state == NET_ST_UP && !A.rxlen) {
            memcpy(A.rxframe, eth, len);
            A.rxlen = len;
        }
    }
}

/* Read the mailbox and run both fresh and deferred messages. */
static void pump(void)
{
    u32 type = 0;
    int n = mbox_recv(&type);
    if (n > 0) {
        u8 body[0x680];
        memcpy(body, pkt + 6, (u32)n);
        pump_body(type, body, (u32)n);
    } else if (n < 0 && A.verbose) {
        log_warn("[wifi] mailbox read: %d\n", n);
    }
    while (d_count) {
        u8 i = d_head;
        u32 t = D[i].type;
        u32 l = D[i].len;
        u8 body[0x680];
        memcpy(body, D[i].body, l);
        d_head = (u8)((d_head + 1) % DEFER_MAX);
        d_count--;
        pump_body(t, body, l);
    }
}

/* ---- iOS control plane (WLAN_CMD_*, docs/wifi.md section 10) ----------------------- */
static int fw_up(void)
{
    return A.state == NET_ST_IDLE || A.state == NET_ST_SCAN || A.state == NET_ST_AUTH ||
           A.state == NET_ST_UP;
}

/* associated or a WMI_CONNECT outstanding (the firmware will report back) */
static int connecting(void)
{
    return A.state == NET_ST_AUTH || A.state == NET_ST_UP ||
           (A.state == NET_ST_SCAN && A.scan_done && !A.reconnect_at);
}

static int user_scan_start(void)
{
    struct __attribute__((packed)) { u8 filter, a1; u16 a2; u32 ieMask; } f = { 1, 0, 0, 0 };
    struct __attribute__((packed)) {
        u32 forceFgScan, isLegacy, homeDwellTime, forceScanInterval;
        u8 scanType, numChannels; u16 channelList;
    } sc = { 0, 0, 20, 0, 0, 0, 0 };
    nscan = 0;
    user_scan = 1;
    A.t_scan = now_ms();
    A.nbssinfo = 0;
    int r = wmi_send(WMI_SET_BSS_FILTER, &f, sizeof(f));
    if (r >= 0) r = wmi_send(WMI_START_SCAN, &sc, sizeof(sc));
    if (A.scans_logged < 10)
        DIAG("scan: request from iOS: SET_BSS_FILTER 1 (all), START_SCAN all channels, no probed SSID: %d", r);
    return r;
}

/* choose channel, BSSID and group cipher for T from the scan table (strongest) */
static void target_lookup(void)
{
    const wlan_bss_t *best = NULL;
    for (u32 i = 0; i < nscan; i++) {
        const wlan_bss_t *b = &scan_tab[i];
        if (b->ssid_len == T.ssid_len && !memcmp(b->ssid, T.ssid, T.ssid_len) &&
            (!best || b->rssi > best->rssi))
            best = b;
    }
    A.found = 0;
    A.group_crypt = CRYPT_AES;
    A.rsn_mode = RSN_MODE_WPA2;
    A.wpa1_ap = 0;
    if (!best) return;
    A.found = 1;
    A.channel = best->channel_mhz;
    memcpy(A.bssid, best->bssid, 6);
    int saw_rsn = 0;
    for (u32 i = 0; i + 2 <= best->ie_len && i + 2 + best->ie[i + 1] <= WLAN_IE_MAX;
         i += 2 + best->ie[i + 1]) {
        if (best->ie[i] == 0x30 && best->ie[i + 1] >= 6) {
            rsn_info_t ri;
            saw_rsn = 1;                        /* a PSK/SAE RSN IE wins over WPA v1 */
            A.rsn_mode = (u8)wpa2_rsn_parse(best->ie + i, best->ie_len - i, &ri);
            A.group_crypt = (ri.ok ? ri.group : best->ie[i + 7]) == 2 ? CRYPT_TKIP : CRYPT_AES;
            u32 l = 2 + best->ie[i + 1];
            if (l > 40) l = 40;
            if (i + l > best->ie_len) l = best->ie_len - i;
            char hx[2 * 40 + 1];
            for (u32 k = 0; k < l; k++) snprintf(hx + 2 * k, 3, "%02x", best->ie[i + k]);
            DIAG("target \"%s\": %u MHz, rssi %d, RSN IE %s", T.ssid, A.channel, best->rssi, hx);
        } else if (best->ie[i] == 0xDD && best->ie[i + 1] >= 10 &&
                   !memcmp(best->ie + i + 2, "\x00\x50\xF2\x01", 4)) {
            rsn_info_t wi;
            if (wpa1_ie_parse(best->ie + i, best->ie_len - i, &wi) == RSN_MODE_WPA1) {
                A.wpa1_ap = 1;
                DIAG("target \"%s\": WPA v1 vendor IE, TKIP pairwise", T.ssid);
            }
        }
    }
    /* Only an actual RSN IE overrides WPA v1. RSN_MODE_WPA2 is also the
       default when there is no RSN IE at all, so testing the mode alone
       cleared WPA v1 and tried WPA2-PSK/CCMP against a TKIP-only AP. */
    if (saw_rsn) A.wpa1_ap = 0;
    if (A.wpa1_ap) A.group_crypt = CRYPT_TKIP;
}

int ar6k_command(const volatile wlan_cmd_t *c)
{
    if (!fw_up()) return -1;
    u32 zero = 0;
    switch (c->cmd) {
    case WLAN_CMD_SCAN:
        logp("[wifi] iOS: scan\n");
        return user_scan_start();
    case WLAN_CMD_JOIN: {
        if (!c->ssid_len || c->ssid_len > 32) return -2;
        int was = connecting();
        memset(&T, 0, sizeof(T));
        for (u32 i = 0; i < c->ssid_len; i++) T.ssid[i] = c->ssid[i];
        T.ssid_len = c->ssid_len;
        T.security = c->security == WLAN_SEC_WPA2 ? WIFI_SEC_WPA2 : WIFI_SEC_OPEN;
        for (u32 i = 0; i < 32; i++) T.pmk[i] = c->pmk[i];
        T.have = 1;
        A.from_ios = 1;
        A.auth_fails = 0;
        A.nonet = 0;
        A.rescan = 0;
        target_lookup();
        logp("[wifi] iOS: join \"%s\" (%s), %s\n", T.ssid, T.security == WIFI_SEC_WPA2 ? "WPA2" : "open",
             A.found ? "in the scan list" : "not in the scan list: scanning");
        if (was) {                              /* leave the current network first */
            A.switching = 1;
            return wmi_send(WMI_DISCONNECT, &zero, 4);
        }
        if (A.found) {
            do_connect();
            return A.err;
        }
        A.state = NET_ST_SCAN;
        return scan_start();
    }
    case WLAN_CMD_LEAVE:
        logp("[wifi] iOS: leave \"%s\"\n", T.ssid);
        T.have = 0;
        A.reconnect_at = 0;
        if (connecting()) return wmi_send(WMI_DISCONNECT, &zero, 4);
        A.state = NET_ST_IDLE;
        return 0;
    }
    return -3;
}

/* WMI_SET_POWER_MODE from the idle policy. ath6kl: 1 = REC_POWER,
   2 = MAX_PERFORMANCE (the value do_connect() sends). Only legal once the
   WMI handshake has completed; a failed send is logged by wmi_queue_flush. */
int ar6k_power_mode(u8 mode)
{
    if (A.state < NET_ST_WMI) return -1;
    return wmi_send(WMI_SET_POWER_MODE, &mode, 1);
}

/* status + scan table into the wlan mailbox (wifi.c, every poll) */
void ar6k_publish(volatile wlanmbox_t *w, u32 *published_scan)
{
    volatile wlan_status_t *st = &w->st;
    u32 ws;
    switch (A.state) {
    case NET_ST_POWER: case NET_ST_BMI: case NET_ST_FWLOAD: case NET_ST_BOOT:
    case NET_ST_HTC: case NET_ST_WMI: ws = WLAN_ST_STARTING; break;
    case NET_ST_IDLE: ws = WLAN_ST_IDLE; break;
    case NET_ST_SCAN: ws = T.have ? WLAN_ST_JOINING : WLAN_ST_IDLE; break;
    case NET_ST_AUTH: ws = WLAN_ST_JOINING; break;
    case NET_ST_UP: ws = WLAN_ST_UP; break;
    default: ws = WLAN_ST_OFF; break;
    }
    st->state = ws;
    for (int i = 0; i < 6; i++) st->mac[i] = A.mac[i];
    st->link_seq = A.link_seq;
    st->disc_reason = A.disc_reason;
    st->disc_code = A.disc_code;
    if (ws == WLAN_ST_UP || ws == WLAN_ST_JOINING) {
        int16_t rssi = -60, noise = -95;
        for (u32 i = 0; i < nscan; i++)
            if (!memcmp(scan_tab[i].bssid, A.bssid, 6)) { rssi = scan_tab[i].rssi; noise = scan_tab[i].noise; }
        for (int i = 0; i < 6; i++) st->bssid[i] = A.bssid[i];
        for (int i = 0; i < 32; i++) st->ssid[i] = i < T.ssid_len ? T.ssid[i] : 0;
        st->ssid_len = T.ssid_len;
        st->security = T.security == WIFI_SEC_WPA2 ? WLAN_SEC_WPA2 : WLAN_SEC_OPEN;
        st->channel_mhz = A.channel;
        st->rssi = rssi;
        st->noise = noise;
    } else {
        st->ssid_len = 0;
    }
    if (*published_scan != scan_seq) {
        *published_scan = scan_seq;
        volatile u8 *d = (volatile u8 *)w->bss;
        const u8 *src = (const u8 *)scan_tab;
        for (u32 i = 0; i < nscan * sizeof(wlan_bss_t); i++) d[i] = src[i];
        st->nscan = nscan;
        st->scan_seq = scan_seq;
    }
}

/* ---- backend interface ------------------------------------------------------------ */
/* sd:/ios3ds/nwm.code (GodMode9 "extrcode" of the console's NWM .app) ->
   the container in fw_buf. 1 = no such file, 0 = done, < 0 = unusable. */
static int fw_from_nwm(u32 main_type)
{
    u8 *buf = (u8 *)wifi_scratch_pa;
    u32 len = 0;
    if (!wifi_scratch_pa) {
        log_warn("[wifi] no scratch memory for %s (boot.cfg layout)\n", NWM_CODE_PATH);
        return 1;
    }
    u32 t = now_ms();
    int r = sd_load(NWM_CODE_PATH, buf, wifi_scratch_len, &len);
    if (r == -4 || r == -5) return 1;           /* FR_NO_FILE / FR_NO_PATH */
    if (r < 0) {
        log_err("[wifi] %s: cannot read (%d%s): Wi-Fi off\n", NWM_CODE_PATH, r,
                r == -1000 ? ", larger than 4 MiB: not NWM's .code" : "");
        return r;
    }
    nwm_info_t in;
    r = nwm_extract(buf, len, wifi_scratch_len, main_type, fw_buf, sizeof(fw_buf), &fw_size, &in);
    memset(buf, 0, in.code_size > len ? in.code_size : len);   /* no Nintendo code left in FCRAM */
    if (r < 0) {
        log_err("[wifi] %s (%u bytes%s): %s (%d): Wi-Fi off (docs/wifi.md section 3)\n", NWM_CODE_PATH, len,
                in.blz ? ", BLZ" : "", nwm_strerror(r), r);
        if (in.layout)
            DIAG("fw: pool at .code+%x, main type1 %x type4 %x type5 %x bytes", in.pool_off,
                 in.main_size[0], in.main_size[1], in.main_size[2]);
        return r;
    }
    u32 m4 = in.main_size[1];
    log_ok("[wifi] firmware from %s: %u bytes%s, main type%u, %u ms\n", NWM_CODE_PATH, len,
           in.blz ? " (BLZ-compressed)" : "", in.main_type, now_ms() - t);
    DIAG("fw: %s pool at .code+%x, text base %x, .code %u bytes; main type1 %x type4 %x type5 %x bytes "
         "(type4 %s GBATEK's A053h/A482h/A5EBh); database end %s stub code start",
         in.layout == NWM_NEWER ? "newer" : "older", in.pool_off, in.text_base, in.code_size,
         in.main_size[0], m4, in.main_size[2],
         m4 == 0xA053 || m4 == 0xA482 || m4 == 0xA5EB ? "is one of" : "is NOT one of",
         in.db_end_is_stub ? "=" : "!=");
    return 0;
}

static int ar6k_init(const wifi_cfg_t *cfg, u8 mac[6])
{
    memset(&A, 0, sizeof(A));
    A.cfg = cfg;
    A.verbose = (u8)cfg->verbose;
    memset(&T, 0, sizeof(T));
    if (cfg->ssid_len) {                    /* wifi.cfg preset: join it at boot */
        memcpy(T.ssid, cfg->ssid, cfg->ssid_len);
        T.ssid_len = cfg->ssid_len;
        T.security = cfg->security;
        memcpy(T.pmk, cfg->pmk, 32);
        T.have = 1;
    }
    memset(mac, 0, 6);          /* known after WMI ready; wifi.c copies it then */
    A.diag = cfg->diag;
    A.t0 = now_ms();
    const char *src = cfg->fw;
    int r = sd_load(cfg->fw, fw_buf, sizeof(fw_buf), &fw_size);
    if (r < 0) {
        int n = fw_from_nwm(cfg->fwmain);
        if (n < 0) return -1;                   /* nwm.code there but unusable: logged */
        if (n > 0) {
            if (cfg->ssid_len)
                log_warn("[wifi] %s: not found (%d), no %s either. Run tools/ios3ds-wifi/ios3ds-wifi-fw.gm9 "
                         "in GodMode9 (docs/wifi.md section 3)\n", cfg->fw, r, NWM_CODE_PATH);
            else
                logp("[wifi] %s: not found (%d), no %s either: Wi-Fi off (docs/wifi.md section 3)\n",
                     cfg->fw, r, NWM_CODE_PATH);
            /* what the .3dsx launcher said when it tried to copy the firmware */
            static char st[160];
            u32 sl = 0;
            if (sd_load("0:/ios3ds/wififw-status.txt", st, sizeof(st) - 1, &sl) >= 0 && sl) {
                st[sl] = 0;
                logp("[wifi] launcher's last firmware copy: %s", st);
            } else {
                logp("[wifi] the launcher never tried to copy the firmware: start ios3ds once from the "
                     "Homebrew Launcher (not Luma's chainloader), or run the GodMode9 script (README: Install)\n");
            }
            return -1;
        }
        src = NWM_CODE_PATH;
    }
    if ((r = fw_check()) < 0) {
        log_err("[wifi] %s: bad container (%d)\n", src, r);
        return -1;
    }
    if (A.diag) {
        static const char *const what[] = { "?", "stub data", "stub code", "database", "main" };
        const fw_hdr_t *h = (const fw_hdr_t *)fw_buf;
        for (u32 i = 0; i < h->nparts; i++) {
            const fw_part_t *q = &h->part[i];
            DIAG("fw: %s part %u %s: %u bytes (0x%x) -> %08x, crc32 %08x", src, q->kind,
                 what[q->kind < 5 ? q->kind : 0], q->len, q->len, q->dst, nwm_crc32(fw_buf + q->off, q->len));
        }
    }
    A.state = NET_ST_POWER;
    return 0;
}

/* Unverified on hardware: whether the ARM9 can use TMIO controller 2's 32-bit
   FIFO at 0x1012210C (dsiwifi's 3DS ARM11 build uses a FIFO mirror at
   +0x200000 instead). Write a pattern into the function-1 scratch registers
   with CMD52, read it back with one CMD53; if that fails, move every mailbox
   transfer to CMD52 (slow, but needs no FIFO). */
static void fifo_selftest(void)
{
    static const u8 pat[4] = { 0x5A, 0xC3, 0x0F, 0x96 };
    u8 got[4] __attribute__((aligned(4))) = { 0 };
    int r = 0;
    for (int i = 0; i < 4 && r >= 0; i++) r = sdio_w8(1, F1_SCRATCH0 + i, pat[i]);
    if (r >= 0) r = sdio_read_bytes(1, F1_SCRATCH0, got, 4);
    A.byte_mode = r < 0 || memcmp(got, pat, 4) != 0;
    if (A.byte_mode)
        log_warn("[wifi] TMIO2 32-bit FIFO check failed (%d, read %02x%02x%02x%02x, status %08x): "
                 "mailbox falls back to CMD52 byte transfers\n", r, got[0], got[1], got[2], got[3],
                 sdio_last_status());
    else
        logp("[wifi] TMIO2 32-bit FIFO works (CMD53 read back the scratch pattern)\n");
}

static u32 ar6k_step(void)
{
    u32 v = 0;
    int r;
    /* bring-up watchdog: every step has its own bound; this catches a
       sequence of slow-but-not-failing steps */
    if (A.state >= NET_ST_BMI && A.state <= NET_ST_WMI && now_ms() - A.t0 > 30000)
        return fail(-112, "bring-up watchdog (30 s since power-up)");
    /* an iOS scan the firmware never completes: publish what came in */
    if (user_scan && now_ms() - A.t_scan > 10000) {
        user_scan = 0;
        scan_seq++;
        log_warn("[wifi] no SCAN_COMPLETE 10 s after the scan request: %u networks (%u BSSINFO)\n",
                 nscan, A.nbssinfo);
    }
    wmi_queue_flush();              /* one non-blocking retry per monitor pass */
    eapol_pending_step();
    switch (A.state) {
    case NET_ST_POWER:
        /* libn3ds WIFI_init: CFG11 "wifi power" (really the MP regs), select
           the SDIO chip, release nRESET, then ~2 ms ("very close to 2 ms")
           before talking SDIO. We pulse reset first because Horizon may
           have left the chip running (warm reboot through Luma). */
        {
            A.t0 = now_ms();
            u8 c0 = CFG11_WIFICNT;
            u16 g2 = GPIO_WIFI_MODE, g3 = GPIO_WIFI_EN;
            CFG11_WIFICNT = 1;
            GPIO_WIFI_MODE = (u16)(GPIO_WIFI_MODE & ~1u);
            GPIO_WIFI_EN = (u16)(GPIO_WIFI_EN & ~1u);
            waitcycles(33500);                          /* ~1 ms in reset */
            GPIO_WIFI_EN = (u16)(GPIO_WIFI_EN | 1u);
            waitcycles(67000);                          /* ~2 ms */
            DIAG("power: CFG11_WIFICNT %02x -> %02x (want bit 0 = 1), GPIO 2_2 %04x -> %04x (want bit 0 = 0: "
                 "SDIO chip), GPIO 3_12 %04x -> %04x (want bit 0 = 1: nRESET released)",
                 c0, CFG11_WIFICNT, g2, GPIO_WIFI_MODE, g3, GPIO_WIFI_EN);
        }
        {
            u32 t = now_ms();
            r = sdio_init();
            const sdio_diag_t *sd = sdio_diag();
            DIAG("sdio: CFG9_SDMMCCTL %04x -> %04x (want bit 2 = 0: port powered), SD_OPTION %04x (%s)",
                 sd->cfg9_before, sd->cfg9_after, sd->opt,
                 (sd->opt & 0xFF) == 0xEE ? "controller 2 answers" : "controller 2 NOT answering");
            if (sd->cmd5_tries)
                DIAG("sdio: CMD5 x%u: first R4 %08x, last %08x (ready %u, %u I/O functions, memory %u, OCR %06x)",
                     sd->cmd5_tries, sd->ocr_first, sd->ocr, sd->ocr >> 31, (sd->ocr >> 28) & 7,
                     (sd->ocr >> 27) & 1, sd->ocr & 0xFFFFFF);
            if (sd->rca)
                DIAG("sdio: RCA %04x, CCCR rev %02x, SD rev %02x, card caps %02x, bus interface %02x "
                     "(want low bits 2 = 4-bit)", sd->rca, sd->cccr_rev, sd->sd_rev, sd->caps, sd->bic);
            if (r < 0) {
                DIAG("sdio: stopped at step \"%s\" after %u ms", sd->stage, now_ms() - t);
                return fail(r, "SDIO enumeration");
            }
            DIAG("sdio: function 1 ready after %u polls; enumeration took %u ms", sd->ready_tries, now_ms() - t);
            if (A.diag) {
                u16 manf = 0, card = 0;
                int m = sdio_manfid(&manf, &card);
                if (m == 0)
                    DIAG("sdio: CIS manufacturer %04x card %04x (%s)", manf, card,
                         manf == 0x0271 ? "Atheros" : "not Atheros 0271");
                else
                    DIAG("sdio: no CISTPL_MANFID in the CIS (%d)", m);
            }
        }
        if ((r = soc_read(SOC_CHIP_ID, &v)) < 0) return fail(r, "chip id read");
        log_ok("[wifi] SDIO up, chip id %08x%s\n", v, v == CHIP_ID_AR6014 ? " (AR6014)" : " (unexpected)");
        fifo_selftest();
        if ((r = soc_write(SOC_RESET_CONTROL, 0x100)) < 0) return fail(r, "warm reset");
        A.t_state = now_ms();                           /* ~100 ms until the BMI ROM runs: not blocking */
        A.state = NET_ST_BMI;
        break;
    case NET_ST_BMI:
        if (now_ms() - A.t_state < 100) break;
        if ((r = bmi_cmd(BMI_GET_TARGET_ID, 0, 0, 0)) < 0 || (r = bmi_r32(&v)) < 0)
            return fail(r, "BMI_GET_TARGET_ID");
        if (v == 0xFFFFFFFFu) {                         /* extended reply: size, version, type */
            u32 sz = 0, ver = 0, x;
            if ((r = bmi_r32(&sz)) < 0 || (r = bmi_r32(&ver)) < 0) return fail(r, "BMI target info");
            for (u32 i = 8; i < sz && i < 32; i += 4) bmi_r32(&x);
            v = ver;
        }
        log_ok("[wifi] BMI ROM version %08x (AR6014 ROM is 2300006F)\n", v);
        /* dsiwifi: hi_app_host_interest = 2, then scratch0 |= 8,
           WLAN_SYSTEM_SLEEP |= 1, CLOCK_CONTROL = 5, [0x4020] = 0 */
        {
            u32 s0 = 0, sl = 0, ver = v;
            if ((r = bmi_write_word(HOST_INTEREST, 2)) < 0) return fail(r, "host interest");
            if ((r = bmi_read_soc(SOC_SCRATCH0, &s0)) < 0 || (r = bmi_write_soc(SOC_SCRATCH0, s0 | 8)) < 0 ||
                (r = bmi_read_soc(SOC_WLAN_SYS_SLEEP, &sl)) < 0 || (r = bmi_write_soc(SOC_WLAN_SYS_SLEEP, sl | 1)) < 0 ||
                (r = bmi_write_soc(SOC_CLOCK_CONTROL, 5)) < 0 || (r = bmi_write_soc(SOC_REG_4020, 0)) < 0)
                return fail(r, "BMI clock setup");
            DIAG("bmi: target version %08x (%s), scratch0 was %08x, WLAN_SYSTEM_SLEEP was %08x; clock setup done",
                 ver, ver == 0x2300006F ? "AR6014 ROM" : "UNEXPECTED", s0, sl);
        }
        A.state = NET_ST_FWLOAD;
        break;
    case NET_ST_FWLOAD:
        r = fw_step();
        if (r < 0) return fail(r, "firmware upload");
        if (r == 1) {
            if ((r = bmi_write_word(HOST_INTEREST + HI_MBOX_IO_BLOCK_SZ, SDIO_BLOCK)) < 0 ||
                (r = bmi_write_word(HOST_INTEREST + HI_MBOX_ISR_YIELD, 0x63)) < 0 ||
                (r = bmi_cmd(BMI_DONE, 0, 0, 0)) < 0)
                return fail(r, "BMI_DONE");
            logp("[wifi] firmware started, waiting for it to come up\n");
            DIAG("fw: upload done, BMI_DONE sent (hi_mbox_io_block_sz %u, hi_mbox_isr_yield 0x63)", SDIO_BLOCK);
            A.t_state = now_ms();
            A.boot_polls = 0;
            A.state = NET_ST_BOOT;
        }
        break;
    case NET_ST_BOOT:
        if ((r = soc_read(HOST_INTEREST + HI_BOARD_DATA_INIT, &v)) < 0) return fail(r, "boot poll");
        A.boot_polls++;
        if (v == 1) {
            log_ok("[wifi] firmware running (board data initialised)\n");
            DIAG("boot: hi_board_data_initialized = 1 after %u ms (%u polls)", now_ms() - A.t_state, A.boot_polls);
            A.state = NET_ST_HTC;
            A.t_state = now_ms();
        } else if (now_ms() - A.t_state > 3000) {
            return fail(-110, "firmware start (hi_board_data_initialized never set)");
        }
        break;
    case NET_ST_SCAN:
        /* no SCAN_COMPLETE within 10 s: connect with what we have and let
           the firmware search; after a disconnect: reconnect when due */
        if (!A.scan_done && now_ms() - A.t_state > 10000) do_connect();
        if (A.reconnect_at && (int)(now_ms() - A.reconnect_at) >= 0) do_connect();
        if (!A.rxlen) pump();
        break;
    case NET_ST_HTC: case NET_ST_WMI:
        /* the firmware announces itself (HTC_READY) and WMI (WMI_READY)
           within milliseconds; give up after 5 s instead of waiting forever */
        if (now_ms() - A.t_state > 5000)
            return fail(-111, A.state == NET_ST_HTC ? "firmware start (no HTC_READY)" : "WMI start (no WMI_READY)");
        if (!A.rxlen) pump();
        break;
    case NET_ST_AUTH: case NET_ST_UP: case NET_ST_IDLE:
        if (!A.rxlen) pump();       /* keep a data frame until rx() took it */
        break;
    default:
        break;
    }
    return A.state;
}

/* one driver step per monitor iteration; [wifi-diag] reports the longest
   (the monitor serves md0 and the GPU mirror in between) */
static u32 ar6k_poll(void)
{
    u32 t = now_ms(), st = A.state;
    u32 r = ar6k_step();
    u32 d = now_ms() - t;
    if (d > A.poll_max) {
        A.poll_max = d;
        if (d >= 20) DIAG("monitor: one driver step took %u ms (state %s)", d, st_name(st));
    }
    return r;
}

static int ar6k_tx(const u8 *eth, u32 len)
{
    static u8 out[2 + 14 + 8 + 1500];
    if (A.state != NET_ST_UP) return -1;
    u32 n = wifi_frame_to_wmi(eth, len, out, sizeof(out));
    if (!n) return -2;
    int r = mbox_send(A.ep_data, 0, 0, out, n, 0); /* no credits: leave the frame in the mailbox */
    return r == SDIO_ERR_BUSY ? -1 : r;
}

static u32 ar6k_rx(u8 *eth, u32 cap)
{
    if (!A.rxlen) pump();
    u32 n = A.rxlen;
    if (!n || n > cap) { A.rxlen = 0; return 0; }
    memcpy(eth, A.rxframe, n);
    A.rxlen = 0;
    return n;
}

static int ar6k_last_error(void) { return A.err; }

/* wifi.c reads the MAC through this once the firmware reported it */
const u8 *ar6k_mac(void) { return A.mac; }

const wifi_backend_t wifi_backend_ar6014 = {
    .name = "ar6014", .id = NET_BACKEND_AR6014,
    .init = ar6k_init, .poll = ar6k_poll, .tx = ar6k_tx, .rx = ar6k_rx,
    .last_error = ar6k_last_error,
};
