/*
 * Host test of the ARM9 stereoscopic screen copy (boot/arm9/display.c):
 *   - stereo3d=0 produces exactly the normal frame (both eyes identical);
 *   - a stamped home frame gives the two eyes the class-dependent offset,
 *     strips the alpha stamp, and leaves the bottom/touch copy unchanged;
 *   - slider down and unstamped frames are flat with the barrier off.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "display.h"
#include "stereo.h"

#define VRAM_PA   0x18000000u
#define CLCD_PA   0x1FFB0000u
#define LCD_PA    0x10202000u
#define TOP0      0x18300000u
#define TOP1      0x18400000u
#define BOT0      0x18346500u
#define BOT1      0x18446500u
#define W         320u
#define H         480u

static uint8_t *vram, *clcd, *lcd;
static int fails, checks, stereo_reqs;

bool blksrv_poll(void) { return false; }
bool gm_set_stereo(bool on) { stereo_reqs++; return true; }
void gm_set_shift(u32 k) { (void)k; }
void mcu_set_3d_led(bool on) { (void)on; }
void log_color(unsigned color, const char *fmt, ...) { (void)color; (void)fmt; }

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A simple but non-trivial source: x-major gradient plus a distinct alpha
   class in three horizontal bands (icon / background / front). */
static void picture(uint8_t cls)
{
    uint32_t *p = (uint32_t *)vram;
    for (uint32_t y = 0; y < H; y++) {
        uint32_t c = y < 80 ? SA_FRONT : (y < 400 ? SA_ICONS : SA_BG);
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = (x * 3 + y * 7) & 0xFFFFFF;
            p[y * W + x] = v | (uint32_t)(cls ? c : 0) << 24;
        }
    }
}

static void setup_clcd(void)
{
    clcd[0] = 1;
    *(volatile uint32_t *)(clcd + 0x24) = VRAM_PA;
    *(volatile uint32_t *)(clcd + 0x28) = W;
    *(volatile uint32_t *)(clcd + 0x30) = W << 16 | H;
    *(volatile uint32_t *)(clcd + 0x08) ^= 1;
}

static uint32_t pixel(uint32_t fb, uint32_t col, uint32_t row)
{
    uint8_t *p = vram + fb - VRAM_PA + (col * 240 + row) * 3;
    return p[0] | p[1] << 8 | p[2] << 16;
}

/* the monitor loop samples the stamp before every copy (display.c no longer
   decides inside display_blit) */
static void blit(void)
{
    display_stereo_probe(CLCD_PA);
    display_blit(CLCD_PA);
}

int main(void)
{
    uint32_t fbs[4], before[240], i, diff;
    vram = mmap((void *)(uintptr_t)VRAM_PA, 0x600000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    clcd = mmap((void *)(uintptr_t)CLCD_PA, 0x1000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    lcd = mmap((void *)(uintptr_t)LCD_PA, 0x1000, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (vram != (void *)(uintptr_t)VRAM_PA || clcd != (void *)(uintptr_t)CLCD_PA ||
        lcd != (void *)(uintptr_t)LCD_PA) {
        fprintf(stderr, "test_stereo_copy: cannot map fixed addresses\n");
        return 2;
    }
    display_init((uint8_t *)(uintptr_t)TOP0, (uint8_t *)(uintptr_t)TOP1,
                 (uint8_t *)(uintptr_t)BOT0, (uint8_t *)(uintptr_t)BOT1);
    display_arm(CLCD_PA);
    setup_clcd();
    display_fbs(fbs);
    CHECK(fbs[0] == TOP0 && fbs[1] == TOP1, "display_fbs");

    /* 1. flag off: the old byte-for-byte path, barrier untouched */
    picture(1);
    display_set_stereo(false, 255);
    blit();
    for (i = 0; i < 240; i++) before[i] = pixel(TOP0, 100, i);
    CHECK(*(volatile uint32_t *)lcd == 0, "flag off: barrier off");
    CHECK(stereo_reqs == 0, "flag off: no PDC request");

    /* 2. slider up + class map: both eyes written, offset per class, opaque */
    picture(1);
    display_set_stereo(true, 255);
    blit();
    /* hysteresis: one home probe is not enough (a transition can show one) */
    CHECK(*(volatile uint32_t *)lcd == 0, "home + slider: first probe still 2D");
    CHECK(stereo_reqs == 0, "home + slider: no PDC request after one probe");
    blit();
    CHECK(*(volatile uint32_t *)lcd == ST_BARRIER_ON, "home + slider: barrier on");
    CHECK(stereo_reqs == 1, "home + slider: PDC switched once");
    diff = 0;
    for (i = 0; i < 240; i++)
        diff += pixel(TOP0, 50, i) != pixel(TOP1, 50, i);
    CHECK(diff > 0, "home + slider: two eyes differ in %u of 240 rows", diff);
    /* class 2 (icon, screen column 100) is at the convergence plane */
    diff = 0;
    for (i = 0; i < 240; i++)
        diff += pixel(TOP0, 100, i) != pixel(TOP1, 100, i);
    CHECK(diff == 0, "icon plane: both eyes equal (%u rows differ)", diff);
    /* class 3 (front, screen column 50): k=4, left samples x-4, right x+4 */
    for (i = 0; i < 240; i++) {
        uint32_t iy = (50 - 20) * 4 / 3, x = i * 4 / 3;
        int xl = (int)x - 4, xr = (int)x + 4;
        uint32_t vl, vr;
        if (xl < 0) xl = 0; if (xl >= (int)W) xl = W - 1;
        if (xr < 0) xr = 0; if (xr >= (int)W) xr = W - 1;
        vl = ((uint32_t)xl * 3 + iy * 7) & 0xFFFFFF;
        vr = ((uint32_t)xr * 3 + iy * 7) & 0xFFFFFF;
        CHECK(pixel(TOP0, 50, i) == vl, "front plane left row %u: %06x != %06x",
              i, pixel(TOP0, 50, i), vl);
        CHECK(pixel(TOP1, 50, i) == vr, "front plane right row %u: %06x != %06x",
              i, pixel(TOP1, 50, i), vr);
    }
    /* 2b. a short flat stretch (a transition) keeps 3D for up to 8 probes,
       the ninth goes back to 2D */
    picture(0);
    for (i = 0; i < 8; i++)
        blit();
    CHECK(*(volatile uint32_t *)lcd == ST_BARRIER_ON, "transition: 3D held through 8 flat probes");
    CHECK(stereo_reqs == 1, "transition: no PDC switch while held");
    blit();
    CHECK(*(volatile uint32_t *)lcd == 0, "transition: 2D after the hold");
    picture(1);
    blit();
    blit();
    CHECK(*(volatile uint32_t *)lcd == ST_BARRIER_ON, "home again: 3D");

    /* 3. slider down: flat, barrier off, and the normal copy restored */
    display_set_stereo(true, 0);
    blit();
    CHECK(*(volatile uint32_t *)lcd == 0, "slider down: barrier off");
    diff = 0;
    for (i = 0; i < 240; i++)
        diff += pixel(TOP0, 100, i) != before[i];
    CHECK(diff == 0, "slider down: byte-for-byte normal copy (%u rows differ)", diff);

    /* 4. unstamped frame (apps): flat even with the slider up */
    picture(0);
    display_set_stereo(true, 255);
    blit();
    CHECK(*(volatile uint32_t *)lcd == 0, "app frame: barrier off");
    diff = 0;
    for (i = 0; i < 240; i++)
        diff += pixel(TOP0, 100, i) != pixel(TOP1, 100, i);
    CHECK(diff == 0, "app frame: both eyes identical (%u rows differ)", diff);

    /* 4b. boot.cfg stereo_depth=0: flat with the slider up */
    picture(1);
    display_set_stereo_depth(0);
    display_set_stereo(true, 255);
    blit();
    blit();
    blit();
    CHECK(*(volatile uint32_t *)lcd == 0, "stereo_depth=0: barrier off");
    display_set_stereo_depth(100);

    /* 5. Dirty-row cache: a full two-eye frame is built in a few bounded
       blits; afterwards a change in one source row rebuilds only the screen
       column fed by that row. Other columns keep the image they already had
       until their own row changes. */
    picture(1);
    display_set_stereo(false, 255);
    blit();                  /* flat copy, cache invalidated */
    display_set_stereo(true, 255);
    for (i = 0; i < 4; i++)
        blit();              /* 360 columns / 120 per blit */
    {
        uint32_t keep = pixel(TOP1, 250, 100);
        uint32_t was = pixel(TOP0, 170, 2);
        uint32_t *src = (uint32_t *)vram;
        src[200 * W + 2] ^= 0x123456u;      /* icon row 200 -> screen column 170, no eye shift */
        blit();
        CHECK(pixel(TOP0, 170, 2) != was, "dirty row: its column was rebuilt");
        CHECK(pixel(TOP1, 250, 100) == keep, "clean row: other column untouched");
        for (i = 0; i < 4; i++)
            blit();
        /* every column now matches a reference computed from the original
           class/delta rules, for both eyes (sampled grid, all depth bands) */
        for (uint32_t c = 0; c < 360; c += 7) {
            uint32_t sy = c * 4 / 3;
            for (i = 0; i < 240; i += 13) {
                uint32_t x = i * 4 / 3;
                uint32_t px = ((uint32_t *)vram)[sy * W + x];
                int cls = stereo_class_of((u8)(px >> 24));
                for (uint32_t eye = 0; eye < 2; eye++) {
                    int d = stereo_eye_delta(cls, 4, eye ? 1 : 0);
                    int s = (int)x - d;
                    uint32_t want;
                    if (s < 0) s = 0;
                    if (s >= (int)W) s = (int)W - 1;
                    want = ((uint32_t)s * 3 + sy * 7) & 0xFFFFFF;
                    CHECK(pixel(eye ? TOP1 : TOP0, 20 + c, i) == want,
                          "converged eye %u col %u row %u: %06x != %06x",
                          eye, 20 + c, i, pixel(eye ? TOP1 : TOP0, 20 + c, i), want);
                }
            }
        }
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
