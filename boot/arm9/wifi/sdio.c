/*
 * SDIO host for the 3DS Wi-Fi slot (TMIO controller 2, 0x10122000), polling.
 *
 * UNTESTED ON HARDWARE. 3dbrew "IO Registers": 0x10122000 (WIFI SDIO) is mapped
 * for both the ARM11 and the ARM9; the ARM9 interrupt controller has no line
 * for it (only SDIO_1/SDIO_3), hence polling. Register map and command encoding: GBATEK "DSi SD/MMC
 * Protocol and I/O Ports", linux-3ds drivers/platform/nintendo3ds/ctr_sdhc.h,
 * and the working ARM9 driver for controller 1 in boot/arm9/sdmmc.c (same IP
 * block). Enumeration order follows dsiwifi's wifi_card_device_init (MIT).
 * Every wait is bounded; a missing controller (our QEMU board: the block
 * reads 0) ends in SDIO_ERR_NOCARD / SDIO_ERR_TIMEOUT instead of a hang.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdio.h"
#include "util.h"

#define TMIO2            0x10122000u
#define CFG9_SDMMC_CTL   (*(vu16 *)0x10000020u)     /* bit2: controller 2 port 0 power, 0 = on */

#define R_CMD        0x00
#define R_PORTSEL    0x02
#define R_ARG0       0x04
#define R_ARG1       0x06
#define R_STOP       0x08
#define R_BLKCNT     0x0A
#define R_RESP0      0x0C
#define R_RESP1      0x0E
#define R_STAT0      0x1C
#define R_STAT1      0x1E
#define R_MASK0      0x20
#define R_MASK1      0x22
#define R_CLKCTL     0x24
#define R_BLKLEN     0x26
#define R_OPT        0x28
#define R_DATACTL    0xD8
#define R_RESET      0xE0
#define R_DATA32CTL  0x100
#define R_BLKLEN32   0x104
#define R_BLKCNT32   0x108
#define R_FIFO32     0x10C

#define STAT0_RESPEND   0x0001
#define STAT0_DATAEND   0x0004
#define STAT1_ERRORS    0x807F      /* illegal access, timeouts, CRC, FIFO errors */
#define STAT1_CMDTMO    0x0040
#define STAT1_RXRDY     0x0100
#define STAT1_TXRQ      0x0200
#define STAT1_BUSY      0x4000
#define D32_RXRDY       0x0100      /* DATA32CTL: FIFO holds a block */

/* command register: index | response type << 8 | data flags */
#define RSP_R1      (4u << 8)
#define RSP_R1B     (5u << 8)
#define RSP_R3      (7u << 8)       /* OCR, no CRC (CMD5's R4) */
#define CMD_DATA    (1u << 11)
#define CMD_READ    (1u << 12)
#define CMD_MULTI   (1u << 13)
#define CMD_SDIO    (1u << 14)      /* "secure"/SDIO flag dsiwifi sets on CMD53 */

#define WAIT_LOOPS  2000000u        /* ~0.1 s per command at worst */
#define FIFO_MIRROR 0x200000u       /* dsiwifi's 3DS ARM11 FIFO mirror */

static u32 last_status, rca;
/* TMIO1's 32-bit data FIFO is at +0x10C. Every Old-3DS log so far reads
   zeroes there even though CMD53 reports success, and dsiwifi's 3DS build
   uses the +0x200000 mirror instead. Keep both addresses selectable so the
   self-test can try the mirror before giving up and using CMD52. */
static u32 fifo32_reg = TMIO2 + R_FIFO32;
u32 sdio_fifo_mirror(int on)
{
    fifo32_reg = TMIO2 + (on ? FIFO_MIRROR : 0u) + R_FIFO32;
    return fifo32_reg;
}
static sdio_diag_t D;
const sdio_diag_t *sdio_diag(void) { return &D; }

static inline u16 rd16(u32 r) { return *(vu16 *)(TMIO2 + r); }
static inline void wr16(u32 r, u16 v) { *(vu16 *)(TMIO2 + r) = v; }
static inline void mask16(u32 r, u16 clr, u16 set) { wr16(r, (u16)((rd16(r) & ~clr) | set)); }
void waitcycles(u32 n);

u32 sdio_last_status(void) { return last_status; }

/* One command, optional single data phase of `len` bytes in SDIO_BLOCK units
   (or one short block of blklen bytes when blklen != SDIO_BLOCK). */
static int command(u32 cmd, u32 arg, u8 *rbuf, const u8 *wbuf, u32 len, u32 blklen, u32 *resp)
{
    u32 guard = 0;
    while (rd16(R_STAT1) & STAT1_BUSY)
        if (++guard > WAIT_LOOPS) return SDIO_ERR_TIMEOUT;
    wr16(R_STAT0, 0);
    wr16(R_STAT1, 0);
    wr16(R_ARG0, (u16)arg);
    wr16(R_ARG1, (u16)(arg >> 16));
    if (cmd & CMD_DATA) {
        u16 nblk = (u16)(len / blklen);
        wr16(R_BLKLEN, (u16)blklen);
        wr16(R_BLKCNT, nblk);
        wr16(R_BLKLEN32, (u16)blklen);
        wr16(R_BLKCNT32, nblk);
        wr16(R_DATACTL, 0x0002);                            /* 32-bit FIFO */
        wr16(R_DATA32CTL, (cmd & CMD_READ) ? 0x0C02 : 0x1402);  /* clear + enable */
    }
    wr16(R_CMD, (u16)cmd);

    u32 left = (cmd & CMD_DATA) ? len : 0;
    u16 want = STAT0_RESPEND | ((cmd & CMD_DATA) ? STAT0_DATAEND : 0);
    for (guard = 0;; guard++) {
        u16 s1 = rd16(R_STAT1), d32 = rd16(R_DATA32CTL);
        if (left && (cmd & CMD_READ) && (d32 & D32_RXRDY)) {
            mask16(R_STAT1, STAT1_RXRDY, 0);
            for (u32 i = 0; i < blklen; i += 4) {
                u32 v = *(vu32 *)fifo32_reg;
                rbuf[0] = (u8)v; rbuf[1] = (u8)(v >> 8); rbuf[2] = (u8)(v >> 16); rbuf[3] = (u8)(v >> 24);
                rbuf += 4;
            }
            left -= blklen;
            guard = 0;
        } else if (left && !(cmd & CMD_READ) && (s1 & STAT1_TXRQ)) {    /* as dsiwifi */
            mask16(R_STAT1, STAT1_TXRQ, 0);
            for (u32 i = 0; i < blklen; i += 4) {
                *(vu32 *)fifo32_reg = (u32)wbuf[0] | (u32)wbuf[1] << 8 | (u32)wbuf[2] << 16 | (u32)wbuf[3] << 24;
                wbuf += 4;
            }
            left -= blklen;
            guard = 0;
        }
        if (s1 & STAT1_ERRORS) {
            last_status = s1 | (u32)rd16(R_STAT0) << 16;
            wr16(R_STAT0, 0);
            wr16(R_STAT1, 0);
            return (s1 & STAT1_CMDTMO) ? SDIO_ERR_NOCARD : SDIO_ERR_CARD;
        }
        /* dsiwifi ends a block transfer on RESPEND + all blocks moved (DATAEND
           is not waited for); plain commands on RESPEND with the bus idle */
        u16 s0 = rd16(R_STAT0);
        if ((cmd & CMD_DATA) ? (!left && (s0 & STAT0_RESPEND)) : (!(s1 & STAT1_BUSY) && (s0 & want) == want))
            break;
        if (guard > WAIT_LOOPS) {
            last_status = s1 | (u32)rd16(R_STAT0) << 16;
            return SDIO_ERR_TIMEOUT;
        }
    }
    last_status = rd16(R_STAT1) | (u32)rd16(R_STAT0) << 16;
    wr16(R_STAT0, 0);
    wr16(R_STAT1, 0);
    if (resp) *resp = rd16(R_RESP0) | (u32)rd16(R_RESP1) << 16;
    return SDIO_OK;
}

static void set_clock(u16 div)
{
    mask16(R_CLKCTL, 0x100, 0);
    mask16(R_CLKCTL, 0x2FF, div & 0x2FF);
    mask16(R_CLKCTL, 0, 0x100);
}

int sdio_r8(u32 func, u32 addr)
{
    u32 resp = 0;
    int r = command(52 | RSP_R1, (func & 7) << 28 | (addr & 0x1FFFF) << 9, NULL, NULL, 0, 0, &resp);
    return r < 0 ? r : (int)(resp & 0xFF);
}

int sdio_w8(u32 func, u32 addr, u8 val)
{
    return command(52 | RSP_R1, 0x80000000u | (func & 7) << 28 | (addr & 0x1FFFF) << 9 | val,
                   NULL, NULL, 0, 0, NULL);
}

int sdio_read_blocks(u32 func, u32 addr, u8 *buf, u32 len)
{
    if (!len || len % SDIO_BLOCK || len / SDIO_BLOCK > 0x1FF) return SDIO_ERR_ARG;
    wr16(R_STOP, 0x100);
    /* block mode (bit 27), incrementing address (bit 26), block count */
    return command(53 | RSP_R1 | CMD_DATA | CMD_READ | CMD_MULTI | CMD_SDIO,
                   (func & 7) << 28 | 1u << 27 | 1u << 26 | (addr & 0x1FFFF) << 9 | len / SDIO_BLOCK,
                   buf, NULL, len, SDIO_BLOCK, NULL);
}

int sdio_read_bytes(u32 func, u32 addr, u8 *buf, u32 len)
{
    if (!len || len > 512 || (len & 3)) return SDIO_ERR_ARG;
    /* byte mode (bit 27 = 0), incrementing address, count = len (512 = 0) */
    return command(53 | RSP_R1 | CMD_DATA | CMD_READ | CMD_SDIO,
                   (func & 7) << 28 | 1u << 26 | (addr & 0x1FFFF) << 9 | (len & 0x1FF),
                   buf, NULL, len, len, NULL);
}

int sdio_write_blocks(u32 func, u32 addr, const u8 *buf, u32 len)
{
    if (!len || len % SDIO_BLOCK || len / SDIO_BLOCK > 0x1FF) return SDIO_ERR_ARG;
    wr16(R_STOP, 0x100);
    return command(53 | RSP_R1 | CMD_DATA | CMD_MULTI | CMD_SDIO,
                   0x80000000u | (func & 7) << 28 | 1u << 27 | 1u << 26 | (addr & 0x1FFFF) << 9 | len / SDIO_BLOCK,
                   NULL, buf, len, SDIO_BLOCK, NULL);
}

int sdio_init(void)
{
    u32 resp = 0, ocr = 0;
    int r, v;

    memset(&D, 0, sizeof(D));
    D.stage = "controller";
    D.cfg9_before = CFG9_SDMMC_CTL;
    CFG9_SDMMC_CTL = (u16)(CFG9_SDMMC_CTL & ~0x0004);   /* port power on */
    D.cfg9_after = CFG9_SDMMC_CTL;
    /* controller init, as boot/arm9/sdmmc.c does for controller 1 */
    mask16(R_DATA32CTL, 0x0800, 0);
    mask16(R_DATA32CTL, 0x1000, 0);
    mask16(R_DATA32CTL, 0, 0x0402);
    mask16(R_DATACTL, 0x0022, 0x0002);
    wr16(R_BLKLEN32, SDIO_BLOCK);
    wr16(R_BLKCNT32, 1);
    mask16(R_RESET, 0x0003, 0);
    mask16(R_RESET, 0, 0x0003);
    wr16(R_MASK0, 0xFFFF);                  /* no interrupts: we poll */
    wr16(R_MASK1, 0xFFFF);
    mask16(R_PORTSEL, 0x0003, 0);
    wr16(R_CLKCTL, 0x0020);                 /* HCLK/128 for enumeration */
    wr16(R_OPT, 0x40EE | 0x8000);           /* 1-bit bus until the card says otherwise */
    wr16(R_BLKLEN, SDIO_BLOCK);
    wr16(R_STOP, 0x0100);
    set_clock(0x0020);
    waitcycles(335000);                     /* ~10 ms */
    D.opt = rd16(R_OPT);
    if ((D.opt & 0x00FF) != 0x00EE) return SDIO_ERR_TIMEOUT;  /* no controller here */

    /* CMD5 until the card reports ready, echoing its OCR back. Bounded: each
       command ends on a response, a CMD timeout or ~0.1 s; at most 200 tries
       1 ms apart. */
    D.stage = "CMD5";
    for (int tries = 0;; tries++) {
        D.cmd5_tries = (u32)tries + 1;
        r = command(5 | RSP_R3, ocr, NULL, NULL, 0, 0, &resp);
        if (r < 0) return r == SDIO_ERR_CARD ? SDIO_ERR_NOCARD : r;
        if (!tries) D.ocr_first = resp;
        D.ocr = resp;
        if (resp & 0x80000000u) break;
        ocr = resp & 0x00FFFFFFu;
        if (tries > 200) return SDIO_ERR_NOCARD;
        waitcycles(33500);
    }
    D.stage = "CMD3 (RCA)";
    r = command(3 | RSP_R1, 0, NULL, NULL, 0, 0, &resp);
    if (r < 0) return r;
    rca = resp >> 16;
    D.rca = rca;
    D.stage = "CMD7 (select)";
    r = command(7 | RSP_R1B, rca << 16, NULL, NULL, 0, 0, NULL);
    if (r < 0) return r;

    /* CCCR: reset function enables, 4-bit bus + CD disable, power, block sizes */
    D.stage = "CCCR";
    if ((r = sdio_w8(0, 0x02, 0x00)) < 0) return r;
    if ((v = sdio_r8(0, 0x00)) >= 0) D.cccr_rev = (u8)v;
    if ((v = sdio_r8(0, 0x01)) >= 0) D.sd_rev = (u8)v;
    if ((v = sdio_r8(0, 0x08)) >= 0) D.caps = (u8)v;
    int bic = sdio_r8(0, 0x07);
    if (bic < 0) return bic;
    if ((r = sdio_w8(0, 0x07, (u8)(bic | 0x82))) < 0) return r;
    if ((v = sdio_r8(0, 0x07)) >= 0) D.bic = (u8)v;
    mask16(R_OPT, 0x8000, 0);               /* controller: 4-bit */
    set_clock(0x0001);                      /* HCLK/4 = 16.8 MHz: inside SDIO default speed (25 MHz) */
    if ((r = sdio_w8(0, 0x12, 0x02)) < 0) return r;
    if ((r = sdio_w8(0, 0x110, SDIO_BLOCK & 0xFF)) < 0) return r;
    if ((r = sdio_w8(0, 0x111, SDIO_BLOCK >> 8)) < 0) return r;
    if ((r = sdio_w8(0, 0x10, SDIO_BLOCK & 0xFF)) < 0) return r;
    if ((r = sdio_w8(0, 0x11, SDIO_BLOCK >> 8)) < 0) return r;
    D.stage = "function 1 enable";
    if ((r = sdio_w8(0, 0x02, 0x02)) < 0) return r;     /* enable function 1 */
    for (int tries = 0; tries < 1000; tries++) {
        int rdy = sdio_r8(0, 0x03);
        D.ready_tries = (u32)tries + 1;
        if (rdy < 0) return rdy;
        if (rdy & 0x02) { D.stage = "done"; return SDIO_OK; }
        waitcycles(33500);
    }
    return SDIO_ERR_TIMEOUT;
}

int sdio_manfid(u16 *manf, u16 *card)
{
    u32 p = 0;
    for (int i = 0; i < 3; i++) {
        int b = sdio_r8(0, 0x09 + (u32)i);
        if (b < 0) return b;
        p |= (u32)b << (8 * i);
    }
    for (int n = 0; n < 16 && p < 0x20000; n++) {          /* walk at most 16 tuples */
        int code = sdio_r8(0, p), link = code < 0 || code == 0xFF ? -1 : sdio_r8(0, p + 1);
        if (code < 0 || code == 0xFF || link < 0) return SDIO_ERR_CARD;
        if (code == 0x20 && link >= 4) {
            int b[4];
            for (int i = 0; i < 4; i++) if ((b[i] = sdio_r8(0, p + 2 + (u32)i)) < 0) return b[i];
            *manf = (u16)(b[0] | b[1] << 8);
            *card = (u16)(b[2] | b[3] << 8);
            return 0;
        }
        p += 2 + (u32)link;
    }
    return SDIO_ERR_CARD;
}
