/*
 * Host unit tests for the pure parts of the Wi-Fi stack:
 *   - netmbox ring: ARM9 side (boot/arm9/wifi/wifi_ring.c) against the ARM11
 *     side (tools/ios3ds-wifi/shim/net_core.h) and the netio_t packing
 *   - Ethernet II <-> WMI data frame conversion (wifi_frame.c)
 *   - wifi.cfg parser (wifi_cfg.c)
 *   - WPA2: SHA-1, HMAC, PBKDF2, AES, RFC 3394 vectors, and a full 4-way +
 *     group handshake against a small authenticator written here (wpa2.c)
 * Build/run: make -C tools/ios3ds-wifi test
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"
#include "wpa2.h"

/* Compile the AR6K driver into the test with its hardware boundary mocked
   below, so the HTC credit path can be exercised without a 3DS. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define waitcycles host_waitcycles
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"   /* wifi_scratch_pa is an ARM9 address */
#include "../../../boot/arm9/wifi/ar6k.c"
#pragma GCC diagnostic pop
#undef waitcycles
#undef ARRAY_SIZE

#define NET_DINVAL(p, len) ((void)(p), (void)(len))
#define NET_DCLEAN(p, len) ((void)(p), (void)(len))
#include "net_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- AR6K hardware boundary mocks ---------------------------------------
   Only the mailbox and the SDIO writes are real enough for the credit path;
   nothing here runs firmware or touches hardware. */
static u32 host_wait_cycles;
void host_waitcycles(u32 n) { host_wait_cycles += n; }

static u8 mock_rx[0x680];
static u32 mock_rx_frames, mock_rx_len;
static int mock_write_err, mock_write_calls;
static int mock_w8_err, mock_w8_calls;
static u8 mock_tx[0x680];
static u32 mock_tx_len;

static void mock_reset(void)
{
    memset(mock_rx, 0, sizeof(mock_rx));
    memset(mock_tx, 0, sizeof(mock_tx));
    mock_rx_frames = mock_rx_len = mock_tx_len = 0;
    mock_write_err = mock_write_calls = mock_w8_err = mock_w8_calls = 0;
}

static void mock_mailbox_frame(u32 ep, const u8 *body, u32 n, const u8 *trailer, u32 tn)
{
    u32 len = n + tn;
    memset(mock_rx, 0, sizeof(mock_rx));
    mock_rx_len = len;
    mock_rx[0] = (u8)ep;
    mock_rx[1] = tn ? HTC_FLG_RX_TRAILER : 0;
    mock_rx[2] = (u8)len;
    mock_rx[3] = (u8)(len >> 8);
    mock_rx[4] = (u8)tn;
    if (n) memcpy(mock_rx + 6, body, n);
    if (tn) memcpy(mock_rx + 6 + n, trailer, tn);
    mock_rx_frames++;
}

int sdio_init(void) { return 0; }
int sdio_r8(u32 func, u32 addr)
{
    (void)func;
    if (addr == F1_HOST_INT_STATUS) return mock_rx_frames ? 1 : 0;
    if (addr >= F1_RX_LOOKAHEAD0 && addr < F1_RX_LOOKAHEAD0 + 4) {
        u32 la = mock_rx_len << 16;
        return (u8)(la >> (8 * (addr - F1_RX_LOOKAHEAD0)));
    }
    return 0;
}
int sdio_w8(u32 func, u32 addr, u8 val)
{
    (void)func; (void)addr; (void)val;
    mock_w8_calls++;
    return mock_w8_err;
}
int sdio_read_blocks(u32 func, u32 addr, u8 *buf, u32 len)
{
    (void)func; (void)addr;
    if (!mock_rx_frames || len > sizeof(mock_rx)) return SDIO_ERR_CARD;
    memset(buf, 0, len);
    memcpy(buf, mock_rx, len);
    mock_rx_frames--;
    return 0;
}
int sdio_write_blocks(u32 func, u32 addr, const u8 *buf, u32 len)
{
    (void)func; (void)addr;
    mock_write_calls++;
    mock_tx_len = len;
    memcpy(mock_tx, buf, len);
    return mock_write_err;
}
int sdio_read_bytes(u32 func, u32 addr, u8 *buf, u32 len)
{
    (void)func; (void)addr; (void)buf; (void)len;
    return 0;
}
u32 sdio_last_status(void) { return 0; }
const sdio_diag_t *sdio_diag(void) { static sdio_diag_t d; return &d; }
int sdio_manfid(u16 *manf, u16 *card) { (void)manf; (void)card; return 0; }
int sd_load(const char *path, void *dst, u32 maxsize, u32 *outsize)
{
    (void)path; (void)dst; (void)maxsize; (void)outsize;
    return -4;
}
int nwm_extract(u8 *code, u32 len, u32 cap, u32 main_type, u8 *out, u32 outcap,
                u32 *outlen, nwm_info_t *info)
{
    (void)code; (void)len; (void)cap; (void)main_type;
    (void)out; (void)outcap; (void)outlen; (void)info;
    return 0;
}
const char *nwm_strerror(int e) { (void)e; return ""; }
u32 nwm_crc32(const u8 *p, u32 n) { (void)p; (void)n; return 0; }
void log_color(u32 color, const char *fmt, ...) { (void)color; (void)fmt; }
u32 wifi_scratch_pa, wifi_scratch_len;

static void unhex(const char *h, u8 *out)
{
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (u8)v;
    }
}
static int eqhex(const u8 *b, const char *h)
{
    u8 t[256];
    unhex(h, t);
    return memcmp(b, t, strlen(h) / 2) == 0;
}

/* ---------------------------------------------------------------- ring */
static void mkframe(u8 *f, u32 len, u32 seed)
{
    for (u32 i = 0; i < len; i++) f[i] = (u8)(seed * 31 + i);
}

static void test_ring(void)
{
    static netmbox_t mb;
    static netio_t io;
    volatile netmbox_t *m = &mb;
    u8 f[NET_FRAME_MAX], g[NET_FRAME_MAX];
    const u8 mac[6] = { 2, 0, 0, 0, 0, 1 };

    memset(&mb, 0xEE, sizeof(mb));                  /* stale FCRAM */
    net_core_fill(m, &io);
    CHECK(io.magic == NETIO_MAGIC && io.link == NETIO_LINK_NONE);

    net_mbox_init(m, NET_BACKEND_LOOP, mac);
    CHECK(mb.magic == NET_MAGIC && mb.rx_head == 0 && mb.tx_tail == 0);
    /* the ARM11 line holds garbage (ARM9 restarted under a running iOS): the
       ARM9 refuses to push until the ARM11's next read resynchronises */
    mb.rx_tail = 0x12345678;
    CHECK(net_mbox_push_rx(m, f, 60) == -1);
    net_core_fill(m, &io);
    CHECK(mb.rx_tail == mb.rx_head && io.nframes == 0);
    mb.a9_rxdrop = 0;
    for (u32 i = 0; i < 3; i++) { mkframe(f, 60 + i, i); CHECK(net_mbox_push_rx(m, f, 60 + i) == 0); }
    net_core_fill(m, &io);
    CHECK(io.nframes == 3 && io.rx_next == 3 && io.rx_pending == 0 && io.link == 0);
    CHECK(memcmp(io.mac, mac, 6) == 0 && io.backend == NET_BACKEND_LOOP);
    CHECK(io.ssid[0] == 0 && io.nettest[0] == 0);
    memcpy((void *)mb.ssid, "net", 3);
    memset((void *)mb.nettest, 'x', sizeof(mb.nettest));    /* unterminated: clipped */
    net_core_fill(m, &io);
    CHECK(memcmp(io.ssid, "net", 4) == 0 && strlen(io.nettest) == 63);
    memset((void *)mb.nettest, 0, sizeof(mb.nettest));
    net_core_fill(m, &io);
    u32 off = 0, len = 0;
    for (u32 i = 0; i < 3; i++) {
        const u8 *p = netio_get(io.data, NETIO_DATA, &off, &len);
        mkframe(f, 60 + i, i);
        CHECK(p && len == 60 + i && memcmp(p, f, len) == 0);
    }
    CHECK(netio_get(io.data, NETIO_DATA, &off, &len) == NULL);    /* zero padding ends the list */
    /* a plain read does not consume */
    net_core_fill(m, &io);
    CHECK(io.nframes == 3);

    /* ack + 2 TX frames in one write */
    netio_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_next = io.rx_next;
    off = 0;
    mkframe(f, 1514, 7); CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 1514) == 0);
    mkframe(f, 99, 8);   CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 99) == 0);
    CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 13) == -1);    /* runt */
    cmd.nframes = 2;
    CHECK(net_core_apply(m, &cmd) == 2);
    CHECK(mb.rx_tail == 3 && mb.tx_head == 2);
    net_core_fill(m, &io);
    CHECK(io.nframes == 0 && io.tx_free == NET_SLOTS - 2);
    /* replaying the same ack is harmless; an ack beyond rx_head is ignored */
    cmd.nframes = 0; CHECK(net_core_apply(m, &cmd) == 0 && mb.rx_tail == 3);
    cmd.rx_next = 99; net_core_apply(m, &cmd); CHECK(mb.rx_tail == 3);

    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 1514);
    mkframe(f, 1514, 7); CHECK(memcmp(f, g, 1514) == 0);
    net_mbox_tx_done(m);
    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 99);
    net_mbox_tx_done(m);
    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 0 && mb.a9_txframes == 2);

    /* RX ring full: the 17th frame is dropped and counted */
    for (u32 i = 0; i < NET_SLOTS; i++) CHECK(net_mbox_push_rx(m, f, 100) == 0);
    CHECK(net_mbox_push_rx(m, f, 100) == -1 && mb.a9_rxdrop == 1);
    CHECK(net_mbox_push_rx(m, f, 2000) == -2);
    /* a batch is limited by the 4 KiB io buffer, the rest stays pending */
    for (u32 i = 0; i < NET_SLOTS; i++) mb.rx[i].len = 1000;
    net_core_fill(m, &io);
    CHECK(io.nframes == 3 && io.rx_pending == NET_SLOTS - 3);   /* 3 x 1002 <= 3968 */

    /* wrap the 32-bit indices */
    net_mbox_init(m, NET_BACKEND_LOOP, mac);
    mb.rx_head = mb.rx_tail = 0xFFFFFFFEu;
    for (u32 i = 0; i < 5; i++) CHECK(net_mbox_push_rx(m, f, 64) == 0);
    net_core_fill(m, &io);
    CHECK(io.nframes == 5 && io.rx_next == 3);
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_next = io.rx_next;
    net_core_apply(m, &cmd);
    CHECK(mb.rx_tail == 3);
    /* TX ring full: extra frames are counted as dropped */
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_pending = NETIO_F_NOACK;
    off = 0;
    for (u32 i = 0; i < 18; i++) CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 60) == 0);
    cmd.nframes = 18;
    CHECK(net_core_apply(m, &cmd) == NET_SLOTS && mb.a11_txdrop == 2 && mb.rx_tail == 3);
}

/* ---------------------------------------------------------------- frames */
static void test_frames(void)
{
    u8 eth[1514], wmi[1600], back[1514];
    u16 et = 0;
    mkframe(eth, 1514, 3);
    eth[12] = 0x08; eth[13] = 0x00;
    u32 n = wifi_frame_to_wmi(eth, 1514, wmi, sizeof(wmi));
    CHECK(n == 2 + 14 + 8 + 1500);
    CHECK(wmi[0] == 0 && wmi[1] == 0x1C && wmi[14] == (1508 >> 8) && wmi[15] == (1508 & 0xFF));
    CHECK(eqhex(wmi + 16, "aaaa030000000800"));
    wmi[0] = 40; wmi[1] = 0;                      /* RX: rssi, 0 */
    CHECK(wifi_frame_from_wmi(wmi, n + 20, back, sizeof(back), &et) == 1514);  /* +padding */
    CHECK(et == 0x0800 && memcmp(back, eth, 1514) == 0);
    CHECK(wifi_frame_to_wmi(eth, 1514, wmi, 1523) == 0);          /* no room */
    CHECK(wifi_frame_to_wmi(eth, 1515, wmi, sizeof(wmi)) == 0);   /* too long */

    /* bridge-tunnel SNAP and an already-Ethernet-II frame */
    u8 rx[64] = { 0x30, 0 };
    memcpy(rx + 2, "\xff\xff\xff\xff\xff\xff\x02\x11\x22\x33\x44\x55", 12);
    rx[14] = 0; rx[15] = 8 + 4;
    memcpy(rx + 16, "\xaa\xaa\x03\x00\x00\xf8\x80\xf3" "ABCD", 12);
    n = wifi_frame_from_wmi(rx, 28, back, sizeof(back), &et);
    CHECK(n == 18 && et == 0x80F3 && back[12] == 0x80 && memcmp(back + 14, "ABCD", 4) == 0);
    rx[14] = 0x88; rx[15] = 0x8E;
    n = wifi_frame_from_wmi(rx, 30, back, sizeof(back), &et);
    CHECK(n == 28 && et == 0x888E);
    /* 802.3 length that lies about the payload */
    rx[14] = 0x05; rx[15] = 0x00;
    CHECK(wifi_frame_from_wmi(rx, 30, back, sizeof(back), &et) == 0);
    CHECK(wifi_frame_from_wmi(rx, 10, back, sizeof(back), &et) == 0);
}

/* HTC credit reports from the RX trailer (ath6kl htc.h record 1) */
static void test_htc_credits(void)
{
    u8 c[8] = { 1, 0, 2, 0, 0, 0, 0, 0 };
    const u8 rep[] = { WMI_HTC_REC_CREDITS, 4, 1, 2, 2, 3 };
    CHECK(wifi_htc_credits(rep, sizeof(rep), c) == 5);
    CHECK(c[1] == 2 && c[2] == 5 && c[3] == 0);
    /* unknown records are skipped; a second credit record is cumulative */
    const u8 two[] = { 7, 2, 0xAA, 0xBB, WMI_HTC_REC_CREDITS, 2, 3, 1, 0, 0 };
    CHECK(wifi_htc_credits(two, sizeof(two), c) == 1);
    CHECK(c[3] == 1 && c[1] == 2);
    /* a torn record credits nothing, including records before the tear */
    u8 before[8];
    memcpy(before, c, sizeof(c));
    const u8 torn[] = { WMI_HTC_REC_CREDITS, 4, 1, 1, 2 };
    CHECK(wifi_htc_credits(torn, sizeof(torn), c) == 0);
    CHECK(memcmp(before, c, sizeof(c)) == 0);
    /* saturation and out-of-range endpoints */
    c[0] = 254;
    const u8 sat[] = { WMI_HTC_REC_CREDITS, 4, 0, 3, 9, 2 };
    CHECK(wifi_htc_credits(sat, sizeof(sat), c) == 3);   /* ep 9 is ignored */
    CHECK(c[0] == 255 && c[1] == 2);                    /* ep 0 saturates */
    CHECK(wifi_htc_credits(rep, 1, c) == 0);                    /* truncated header */
}

/* ---------------------------------------------------------------- wlan */
static void test_wlan(void)
{
    static wlanmbox_t wb;
    static wlanio_t io, cmd;
    volatile wlanmbox_t *w = &wb;
    memset(&wb, 0xEE, sizeof(wb));                  /* stale FCRAM: not served */
    wlan_core_fill(w, &io);
    CHECK(io.magic == WLANIO_MAGIC && io.version == 0);
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = WLANIO_CMD; cmd.version = WLAN_VERSION; cmd.cmd.cmd = WLAN_CMD_SCAN;
    CHECK(wlan_core_apply(w, &cmd) == 0);           /* nobody serves: not posted */

    memset(&wb, 0, sizeof(wb));
    wb.st.magic = WLAN_MAGIC; wb.st.version = WLAN_VERSION; wb.st.state = WLAN_ST_IDLE;
    wb.st.nscan = 2; wb.bss[0].ssid_len = 3; memcpy(wb.bss[0].ssid, "abc", 3);
    wb.bss[1].rssi = -70; wb.bss[2].rssi = -1;      /* beyond nscan: not copied */
    wlan_core_fill(w, &io);
    CHECK(io.version == WLAN_VERSION && io.st.state == WLAN_ST_IDLE && io.st.nscan == 2);
    CHECK(memcmp(io.bss[0].ssid, "abc", 3) == 0 && io.bss[1].rssi == -70 && io.bss[2].rssi == 0);
    CHECK(io.cmd_seq == 0);
    cmd.cmd.cmd = WLAN_CMD_JOIN; cmd.cmd.ssid_len = 3; memcpy(cmd.cmd.ssid, "abc", 3);
    cmd.cmd.seq = 77;                               /* ignored: the shim numbers commands */
    CHECK(wlan_core_apply(w, &cmd) == 1 && wb.cmd.seq == 1 && wb.cmd.cmd == WLAN_CMD_JOIN);
    CHECK(wlan_core_apply(w, &cmd) == 1 && wb.cmd.seq == 2 && memcmp((const void *)wb.cmd.ssid, "abc", 3) == 0);
    wlan_core_fill(w, &io);
    CHECK(io.cmd_seq == 2);
    io.magic = WLANIO_MAGIC;                        /* a plain read written back: no command */
    CHECK(wlan_core_apply(w, &io) == 0 && wb.cmd.seq == 2);
}

/* ---------------------------------------------------------------- config */
static void test_cfg(void)
{
    wifi_cfg_t c;
    char t1[] = "# home\r\nssid=My Net \r\n psk = correct horse\nverbose=1\nunknown=1\n";
    CHECK(wifi_cfg_parse(t1, &c) == 0);
    CHECK(c.ssid_len == 7 && memcmp(c.ssid, "My Net ", 7) == 0);
    CHECK(strcmp(c.psk, " correct horse") == 0 && c.security == WIFI_SEC_WPA2 && c.verbose == 1);
    CHECK(c.backend == WIFI_BE_AUTO && strcmp(c.fw, "0:/ios3ds/wifi-fw.bin") == 0);
    CHECK(c.diag == 1 && c.fwmain == 4);                         /* defaults */
    CHECK(wifi_cfg_check(&c) == NULL);

    {
        char t[] = "diag=0\nfwmain=1\n", bad1[] = "diag=2\n", bad2[] = "fwmain=5\n";
        CHECK(wifi_cfg_parse(t, &c) == 0 && c.diag == 0 && c.fwmain == 1);
        CHECK(wifi_cfg_parse(bad1, &c) == -1 && wifi_cfg_parse(bad2, &c) == -1);
    }
    char t2[] = "ssid=x\npsk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n";
    CHECK(wifi_cfg_parse(t2, &c) == 0 && c.have_pmk && c.pmk[0] == 0xF4 && c.pmk[31] == 0x2E);

    char t3[] = "ssid=open\n";
    CHECK(wifi_cfg_parse(t3, &c) == 0 && c.security == WIFI_SEC_OPEN && wifi_cfg_check(&c) == NULL);
    char t4[] = "ssid=a\npsk=short\n";
    CHECK(wifi_cfg_parse(t4, &c) == -2);
    char t5[] = "security=wpa2\nssid=a\n";
    CHECK(wifi_cfg_parse(t5, &c) == 0 && wifi_cfg_check(&c) != NULL);
    char t6[] = "backend=qemu\nmac=03:00:00:00:00:01\n";
    CHECK(wifi_cfg_parse(t6, &c) == 0 && c.backend == WIFI_BE_QEMU && wifi_cfg_check(&c) != NULL);
    char t7[] = "backend=loop\nmac=02-00-00-00-00-01\n";
    CHECK(wifi_cfg_parse(t7, &c) == 0 && wifi_cfg_check(&c) == NULL && c.mac[5] == 1);
    char t8[] = "ssid=a\nnoequals\n";
    CHECK(wifi_cfg_parse(t8, &c) == -2);
    char t9[] = "ssid=123456789012345678901234567890123\n";
    CHECK(wifi_cfg_parse(t9, &c) == -1);
    char t10[] = "ssid=a\nnettest=http://10.0.2.2:8080/x example.com\n";
    CHECK(wifi_cfg_parse(t10, &c) == 0 && strcmp(c.nettest, "http://10.0.2.2:8080/x example.com") == 0);
    char t11[] = "ssid=a\nnettest=http://0123456789012345678901234567890123456789012345678901234567890123\n";
    CHECK(wifi_cfg_parse(t11, &c) == -2);
}

/* ---------------------------------------------------------------- crypto */
static void sha1(const void *p, u32 n, u8 out[20])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, p, n);
    sha1_final(&c, out);
}

static void aes_wrap(const u8 kek[16], const u8 *in, u32 n, u8 *out)   /* RFC 3394 */
{
    u8 a[8], b[16];
    u32 blocks = n / 8;
    memset(a, 0xA6, 8);
    memcpy(out + 8, in, n);
    for (u32 j = 0; j < 6; j++)
        for (u32 i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(kek, b, b);
            u32 t = blocks * j + i;
            memcpy(a, b, 8);
            a[7] ^= (u8)t; a[6] ^= (u8)(t >> 8); a[5] ^= (u8)(t >> 16); a[4] ^= (u8)(t >> 24);
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

static void test_crypto(void)
{
    u8 d[64];
    sha1("abc", 3, d);
    CHECK(eqhex(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(m, (u32)strlen(m), d);
    CHECK(eqhex(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    static u8 big[1000000];
    memset(big, 'a', sizeof(big));
    sha1(big, sizeof(big), d);
    CHECK(eqhex(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

    hmac_ctx k;                                         /* RFC 2202 cases 2 and 6 */
    hmac_sha1_key(&k, (const u8 *)"Jefe", 4);
    hmac_sha1(&k, (const u8 *)"what do ya want ", 16, (const u8 *)"for nothing?", 12, d);
    CHECK(eqhex(d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    u8 key80[80];
    memset(key80, 0xAA, 80);
    hmac_sha1_key(&k, key80, 80);
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha1(&k, (const u8 *)msg, (u32)strlen(msg), NULL, 0, d);
    CHECK(eqhex(d, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));

    u8 pmk[32];                                         /* IEEE 802.11i H.4 */
    wpa2_pmk("password", (const u8 *)"IEEE", 4, pmk);
    CHECK(eqhex(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    wpa2_pmk("ThisIsAPassword", (const u8 *)"ThisIsASSID", 11, pmk);
    CHECK(eqhex(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));

    u8 key[16], pt[16], ct[16];                         /* FIPS-197 C.1 */
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    aes128_encrypt(key, pt, ct);
    CHECK(eqhex(ct, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(key, ct, d);
    CHECK(memcmp(d, pt, 16) == 0);

    u8 w[24], u[16];                                    /* RFC 3394 4.1 */
    unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", w);
    CHECK(aes_key_unwrap(key, w, 24, u) == 0 && eqhex(u, "00112233445566778899aabbccddeeff"));
    w[3] ^= 1;
    CHECK(aes_key_unwrap(key, w, 24, u) == -1);
    aes_wrap(key, pt, 16, w);
    CHECK(eqhex(w, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
}

/* ------------------------------------------------- 4-way handshake (AP side) */
#define EK_DESC 4
#define EK_INFO 5
#define EK_KEYLEN 7
#define EK_REPLAY 9
#define EK_NONCE 17
#define EK_IV 49
#define EK_RSC 65
#define EK_MIC 81
#define EK_DATALEN 97
#define EK_DATA 99

static void ap_mic(const u8 kck[16], u8 *f, u32 len)
{
    hmac_ctx k;
    u8 d[20];
    memset(f + EK_MIC, 0, 16);
    hmac_sha1_key(&k, kck, 16);
    hmac_sha1(&k, f, len, NULL, 0, d);
    memcpy(f + EK_MIC, d, 16);
}

static u32 ap_frame(u8 *f, u32 info, u64 replay, const u8 *nonce, const u8 *kd, u32 kdlen)
{
    u32 total = EK_DATA + kdlen;
    memset(f, 0, total);
    f[0] = 2; f[1] = 3; f[2] = (u8)((total - 4) >> 8); f[3] = (u8)(total - 4);
    f[4] = 2; f[EK_INFO] = (u8)(info >> 8); f[EK_INFO + 1] = (u8)info;
    f[8] = 16;
    for (int i = 0; i < 8; i++) f[EK_REPLAY + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(f + EK_NONCE, nonce, 32);
    f[EK_DATALEN] = (u8)(kdlen >> 8); f[EK_DATALEN + 1] = (u8)kdlen;
    if (kdlen) memcpy(f + EK_DATA, kd, kdlen);
    return total;
}

static int mic_ok(const u8 kck[16], const u8 *f, u32 len)
{
    u8 c[256], save[16];
    memcpy(c, f, len);
    memcpy(save, c + EK_MIC, 16);
    ap_mic(kck, c, len);
    return memcmp(save, c + EK_MIC, 16) == 0;
}

static void test_handshake(void)
{
    wpa2_t w;
    u8 pmk[32], sta[6] = { 2, 1, 2, 3, 4, 5 }, ap[6] = { 0x10, 9, 8, 7, 6, 5 };
    u8 sn[32], an[32], ptk[48], out[256], f[256], gtk[16], kd[64], wrapped[72];
    u32 olen = 0;
    wpa2_pmk("correct horse battery", (const u8 *)"ios3ds", 6, pmk);
    for (int i = 0; i < 32; i++) { sn[i] = (u8)(i * 7); an[i] = (u8)(0x80 + i); }
    for (int i = 0; i < 16; i++) gtk[i] = (u8)(0xC0 + i);
    wpa2_start(&w, pmk, sta, ap, sn);
    wpa2_ptk(pmk, sta, ap, sn, an, ptk);

    /* M1 */
    u32 n = ap_frame(f, 0x008A, 1, an, NULL, 0);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_M2_SENT);
    CHECK(olen == EK_DATA + 22 && memcmp(out + EK_NONCE, sn, 32) == 0 && mic_ok(ptk, out, olen));
    CHECK(out[EK_INFO] == 0x01 && out[EK_INFO + 1] == 0x0A && out[EK_REPLAY + 7] == 1);
    CHECK(eqhex(out + EK_DATA, "30140100000fac040100000fac040100000fac020000"));

    /* M3: install, ack, MIC, secure, encrypted key data = RSN IE + GTK KDE + pad */
    u32 k = 0;
    memcpy(kd, out + EK_DATA, 22); k = 22;
    const u8 kde[8] = { 0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x01, 0x00 };
    memcpy(kd + k, kde, 8); k += 8;
    memcpy(kd + k, gtk, 16); k += 16;
    kd[k++] = 0xDD; while (k % 8) kd[k++] = 0;
    aes_wrap(ptk + 16, kd, k, wrapped);
    n = ap_frame(f, 0x13CA, 2, an, wrapped, k + 8);
    f[EK_RSC] = 0x42;
    ap_mic(ptk, f, n);
    u8 bad[256];
    memcpy(bad, f, n); bad[EK_MIC] ^= 1;
    CHECK(wpa2_rx_eapol(&w, bad, n, out, sizeof(out), &olen) == WPA2_ERR_MIC);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_KEYS);
    CHECK(memcmp(w.tk, ptk + 32, 16) == 0 && w.gtk_len == 16 && w.gtk_idx == 1);
    CHECK(memcmp(w.gtk, gtk, 16) == 0 && w.rsc[0] == 0x42);
    CHECK(olen == EK_DATA && mic_ok(ptk, out, olen) && out[EK_INFO] == 0x03 && out[EK_INFO + 1] == 0x0A);
    /* replayed M3 */
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_ERR_REPLAY);

    /* group rekey (group message 1), key id 2 */
    k = 0;
    const u8 kde2[8] = { 0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x02, 0x00 };
    memcpy(kd, kde2, 8); k = 8;
    for (int i = 0; i < 16; i++) kd[k++] = (u8)(0x50 + i);
    aes_wrap(ptk + 16, kd, k, wrapped);
    n = ap_frame(f, 0x1382, 3, NULL, wrapped, k + 8);
    ap_mic(ptk, f, n);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_GTK);
    CHECK(w.gtk_idx == 2 && w.gtk[0] == 0x50 && mic_ok(ptk, out, olen));
    CHECK(out[EK_INFO] == 0x03 && out[EK_INFO + 1] == 0x02);

    /* garbage */
    CHECK(wpa2_rx_eapol(&w, f, 50, out, sizeof(out), &olen) == WPA2_ERR_SHORT);
    f[1] = 0;
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_ERR_TYPE);
}

static void test_rsn_parse(void)
{
    rsn_info_t r;
    /* plain WPA2-PSK/CCMP (our own association IE) */
    static const u8 wpa2[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
    CHECK(wpa2_rsn_parse(wpa2, sizeof wpa2, &r) == RSN_MODE_WPA2 && r.ok && r.psk && !r.sae && r.group == 4);
    /* FRITZ!Box "WPA2 + WPA3": AKM PSK + SAE, MFPC, PMKID count 0, group mgmt BIP */
    static const u8 mixed[] = { 0x30, 0x1a, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                0x02, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x0F, 0xAC, 0x08, 0x80, 0x00,
                                0x00, 0x00 };
    CHECK(wpa2_rsn_parse(mixed, sizeof mixed, &r) == RSN_MODE_MIXED && r.psk && r.sae && r.mfpc && !r.mfpr && r.n_akm == 2);
    /* SAE listed first, TKIP group, group mgmt cipher present */
    static const u8 mixed2[] = { 0x30, 0x20, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0x00, 0x0F, 0xAC, 0x02,
                                 0x80, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xAC, 0x06 };
    CHECK(wpa2_rsn_parse(mixed2, sizeof mixed2, &r) == RSN_MODE_MIXED && r.group == 2 && r.tkip && r.ccmp);
    /* WPA3 only: SAE, MFPC + MFPR */
    static const u8 wpa3[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0xc0, 0x00 };
    CHECK(wpa2_rsn_parse(wpa3, sizeof wpa3, &r) == RSN_MODE_WPA3_ONLY && r.mfpr);
    /* PSK but PMF required: we have no PMF, so not joinable either */
    static const u8 pmfreq[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0xc0, 0x00 };
    CHECK(wpa2_rsn_parse(pmfreq, sizeof pmfreq, &r) == RSN_MODE_WPA3_ONLY);
    /* 802.1X (enterprise) */
    static const u8 ent[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                              0x01, 0x00, 0x00, 0x0F, 0xAC, 0x01, 0x00, 0x00 };
    CHECK(wpa2_rsn_parse(ent, sizeof ent, &r) == RSN_MODE_OTHER);
    /* truncated / garbage never reads past n */
    CHECK(wpa2_rsn_parse(mixed, 10, &r) == RSN_MODE_OTHER);
    static const u8 badcount[] = { 0x30, 0x0a, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0xff, 0x00, 0x00, 0x0F };
    CHECK(wpa2_rsn_parse(badcount, sizeof badcount, &r) == RSN_MODE_OTHER);
    static const u8 notrsn[] = { 0xdd, 0x04, 0x00, 0x50, 0xf2, 0x01 };
    CHECK(wpa2_rsn_parse(notrsn, sizeof notrsn, &r) == RSN_MODE_OTHER);
}

/* The IE view handed to iOS's WiFiManager (Ask-to-Join popup and Settings):
   mixed-mode networks must look like WPA2-PSK, SAE-only must stay as it is. */
static void test_rsn_view(void)
{
    rsn_info_t r;
    static const u8 mixed[] = { 0x30, 0x1a, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                0x02, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x0F, 0xAC, 0x08, 0x80, 0x00, 0x00, 0x00 };
    static const u8 wpa2[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
    u8 b[64];
    memset(b, 0xEE, sizeof b);
    memcpy(b, wpa2, sizeof wpa2);
    CHECK(wlan_ie_wpa2_view(b, sizeof wpa2) == sizeof wpa2);
    CHECK(!memcmp(b, wpa2, sizeof wpa2));           /* plain WPA2 untouched */

    memcpy(b, mixed, sizeof mixed);
    CHECK(wlan_ie_wpa2_view(b, sizeof mixed) == sizeof mixed - 4);
    CHECK(b[1] == 22 && b[14] == 1 && !b[15] && !b[20] && !b[21]);  /* one AKM, MFPC cleared */
    CHECK(wpa2_rsn_parse(b, sizeof mixed - 4, &r) == RSN_MODE_WPA2 && r.psk && !r.sae && !r.mfpc);

    /* SAE listed first, group cipher TKIP, another IE after the RSN element */
    static const u8 mixed2[] = { 0x30, 0x20, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0x00, 0x0F, 0xAC, 0x02,
                                 0x80, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xAC, 0x06 };
    static const u8 tail[] = { 0xDD, 0x03, 1, 2, 3 };
    memcpy(b, mixed2, sizeof mixed2);
    memcpy(b + sizeof mixed2, tail, sizeof tail);
    uint32_t n = wlan_ie_wpa2_view(b, sizeof mixed2 + sizeof tail);
    CHECK(n == sizeof mixed2 + sizeof tail - 4);
    CHECK(!memcmp(b + n - sizeof tail, tail, sizeof tail));
    CHECK(wpa2_rsn_parse(b, n - sizeof tail, &r) == RSN_MODE_WPA2 && r.psk && !r.sae && !r.mfpc && r.group == 2);

    static const u8 wpa3[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0xc0, 0x00 };
    memcpy(b, wpa3, sizeof wpa3);
    CHECK(wlan_ie_wpa2_view(b, sizeof wpa3) == sizeof wpa3);
    CHECK(!memcmp(b, wpa3, sizeof wpa3));           /* SAE-only stays unsupported */

    /* SSID + rates + DS channel (an open network): nothing to rewrite */
    static const u8 open[] = { 0, 3, 'a', 'b', 'c', 1, 2, 0x82, 0x84, 3, 1, 6 };
    memcpy(b, open, sizeof open);
    CHECK(wlan_ie_wpa2_view(b, sizeof open) == sizeof open);
    CHECK(!memcmp(b, open, sizeof open));

    /* a truncated RSN IE is never read past the buffer */
    memcpy(b, mixed, 10);
    CHECK(wlan_ie_wpa2_view(b, 10) == 10);
    CHECK(!memcmp(b, mixed, 10));
}

/* ------------------------------------------------------------- WPA v1 / TKIP */
static void test_wpa1_crypto(void)
{
    u8 d[64];
    md5_hash((const u8 *)"", 0, d);             /* RFC 1321 test suite */
    CHECK(eqhex(d, "d41d8cd98f00b204e9800998ecf8427e"));
    md5_hash((const u8 *)"abc", 3, d);
    CHECK(eqhex(d, "900150983cd24fb0d6963f7d28e17f72"));
    md5_hash((const u8 *)"abcdefghijklmnopqrstuvwxyz", 26, d);
    CHECK(eqhex(d, "c3fcd3d76192e4007dfb496cca67e13b"));
    static u8 nulkey[16];
    memset(nulkey, 0x0B, sizeof(nulkey));        /* RFC 2202 cases 1, 2 and 4 */
    hmac_md5(nulkey, (const u8 *)"Hi There", 8, NULL, 0, d);
    CHECK(eqhex(d, "9294727a3638bb1c13f48ef8158bfc9d"));
    u8 jefe[16] = { 0 };
    memcpy(jefe, "Jefe", 4);
    hmac_md5(jefe, (const u8 *)"what do ya want for nothing?", 28, NULL, 0, d);
    CHECK(eqhex(d, "750c783e6ab0b503eaa86e310a5db738"));
    /* RC4 (key "Key", "Wiki": well-known reference vectors) */
    u8 buf[32], k32[32];
    memset(k32, 0, sizeof(k32));
    memcpy(k32, "Key", 3);
    memcpy(buf, "Plaintext", 9);
    wpa_rc4_skip(k32, 3, 0, buf, 9);
    CHECK(eqhex(buf, "bbf316e8d940af0ad3"));
    memset(k32, 0, sizeof(k32));
    memcpy(k32, "Wiki", 4);
    memcpy(buf, "pedia", 5);
    wpa_rc4_skip(k32, 4, 0, buf, 5);
    CHECK(eqhex(buf, "1021bf0420"));
    /* WPA RC4-drop-256: key and plaintext 00..1f, independently computed */
    for (u32 i = 0; i < 32; i++) { k32[i] = (u8)i; buf[i] = (u8)i; }
    wpa_rc4_skip(k32, 32, 256, buf, 32);
    CHECK(eqhex(buf, "9f8b7fdda9150abbd319a32da0d9117d04c2809b6067c9e254a4681f9dab200a"));

    u8 pmk[32], sn[32], an[32], sta[6] = { 2, 1, 2, 3, 4, 5 }, ap[6] = { 0x10, 9, 8, 7, 6, 5 };
    u8 p48[48], p64[64];
    wpa2_pmk("password", (const u8 *)"IEEE", 4, pmk);
    for (u32 i = 0; i < 32; i++) { sn[i] = (u8)i; an[i] = (u8)(0x80 + i); }
    wpa2_ptk(pmk, sta, ap, sn, an, p48);
    wpa1_ptk(pmk, sta, ap, sn, an, p64);
    CHECK(!memcmp(p48, p64, 48));                /* same PRF, TKIP asks for 64 */
    CHECK(!eqhex(p64 + 48, "00000000000000000000000000000000"));
}

static void test_wpa1_parse(void)
{
    u8 ie[32], bad[32];
    rsn_info_t r;
    wpa2_t w;
    memset(&w, 0, sizeof(w));
    u32 n = wpa1_ie(&w, ie);
    CHECK(n == 26 && ie[1] == 24 && ie[6] == 1);
    CHECK(wpa1_ie_parse(ie, n, &r) == RSN_MODE_WPA1);
    CHECK(r.ok && r.wpa1 && r.group == 2 && r.tkip && r.psk && !r.ccmp);
    memcpy(bad, ie, n);
    bad[5] = 0xF2;                                /* OUI type must be 01 */
    CHECK(wpa1_ie_parse(bad, n, &r) == RSN_MODE_OTHER);
    memcpy(bad, ie, n);
    bad[23] = 3;                                  /* AKM 00:50:F2:03, not PSK */
    CHECK(wpa1_ie_parse(bad, n, &r) == RSN_MODE_OTHER);
    CHECK(wpa1_ie_parse(ie, 10, &r) == RSN_MODE_OTHER);
}

static void wpa1_mic(const u8 kck[16], u8 *f, u32 len)
{
    u8 m[16];
    u8 c[512];
    if (len > sizeof(c)) return;
    memcpy(c, f, len);
    memset(c + EK_MIC, 0, 16);
    hmac_md5(kck, c, len, NULL, 0, m);
    memcpy(f + EK_MIC, m, 16);
}

static int wpa1_mic_ok(const u8 kck[16], const u8 *f, u32 len)
{
    u8 c[512], m[16];
    if (len > sizeof(c)) return 0;
    memcpy(c, f, len);
    memcpy(m, c + EK_MIC, 16);
    wpa1_mic(kck, c, len);
    return !memcmp(m, c + EK_MIC, 16);
}

static u32 wpa1_frame(u8 *f, u32 info, u64 replay, const u8 *nonce, const u8 *iv,
                      const u8 *kd, u32 dlen, u32 keylen)
{
    u32 total = EK_DATA + dlen;
    memset(f, 0, total);
    f[0] = 1; f[1] = 3; f[2] = (u8)((total - 4) >> 8); f[3] = (u8)(total - 4);
    f[4] = 0xFE; f[EK_INFO] = (u8)(info >> 8); f[EK_INFO + 1] = (u8)info;
    f[EK_KEYLEN] = (u8)(keylen >> 8); f[EK_KEYLEN + 1] = (u8)keylen;
    for (int i = 0; i < 8; i++) f[EK_REPLAY + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(f + EK_NONCE, nonce, 32);
    if (iv) memcpy(f + EK_IV, iv, 16);
    f[EK_DATALEN] = (u8)(dlen >> 8); f[EK_DATALEN + 1] = (u8)dlen;
    if (dlen) memcpy(f + EK_DATA, kd, dlen);
    return total;
}

static void test_wpa1_handshake(void)
{
    wpa2_t w;
    u8 pmk[32], sn[32], an[32], iv[32], ptk[64], sta[6] = { 2, 1, 2, 3, 4, 5 };
    u8 ap[6] = { 0x10, 9, 8, 7, 6, 5 }, gtk[32], kd[128], crypt[128], out[256], f[256];
    u8 wpaie[26];
    u32 olen = 0;
    wpa2_pmk("mixed-mode-pass", (const u8 *)"wpa1-net", 8, pmk);
    for (u32 i = 0; i < 32; i++) { sn[i] = (u8)(i * 3); an[i] = (u8)(i + 1); iv[i] = (u8)(0xA0 + i); }
    for (u32 i = 0; i < 32; i++) gtk[i] = (u8)(0x40 + i);
    memset(&w, 0, sizeof(w));
    wpa1_start(&w, pmk, sta, ap, sn);
    wpa1_ptk(pmk, sta, ap, sn, an, ptk);
    {
        wpa2_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        wpa1_ie(&tmp, wpaie);
        memcpy(w.own_ie, wpaie, sizeof(wpaie));
        w.own_ie_len = sizeof(wpaie);
    }

    /* M1: no MIC; WPA v1 is descriptor FE, version 0, Key Length 32 */
    u32 n = wpa1_frame(f, 0x0088, 1, an, NULL, NULL, 0, 32);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_M2_SENT);
    CHECK(olen == EK_DATA + sizeof(wpaie) && out[EK_DESC] == 0xFE && out[EK_KEYLEN + 1] == 32);
    CHECK(out[EK_INFO] == 0x01 && out[EK_INFO + 1] == 0x08);
    CHECK(!memcmp(out + EK_DATA, wpaie, sizeof(wpaie)));
    CHECK(wpa1_mic_ok(ptk, out, olen));

    /* M3: WPA IE + TKIP GTK KDE, RC4(key_iv||KEK) with 256-byte drop */
    u32 k = 0;
    memcpy(kd, wpaie, sizeof(wpaie)); k = sizeof(wpaie);
    const u8 kde[8] = { 0xDD, 38, 0, 0x0F, 0xAC, 0x01, 0x01, 0 };
    memcpy(kd + k, kde, 8); k += 8;
    memcpy(kd + k, gtk, 32); k += 32;
    u8 ek[32];
    memcpy(ek, iv, 16);
    memcpy(ek + 16, ptk + 16, 16);
    memcpy(crypt, kd, k);
    wpa_rc4_skip(ek, 32, 256, crypt, k);
    n = wpa1_frame(f, 0x13A8, 2, an, iv, crypt, k, 32);
    f[EK_RSC] = 0x17;
    wpa1_mic(ptk, f, n);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_KEYS);
    CHECK(!memcmp(w.tk, ptk + 32, 32) && w.gtk_len == 32 && w.gtk_idx == 1);
    CHECK(!memcmp(w.gtk, gtk, 32) && w.rsc[0] == 0x17);
    CHECK(out[EK_DESC] == 0xFE && out[EK_KEYLEN + 1] == 0 && !memcmp(out + EK_KEYLEN, "\0\0", 2));
    CHECK(out[EK_INFO] == 0x03 && out[EK_INFO + 1] == 0x08);   /* pairwise|MIC|secure */
    CHECK(wpa1_mic_ok(ptk, out, olen));
    /* a wrong MIC is rejected */
    u8 bad[256];
    memcpy(bad, f, n); bad[EK_MIC] ^= 1;
    wpa2_t w2;
    memcpy(&w2, &w, sizeof(w2));
    w2.have_replay = 0;                           /* allow the same counter again */
    CHECK(wpa2_rx_eapol(&w2, bad, n, out, sizeof(out), &olen) == WPA2_ERR_MIC);

    /* group rekey with a new 32-byte TKIP GTK */
    k = 0;
    const u8 kde2[8] = { 0xDD, 38, 0, 0x0F, 0xAC, 0x01, 0x02, 0 };
    memcpy(kd, kde2, 8); k += 8;
    for (u32 i = 0; i < 32; i++) kd[k++] = (u8)(0x80 + i);
    memcpy(crypt, kd, k);
    wpa_rc4_skip(ek, 32, 256, crypt, k);
    n = wpa1_frame(f, 0x1380, 3, an, iv, crypt, k, 0);
    wpa1_mic(ptk, f, n);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_GTK);
    CHECK(w.gtk_idx == 2 && w.gtk[0] == 0x80);
}

/* ---- AR6K credit/defer path --------------------------------------------- */
static void test_ar6k_credits(void)
{
    const u8 body[4] = { 1, 2, 3, 4 };
    const u8 credit_rep[] = { 1, 4, 2, 1, 0, 0 };   /* trailer: ep2 +1 credit */
    u8 eth[64], framed[64];
    u32 waits;

    memset(&A, 0, sizeof(A));
    A.cred_sz = 1664;
    A.ep_wmi = 1;
    A.ep_data = 2;
    d_head = d_count = 0;
    eapol_pending_len = 0;
    mock_reset();
    host_wait_cycles = 0;

    /* no report: one step's worth of waits, not the old ~4 s busy loop */
    CHECK(credit_wait(2, 1) == SDIO_ERR_BUSY);
    CHECK(host_wait_cycles == CREDIT_WAIT_POLLS * 3350);
    CHECK(A.credit_busy == 1);

    /* a trailer credit ends the wait at once; the packet is kept for pump() */
    mock_mailbox_frame(2, body, sizeof(body), credit_rep, sizeof(credit_rep));
    CHECK(credit_wait(2, 1) == 0);
    CHECK(A.ep_credits[2] == 1 && d_count == 1);
    CHECK(host_wait_cycles == CREDIT_WAIT_POLLS * 3350);
    d_head = d_count = 0;

    /* data frames report BUSY without waiting; EAPOL is retried, not lost */
    A.state = NET_ST_UP;
    A.ep_credits[2] = 0;
    memset(eth, 0, sizeof(eth));
    eth[12] = 0x08; eth[13] = 0x00;                 /* IPv4 Ethernet frame */
    memset(framed, 0x5A, sizeof(framed));
    waits = host_wait_cycles;
    CHECK(ar6k_tx(eth, sizeof(eth)) == -1);
    CHECK(host_wait_cycles == waits);
    CHECK(eapol_send(framed, sizeof(framed)) == SDIO_ERR_BUSY);
    CHECK(eapol_pending_len == sizeof(framed) && host_wait_cycles == waits);
    A.ep_credits[2] = 2;
    mock_reset();
    eapol_pending_step();
    CHECK(eapol_pending_len == 0 && mock_write_calls == 1);
    CHECK(host_wait_cycles == waits);
}

static void test_ar6k_write_credits(void)
{
    const u8 body[8] = { 0 };

    memset(&A, 0, sizeof(A));
    A.cred_sz = 1664;
    A.ep_credits[2] = 1;
    A.ep_cred_per_msg[2] = 2;           /* the send sets NEED_CREDIT_UPDATE */
    A.credit_used = 10;
    mock_reset();

    /* SDIO write fails: the target never sees the message, credits come back */
    mock_write_err = SDIO_ERR_CARD;
    CHECK(mbox_send(2, 0, 0, body, sizeof(body), 0) == SDIO_ERR_CARD);
    CHECK(A.ep_credits[2] == 1 && A.credit_used == 10 && A.credit_refund == 1);
    CHECK(A.credit_low == 0);
    /* a successful send still consumes exactly one credit */
    mock_write_err = 0;
    CHECK(mbox_send(2, 0, 0, body, sizeof(body), 0) == 0);
    CHECK(A.ep_credits[2] == 0 && A.credit_used == 11);
    /* same refund in the CMD52 byte-mode fallback */
    A.ep_credits[2] = 1;
    A.byte_mode = 1;
    mock_w8_err = SDIO_ERR_CARD;
    CHECK(mbox_send(2, 0, 0, body, sizeof(body), 0) == SDIO_ERR_CARD);
    CHECK(A.ep_credits[2] == 1 && A.credit_used == 11 && A.credit_refund == 2);
    CHECK(mock_w8_calls > 0);
}

static void test_ar6k_defer_ring(void)
{
    const u8 body[4] = { 9, 8, 7, 6 };

    memset(&A, 0, sizeof(A));
    A.cred_sz = 1664;
    A.ep_wmi = 1;                       /* 0xFE below is an unknown packet type */
    A.ep_data = 2;
    d_head = d_count = 0;
    mock_reset();
    host_wait_cycles = 0;

    for (u32 i = 0; i < DEFER_MAX; i++)
        CHECK(defer_msg(0xFE, body, sizeof(body)) == 0);
    CHECK(d_count == DEFER_MAX && A.defer_drop == 0);
    CHECK(defer_msg(0xFE, body, sizeof(body)) == SDIO_ERR_BUSY);
    CHECK(d_count == DEFER_MAX && A.defer_drop == 1);

    /* credit_wait stops draining on overflow: the extra frames stay in the
       hardware mailbox instead of being silently dropped */
    d_head = d_count = 0;
    A.defer_drop = 0;
    for (u32 i = 0; i < DEFER_MAX + 5; i++)
        mock_mailbox_frame(0xFE, body, sizeof(body), NULL, 0);
    CHECK(credit_wait(2, 1) == SDIO_ERR_BUSY);
    CHECK(d_count == DEFER_MAX && A.defer_full == 1 && A.defer_drop == 0);
    CHECK(mock_rx_frames == 5 && host_wait_cycles == 0);

    /* pump() runs every deferred packet once, including the last one */
    u32 before = A.defer_drop;
    pump();
    CHECK(d_count == 0 && A.defer_drop == before);
}

static void test_wpa2_long_frame(void)
{
    wpa2_t w;
    u8 f[1024], out[1024], keydata[512];
    u32 olen = 123;
    const u8 pmk[32] = { 1 };
    memset(keydata, 0, sizeof(keydata));

    /* WPA v1 MICs use a 512-byte copy; anything longer must be rejected
       before the caller compares mic[] with the frame. */
    memset(&w, 0, sizeof(w));
    w.wpa1 = 1;
    w.have_ptk = 1;
    memcpy(w.kck, pmk, 16);
    u32 n = wpa1_frame(f, 0x0188, 1, NULL, NULL, keydata, 450, 0);  /* pairwise|MIC|ACK */
    CHECK(n == EK_DATA + 450);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_ERR_LONG);
    CHECK(olen == 0 && !w.have_replay && !w.done);
}

static void test_ar6k_wmi_queue(void)
{
    const u8 p[4] = { 1, 2, 3, 4 };

    memset(&A, 0, sizeof(A));
    A.cred_sz = 1664;
    A.ep_wmi = 1;
    A.ep_credits[1] = 0;
    wmi_queue_clear();
    mock_reset();

    /* without credits WMI control is queued, not dropped, and stays ordered */
    CHECK(wmi_send(0x22, p, sizeof(p)) == 0);
    CHECK(wmi_send(0x16, p, sizeof(p)) == 0);
    CHECK(wq_count == 2 && A.wmi_busy == 2 && mock_write_calls == 0);
    A.ep_credits[1] = 1;
    CHECK(wmi_queue_flush() == SDIO_ERR_BUSY);
    CHECK(wq_count == 1 && mock_write_calls == 1 && mock_tx[6] == 0x22);
    A.ep_credits[1] = 1;
    CHECK(wmi_queue_flush() == 0);
    CHECK(wq_count == 0 && mock_write_calls == 2 && mock_tx[6] == 0x16);
}


/* issue 1: a home router's probe response with a long country IE and HT caps
   ahead of the RSN IE. The RSN IE must survive the 76-byte IE copy, or iOS
   takes the network for WEP. */
static void test_scan_ie_priority(void)
{
    u8 f[28 + 200];
    u32 n = 28, i;
    memset(f, 0, sizeof f);
    f[0] = 0xA8; f[1] = 0x09;                       /* 2472 MHz */
    f[2] = 2; f[3] = 30; f[4] = 0xBF; f[5] = 0xFF;  /* probe resp, snr, rssi -65 */
    f[6] = 0x02; f[11] = 0x77;                      /* BSSID */
    f[24] = 100; f[26] = 0x11; f[27] = 0x04;        /* interval, caps (privacy) */
    static const u8 ssid[] = { 0, 4, 'h', 'o', 'm', 'e' };
    static const u8 rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    static const u8 ds[] = { 3, 1, 13 };
    memcpy(f + n, ssid, sizeof ssid); n += sizeof ssid;
    memcpy(f + n, rates, sizeof rates); n += sizeof rates;
    memcpy(f + n, ds, sizeof ds); n += sizeof ds;
    f[n] = 7; f[n + 1] = 41; f[n + 2] = 'D'; f[n + 3] = 'E'; f[n + 4] = ' ';
    for (i = 0; i < 13; i++) { f[n + 5 + 3 * i] = (u8)(i + 1); f[n + 6 + 3 * i] = 1; f[n + 7 + 3 * i] = 20; }
    n += 43;
    f[n] = 45; f[n + 1] = 26; n += 28;              /* HT capabilities */
    static const u8 rsn[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                              0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x0c, 0x00 };
    memcpy(f + n, rsn, sizeof rsn); n += sizeof rsn;
    nscan = 0;
    scan_record(f, n);
    CHECK(nscan == 1);
    const wlan_bss_t *b = &scan_tab[0];
    CHECK(b->ie_len <= WLAN_IE_MAX);
    CHECK((b->flags & WLAN_BSS_RSN) && b->ssid_len == 4 && b->channel_mhz == 2472);
    int found = 0;
    for (u32 p = 0; p + 2 <= b->ie_len; p += 2 + b->ie[p + 1])
        if (b->ie[p] == 0x30 && b->ie[p + 1] == sizeof rsn - 2 && !memcmp(b->ie + p, rsn, sizeof rsn)) found = 1;
    CHECK(found);
    CHECK(b->ie[0] == 0 && b->ie[1] == 4);          /* SSID still first */
    nscan = 0;
}

int main(void)
{
    test_ring();
    test_wlan();
    test_frames();
    test_htc_credits();
    test_ar6k_credits();
    test_ar6k_write_credits();
    test_ar6k_wmi_queue();
    test_ar6k_defer_ring();
    test_cfg();
    test_crypto();
    test_handshake();
    test_wpa1_crypto();
    test_wpa1_parse();
    test_wpa1_handshake();
    test_wpa2_long_frame();
    test_rsn_parse();
    test_rsn_view();
    test_scan_ie_priority();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
