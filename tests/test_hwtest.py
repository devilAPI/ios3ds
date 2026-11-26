"""Static checks of the one-boot hardware test kit (tools/ios3ds-diag/hwtest.c).

No Apple files: checks that boot.cfg hwtest= reaches the boot-args, that the
launchd job is gated on that boot-arg (so a hwtest=0 boot runs nothing), that
every version's recipe installs the tool and the job, and that the tool prints
the summary block docs/real-hardware.md tells people to look for.
"""
import json
import os
import plistlib
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLIST = "/System/Library/LaunchDaemons/com.ios3ds.hwtest.plist"


def read(*p):
    with open(os.path.join(ROOT, *p)) as f:
        return f.read()


class HwTest(unittest.TestCase):
    def test_job_is_gated(self):
        with open(os.path.join(ROOT, "tools/ios3ds-diag/com.ios3ds.hwtest.plist"), "rb") as f:
            pl = plistlib.load(f)
        self.assertEqual(pl["ProgramArguments"][:4],
                         ["/usr/libexec/ios3ds-diag", "-g", "ios3ds_hwtest", "/usr/libexec/ios3ds-hwtest"])
        self.assertFalse(pl["KeepAlive"])

    def test_arm9_boot_arg(self):
        self.assertIn('{ "hwtest", &cfg.hwtest }', read("boot/arm9/bootcfg.c"))
        self.assertIn("ios3ds_hwtest=%u", read("boot/arm9/main.c"))

    def test_every_version_installs_it(self):
        for name, exe in (("recipe.json", "build/rootfs-add/usr/libexec/ios3ds-hwtest"),
                          ("overlay-3.1.3.json", "build/rootfs-add/usr/libexec/ios3ds-hwtest"),
                          ("overlay-2.2.1.json", "build/rootfs-add/ios2/ios3ds-hwtest")):
            r = json.loads(read("rootfs", name))
            self.assertEqual(r["add"].get(exe), "/usr/libexec/ios3ds-hwtest", name)
            self.assertIn(PLIST, r["add"].values(), name)
            self.assertEqual(r["chmod"].get("/usr/libexec/ios3ds-hwtest"), "0755", name)
        # 4.1 inherits the 4.2.1 add list
        self.assertNotIn("add", json.loads(read("rootfs", "overlay-4.1.json")))

    def test_summary_and_versions(self):
        src = read("tools/ios3ds-diag/hwtest.c")
        for s in ("==== SUMMARY", "==== END", "8C148", "8B117", "7E18", "5H11",
                  '"PASS", "FAIL", "SKIP"'):
            self.assertIn(s, src)
        for check in ("speaker", "headphone", "mic", "wifi.join", "wifi.https", "accel", "gyro",
                      "touch", "buttons", "brightness", "battery", "cameras", "gps", "gpu",
                      "stereo3d", "lock/wake"):
            self.assertIn('"%s"' % check, src)

    def test_cameras_pass_on_frames_every_version(self):
        """2.x has no Camera.app, but a delivered frame is still a driver PASS.

        The 4.x tool must keep its old code path and byte-identical binary:
        the 2.x-only branch is compiled in with -DIOS2_HWTEST in the HW2
        recipe alone (re-log 153).
        """
        src = read("tools/ios3ds-diag/hwtest.c")
        ios2 = src.split("#ifdef IOS2_HWTEST")[1].split("#else")[0]
        self.assertIn("if (ok == (int)CAM_COUNT)", ios2)
        self.assertIn('result("cameras", PASS, "%s", what);', ios2)
        self.assertIn("#ifdef IOS2_HWTEST", src)
        mk = read("tools", "ios3ds-diag", "Makefile")
        hw4 = mk.split("$(HW): hwtest.c")[1].split("$(HW2):")[0]
        self.assertNotIn("-DIOS2_HWTEST", hw4)
        hw2 = mk.split("$(HW2): hwtest.c $(GLUE)")[1].split("\n\n")[0]
        self.assertIn("-DIOS2_HWTEST", hw2)

    def test_harness_uses_hardware_timing_for_camera_checks(self):
        """Wall-clock QEMU starves the 1 ms camera drain on 2.x (re-log 153)."""
        import sys
        sys.path.insert(0, os.path.join(ROOT, "emu"))
        import hwtest_check
        self.assertEqual(hwtest_check.icount_arg(2, None), "2")
        self.assertEqual(hwtest_check.icount_arg(1, None), "2")
        self.assertEqual(hwtest_check.icount_arg(0, None), "")  # control: no camera check
        self.assertEqual(hwtest_check.icount_arg(2, ""), "")    # explicit opt-out
        self.assertEqual(hwtest_check.icount_arg(2, "3"), "3")


if __name__ == "__main__":
    unittest.main()
