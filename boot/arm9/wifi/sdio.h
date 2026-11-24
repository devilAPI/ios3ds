/*
 * SDIO host for the 3DS Wi-Fi slot: TMIO controller 2 at 0x10122000, driven
 * from the ARM9 by polling (its interrupt, GIC 64/65, only exists on the ARM11).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"

#define SDIO_BLOCK 128u         /* function 1 block size (same as NWM / dsiwifi) */

enum {
    SDIO_OK = 0,
    SDIO_ERR_TIMEOUT = -1,      /* controller never finished the command */
    SDIO_ERR_CARD = -2,         /* error bits in STAT1 (see sdio_last_status) */
    SDIO_ERR_NOCARD = -3,       /* no answer to CMD5: chip unpowered / held in reset */
    SDIO_ERR_ARG = -4,
    SDIO_ERR_BUSY = -5,         /* no HTC credits left: retry the frame later */
};

/* Power the port, reset the controller, enumerate the card (CMD5/CMD3/CMD7),
   switch to 4-bit, set the block size and enable function 1. */
int sdio_init(void);
/* CMD52. sdio_r8 returns the byte (0..255) or a negative SDIO_ERR_*. */
int sdio_r8(u32 func, u32 addr);
int sdio_w8(u32 func, u32 addr, u8 val);
/* CMD53, block mode, fixed address. len must be a multiple of SDIO_BLOCK and
   buf 4-byte aligned. */
int sdio_read_blocks(u32 func, u32 addr, u8 *buf, u32 len);
int sdio_write_blocks(u32 func, u32 addr, const u8 *buf, u32 len);
/* CMD53, byte mode, incrementing address, one transfer of len (4..512,
   multiple of 4) bytes: used to check that the 32-bit FIFO works. */
int sdio_read_bytes(u32 func, u32 addr, u8 *buf, u32 len);
/* Pick the data FIFO used by CMD53: +0x10C, or the +0x200000 mirror that
   dsiwifi's 3DS build uses. Only the data register moves; command/status
   stay at TMIO2. Returns the register now in use. */
u32 sdio_fifo_mirror(int on);
/* STAT1 | STAT0 << 16 of the last command, for the log. */
u32 sdio_last_status(void);

/* What sdio_init() saw, for the [wifi-diag] log (docs/wifi.md section 6). */
typedef struct {
    const char *stage;          /* last step started: "controller", "CMD5", ... */
    u16 cfg9_before, cfg9_after;/* CFG9_SDMMCCTL around the port-power write */
    u16 opt;                    /* SD_OPTION read back (0x..EE = controller answers) */
    u32 ocr_first, ocr;         /* R4 of the first CMD5 (OCR 0) and of the ready one */
    u32 cmd5_tries;
    u32 rca;
    u8  cccr_rev, sd_rev, caps, bic;   /* CCCR 0x00, 0x01, 0x08, 0x07 (after the write) */
    u32 ready_tries;            /* polls of CCCR 0x03 until function 1 was ready */
} sdio_diag_t;
const sdio_diag_t *sdio_diag(void);
/* CISTPL_MANFID from the common CIS (CMD52 reads): 0 = found, < 0 = not. */
int sdio_manfid(u16 *manf, u16 *card);
