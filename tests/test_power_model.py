"""Host test of the integer power estimate used by [pwr] telemetry.

No floats and no hardware: this pins the coefficients in power_model.h so a
future edit cannot silently change the hours reported in docs/mcu.md.
"""
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST = r'''
#include <stdio.h>
#include "power_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static pwr_state_t state(void)
{
    pwr_state_t s = { 0 };
    s.backlight = 500;
    s.arm11_mhz = 268;
    s.arm9_loops_s = 1800;
    s.arm11_irqs_s = 127;
    return s;
}

int main(void)
{
    pwr_state_t s = state();
    CHECK(pwr_estimate_ma(&s) == 68);           /* screen off, no radio */
    CHECK(pwr_hours_x10(pwr_estimate_ma(&s), PWR_CAP_OLD_MAH) == 191);

    s = state(); s.screen_on = 1; s.gpu_frames_s = 30;
    CHECK(pwr_estimate_ma(&s) == 183);          /* 30 fps mirror + two LCDs */

    s = state(); s.wifi_link = 1;
    CHECK(pwr_estimate_ma(&s) == 138);          /* max-performance Wi-Fi */
    s.wifi_rec = 1;
    CHECK(pwr_estimate_ma(&s) == 98);           /* WMI REC_POWER */

    s = state(); s.backlight = 0; s.arm9_loops_s = 530; s.arm11_irqs_s = 146;
    CHECK(pwr_estimate_ma(&s) == 66);           /* screen off, powersave=1 */
    s.wifi_link = 1; s.wifi_rec = 1;
    CHECK(pwr_estimate_ma(&s) == 96);
    s.wifi_link = 0; s.wifi_radio = 1;
    CHECK(pwr_estimate_ma(&s) == 81);
    s.camera_on = 1; s.csnd_on = 1;
    CHECK(pwr_estimate_ma(&s) == 116);          /* nothing gated */

    s = state(); s.backlight = 0; s.arm11_mhz = 804; s.wifi_link = 1; s.wifi_rec = 1;
    CHECK(pwr_estimate_ma(&s) == 125);          /* New 3DS, screen off, old telemetry */
    s.arm11_mhz = 536;
    CHECK(pwr_estimate_ma(&s) == 112);          /* idle clock saves 13 mA in the model */

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class PowerModel(unittest.TestCase):
    def test_estimate(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "test.c")
            with open(src, "w") as f:
                f.write(textwrap.dedent(TEST))
            exe = os.path.join(td, "test_power")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-I" +
                            os.path.join(ROOT, "boot", "common"), src, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
