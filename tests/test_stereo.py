"""Host tests of the stereoscopic 3D depth/offset logic (boot/common/stereo.h,
docs/stereo3d.md): slider gating, shift curve, alpha<->class mapping, eye
deltas, the per-frame layer classifier and the frame decision."""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

C_SRC = r'''
#include <stdio.h>
#include <string.h>
#include "stereo.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    /* slider gating */
    CHECK(!stereo_active(0, 255), "flag off => inactive");
    CHECK(!stereo_active(1, 0), "slider 0 => inactive");
    CHECK(!stereo_active(1, 15), "slider below threshold => inactive");
    CHECK(stereo_active(1, 16), "slider at threshold => active");
    CHECK(stereo_active(1, 255), "slider max => active");

    /* shift curve: 0 below threshold, 1..4 above */
    CHECK(stereo_shift_k(0) == 0, "shift 0 when off");
    CHECK(stereo_shift_k(15) == 0, "shift 0 below threshold");
    CHECK(stereo_shift_k(16) == 1, "shift 1 at threshold");
    CHECK(stereo_shift_k(255) == STEREO_MAX_SHIFT, "shift max at slider max");

    /* alpha <-> class */
    CHECK(stereo_class_of(SA_FLAT) == STC_FLAT, "flat class");
    CHECK(stereo_class_of(SA_BG) == STC_BG, "bg class");
    CHECK(stereo_class_of(SA_ICONS) == STC_ICONS, "icons class");
    CHECK(stereo_class_of(SA_FRONT) == STC_FRONT, "front class");
    CHECK(stereo_class_of(SA_ALERT) == STC_ALERT, "alert class");
    CHECK(stereo_class_of(0x55) == -1, "unknown alpha => -1");
    CHECK(stereo_magic(STC_BG) == SA_BG, "magic bg");
    CHECK(stereo_magic(STC_ALERT) == SA_ALERT, "magic alert");

    /* eye deltas: class 2 (icons) stays put; bg (1) moves opposite to alert (4) */
    int k = 4;
    CHECK(stereo_eye_delta(STC_ICONS, k, 0) == 0, "icons delta 0 left");
    CHECK(stereo_eye_delta(STC_ICONS, k, 1) == 0, "icons delta 0 right");
    CHECK(stereo_eye_delta(STC_BG, k, 0) == -k, "bg left = -k");
    CHECK(stereo_eye_delta(STC_BG, k, 1) == +k, "bg right = +k");
    CHECK(stereo_eye_delta(STC_ALERT, k, 0) == 2 * k, "alert left = +2k");
    CHECK(stereo_eye_delta(STC_ALERT, k, 1) == -2 * k, "alert right = -2k");
    CHECK(stereo_eye_delta(STC_FLAT, k, 0) == 0, "flat delta 0");
    CHECK(stereo_eye_delta(STC_BG, 0, 0) == 0, "k=0 => no delta");

    /* boot.cfg stereo_depth: 100 = the plain curve, 0 = flat, >= 1 px otherwise */
    for (unsigned sl = 0; sl < 256; sl++) {
        CHECK(stereo_depth_k(sl, 100) == stereo_shift_k(sl), "depth 100 = slider curve");
        CHECK(stereo_depth_k(sl, 250) == stereo_shift_k(sl), "depth > 100 clamps");
        CHECK(stereo_depth_k(sl, 0) == 0, "depth 0 = flat");
        CHECK(stereo_depth_k(sl, 50) <= stereo_shift_k(sl), "depth 50 <= full");
        CHECK((stereo_depth_k(sl, 1) > 0) == (stereo_shift_k(sl) > 0), "depth 1 still 3D when slider up");
    }
    CHECK(stereo_depth_k(255, 50) == STEREO_MAX_SHIFT / 2, "depth 50 halves the max shift");
    CHECK(stereo_depth_k(255, 25) == 1, "depth 25 at max = 1 px");

    /* classifier: a home screen has bg + >=4 icon draws */
    stereo_frame_t f;
    stereo_frame_begin(&f);
    int vp[4] = { 0, 0, 320, 480 };
    /* full-screen opaque background */
    int cls = stereo_draw(&f, 0, 0, 319, 479, vp, 1, 1);
    CHECK(cls == STC_BG, "first full opaque => bg");
    /* a few icon-sized draws */
    for (int i = 0; i < 6; i++)
        stereo_draw(&f, 10 + i * 40, 100, 40 + i * 40, 130, vp, 0, 1);
    CHECK(stereo_frame_home(&f), "home screen detected");
    /* status bar band => front */
    cls = stereo_draw(&f, 0, 0, 319, 20, vp, 0, 1);
    CHECK(cls == STC_FRONT, "status bar band => front");

    /* SpringBoard's wallpaper is a full-screen *translucent* draw: it still
       establishes the background, so the icon grid after it is stamped */
    stereo_frame_t f4;
    stereo_frame_begin(&f4);
    cls = stereo_draw(&f4, 0, 0, 319, 479, vp, 1, 0);
    CHECK(cls == STC_BG, "translucent wallpaper still sets the background");
    for (int i = 0; i < 6; i++)
        stereo_draw(&f4, 10 + i * 40, 100, 40 + i * 40, 130, vp, 0, 1);
    CHECK(stereo_frame_home(&f4), "translucent background + icons => home");

    /* SpringBoard can also draw the wallpaper as small layers: the first
       four small draws establish the background, the rest get stamped */
    stereo_frame_t f5;
    stereo_frame_begin(&f5);
    for (int i = 0; i < 10; i++)
        cls = stereo_draw(&f5, 10 + i * 30, 100, 35 + i * 30, 130, vp, 0, 1);
    CHECK(cls == STC_ICONS, "small draws after the background are icons");
    CHECK(stereo_frame_home(&f5), "small-draw wallpaper + icons => home");
    stereo_frame_t f6;                  /* an app with a few small draws stays flat */
    stereo_frame_begin(&f6);
    for (int i = 0; i < 3; i++)
        stereo_draw(&f6, 10 + i * 30, 100, 35 + i * 30, 130, vp, 0, 1);
    CHECK(!stereo_frame_home(&f6), "three small draws are not a home frame");

    /* an opaque full-screen draw after bg flips to flat */
    stereo_frame_t f2;
    stereo_frame_begin(&f2);
    stereo_draw(&f2, 0, 0, 319, 479, vp, 1, 1);
    cls = stereo_draw(&f2, 0, 0, 319, 479, vp, 1, 1);
    CHECK(cls == -1, "second opaque full => not stamped");
    CHECK(!stereo_frame_home(&f2), "second full opaque => not home");

    /* backdrop (translucent full screen) keeps classes, then alert */
    stereo_frame_t f3;
    stereo_frame_begin(&f3);
    stereo_draw(&f3, 0, 0, 319, 479, vp, 1, 1);
    for (int i = 0; i < 6; i++)
        stereo_draw(&f3, 10 + i * 40, 100, 40 + i * 40, 130, vp, 0, 1);
    int bcls = stereo_draw(&f3, 0, 0, 319, 479, vp, 1, 0);
    CHECK(bcls == -1, "backdrop not stamped");
    cls = stereo_draw(&f3, 50, 50, 100, 100, vp, 0, 1);
    CHECK(cls == STC_ALERT, "after backdrop => alert");

    /* alphas_stereo: all class magics with >=2 non-flat distinct */
    uint8_t alphas[6] = { SA_BG, SA_ICONS, SA_ICONS, SA_FRONT, SA_ALERT, SA_ICONS };
    CHECK(stereo_alphas_stereo(alphas, 6), "stamped frame => stereo");
    uint8_t flat[4] = { SA_FLAT, SA_FLAT, SA_FLAT, SA_FLAT };
    CHECK(!stereo_alphas_stereo(flat, 4), "all flat => not stereo");
    uint8_t unknown[4] = { SA_BG, SA_ICONS, 0x55, SA_FRONT };
    CHECK(!stereo_alphas_stereo(unknown, 4), "unknown alpha => not stereo");
    uint8_t oneclass[4] = { SA_ICONS, SA_ICONS, SA_ICONS, SA_ICONS };
    CHECK(!stereo_alphas_stereo(oneclass, 4), "single non-flat class => not stereo");

    if (fails) { printf("TOTAL FAILS: %d\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
'''

class StereoHost(unittest.TestCase):
    @unittest.skipUnless(__import__("shutil").which("cc"), "no cc")
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "t.c")
            with open(src, "w") as f:
                f.write(C_SRC)
            exe = os.path.join(td, "t")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I", os.path.join(ROOT, "boot", "common"),
                            src, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)

if __name__ == "__main__":
    unittest.main()
