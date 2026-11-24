/*
 * Undo the boot.firm swap made by the ios3ds launcher (.3dsx).
 *
 * The launcher (tools/ios3ds-launcher) does, on the SD card only:
 *   1. create  ios3ds/swap.flg
 *   2. rename  boot.firm          -> ios3ds/luma.firm     (Luma3DS)
 *   3. rename  ios3ds/ios3ds.firm -> boot.firm            (this payload)
 *   4. reboot  -> boot9strap boots us directly
 * and the very first thing we do after mounting the SD card is the reverse, so
 * the next power-on is Luma again. Every interrupted state still boots:
 * between the two renames there is no sd:/boot.firm and boot9strap falls back
 * to ctrnand:/boot.firm (installed by the 3ds.hacks.guide); after the swap,
 * this payload boots and restores. Nothing here touches NAND.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "swap.h"
#include "log.h"
#include "fatfs/ff.h"

#define FLAG  "0:/ios3ds/swap.flg"
#define LUMA  "0:/ios3ds/luma.firm"
#define SELF  "0:/ios3ds/ios3ds.firm"
#define BOOT  "0:/boot.firm"

static bool exists(const char *p)
{
    FILINFO fi;
    return f_stat(p, &fi) == FR_OK;
}

void firm_swap_restore(void)
{
    if (!exists(FLAG)) return;
    logp("launcher swap: restoring Luma as sd:/boot.firm\n");
    if (!exists(LUMA)) {
        /* already restored (or never swapped): just drop the flag */
        log_warn("launcher swap: no %s, nothing to restore\n", LUMA);
        f_unlink(FLAG);
        return;
    }
    FRESULT r = FR_OK;
    if (exists(BOOT)) {
        if (exists(SELF)) r = f_unlink(BOOT);   /* boot.firm is a copy of us */
        else r = f_rename(BOOT, SELF);
    }
    if (r == FR_OK) r = f_rename(LUMA, BOOT);
    if (r != FR_OK) {
        log_err("launcher swap: restore failed (FRESULT %d); fix by hand: "
                "ios3ds/luma.firm -> boot.firm\n", r);
        return;                                 /* keep the flag: retry next boot */
    }
    f_unlink(FLAG);
    log_ok("launcher swap: restored, next boot is Luma\n");
}

/*
 * Issue #9: a Luma-chainloaded payload (sd:/luma/payloads/ios3ds.firm) that is
 * older than the sd:/ios3ds/ folder. Updating means copying the new ios3ds/
 * folder; the chainloader copy is easy to miss. The launcher (.3dsx) boots
 * ios3ds/ios3ds.firm, so that path worked while the chainloader kept booting
 * the old payload against the new shim.bin/gpu.bin. The two disagree on the
 * shared layout (e.g. the SELECT+START word moved from shim+0x6C to
 * shim+0x6500): garbled screen, dead buttons, spurious panics. ios3ds.firm in
 * the ios3ds/ folder is from the same zip as shim.bin, so it is the reference:
 * if it does not contain our own version string, we are a different build.
 */
#define PAYLOAD "0:/luma/payloads/ios3ds.firm"

static u8 scanbuf[4096];

/* 1: SELF contains IOS3DS_VERSION, 0: it does not, -1: no SELF / read error */
static int self_has_our_version(void)
{
    const char *v = IOS3DS_VERSION;
    UINT vl = 0;
    while (v[vl]) vl++;
    if (vl < 8 || vl > 128) return 1;         /* "dev" build: no usable id */
    FIL f;
    if (f_open(&f, SELF, FA_READ) != FR_OK) return -1;
    UINT keep = 0, got = 0;
    int found = 0;
    for (;;) {
        if (f_read(&f, scanbuf + keep, sizeof scanbuf - keep, &got) != FR_OK) { found = -1; break; }
        UINT n = keep + got;
        for (UINT i = 0; i + vl <= n && !found; i++) {
            UINT k = 0;
            while (k < vl && scanbuf[i + k] == (u8)v[k]) k++;
            if (k == vl) found = 1;
        }
        if (found || got == 0) break;
        keep = n < vl - 1 ? n : vl - 1;     /* overlap: a match across chunks */
        for (UINT i = 0; i < keep; i++) scanbuf[i] = scanbuf[n - keep + i];
    }
    f_close(&f);
    return found;
}

static FRESULT copy_file(const char *from, const char *to)
{
    FIL in, out;
    FRESULT r = f_open(&in, from, FA_READ);
    if (r != FR_OK) return r;
    r = f_open(&out, to, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) { f_close(&in); return r; }
    UINT got, put;
    for (;;) {
        r = f_read(&in, scanbuf, sizeof scanbuf, &got);
        if (r != FR_OK || got == 0) break;
        r = f_write(&out, scanbuf, got, &put);
        if (r == FR_OK && put != got) r = FR_DISK_ERR;
        if (r != FR_OK) break;
    }
    f_close(&in);
    FRESULT rc = f_close(&out);
    return r != FR_OK ? r : rc;
}

bool firm_is_stale(void)
{
    int s = self_has_our_version();
    if (s != 0) {
        if (s < 0) log_warn("payload check: cannot read %s, skipped\n", SELF);
        return false;
    }
    log_err("payload check: this payload (%s) is not the build in sd:/ios3ds/ios3ds.firm: "
            "the chainloaded copy is outdated and does not match shim.bin/gpu.bin\n", IOS3DS_VERSION);
    if (exists(PAYLOAD)) {
        FRESULT r = copy_file(SELF, PAYLOAD);
        if (r == FR_OK)
            log_ok("payload check: copied ios3ds/ios3ds.firm over luma/payloads/ios3ds.firm - power off and boot again\n");
        else
            log_err("payload check: updating luma/payloads/ios3ds.firm failed (FRESULT %d): copy "
                    "ios3ds/ios3ds.firm there by hand\n", r);
    } else {
        log_err("payload check: replace the .firm you chainloaded with ios3ds/ios3ds.firm\n");
    }
    return true;
}
