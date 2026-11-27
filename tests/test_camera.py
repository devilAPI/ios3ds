"""Host test of the camera path's pure logic: the shim's CAM FIFO drain
(kernel/shim/cam.c built with -DCAM_HOST_TEST) against a model CAM block, and
the YUV conversion / test pattern (boot/common/cam_pattern.h); see
tools/ios3ds-camera/tests/test_cam.c and docs/camera.md. Also checks the
rootfs recipe's camera entries."""
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class CameraDrain(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            exe = os.path.join(td, "test_cam")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DCAM_HOST_TEST",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            os.path.join(ROOT, "kernel", "shim", "cam.c"),
                            os.path.join(ROOT, "tools", "ios3ds-camera", "tests", "test_cam.c"),
                            "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


class PreviewCounter(unittest.TestCase):
    def test_selftest(self):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ios3ds-camera", "tests", "preview_counter.py"),
                            "--selftest"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class CameraDumpFolder(unittest.TestCase):
    """The ARM9 dump path and the 8.3 names come from boot/common/cam_dump.h.
    Check the header against the writer, so a return to sd:/ios3ds/CAM*.RAW
    (or an unsafe long name) fails the test. See docs/camera.md section 4.1."""

    def test_dump_path_and_legacy_name(self):
        cam = os.path.join(ROOT, "boot", "arm9", "camera.c")
        with open(cam) as f:
            src = f.read()
        for needed in ('#include "cam_dump.h"', "cam_dump_path(path",
                       "f_mkdir(CAM_PHOTOS_DIR)", "f_rename(from, to)",
                       "cam_photos_init();"):
            self.assertIn(needed, src, needed)

        harness = r'''
#include <stdio.h>
#include <string.h>
#include "cam_dump.h"
int main(void)
{
    char p[CAM_DUMP_PATH_LEN], small[8];
    const char *bad[] = {"CAM0000123.RAW", "CAM000012345.RAW",
                         "CAM00001234.TXT", "CAM0000ABCD.RAW",
                         "CAM00001234.RAWx", "LOG00000001.RAW",
                         "photos/CAM00001234.RAW", ""};
    if (cam_dump_path(p, sizeof(p), 1234u) != 32 ||
        strcmp(p, "0:/ios3ds/photos/CAM00001234.RAW")) return 1;
    if (cam_dump_path(p, sizeof(p), 0u) != 32 ||
        strcmp(p, "0:/ios3ds/photos/CAM00000000.RAW")) return 2;
    memset(small, '#', sizeof(small));
    if (cam_dump_path(small, sizeof(small), 1u) != 0 || small[0] != '#') return 3;
    /* a sequence past 8 digits wraps instead of breaking the FAT 8.3 name */
    if (cam_dump_path(p, sizeof(p), 0xFFFFFFFFu) != 32 ||
        strcmp(p, "0:/ios3ds/photos/CAM94967295.RAW")) return 4;
    if (strlen(strrchr(p, '/') + 1) != CAM_DUMP_NAME_LEN) return 5;
    if (!cam_is_dump_name("CAM00001234.RAW") ||
        !cam_is_dump_name("cam00001234.raw")) return 6;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (cam_is_dump_name(bad[i])) return 7;
    puts(p);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            csrc = os.path.join(td, "dump.c")
            exe = os.path.join(td, "dump")
            with open(csrc, "w") as f:
                f.write(harness)
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            csrc, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "0:/ios3ds/photos/CAM94967295.RAW")


class CameraRootfs(unittest.TestCase):
    def test_recipe(self):
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            r = json.load(f)
        self.assertEqual(r["add"]["build/rootfs-add/System/Library/MediaCapture/vcd.mediacapture"],
                         "/System/Library/MediaCapture/vcd.mediacapture")
        caps = r["plist_merge"]["/System/Library/CoreServices/SpringBoard.app/N72AP.plist"]["capabilities"]
        self.assertTrue(caps["still-camera"] and caps["front-facing-camera"])
        with open(os.path.join(ROOT, "rootfs", "files", "camera", "com.apple.coremedia.plist"), "rb") as f:
            prefs = plistlib.load(f)
        self.assertIs(prefs["vcd"], True)
        self.assertNotIn("ios3dsPattern", prefs)        # the shipped image uses the real cameras
        with open(os.path.join(ROOT, "rootfs", "files", "camera", "AVCapture.plist"), "rb") as f:
            av = plistlib.load(f)
        for mode, port in (("AVCaptureMode_PhotoCapture", "PortTypeBack"),
                           ("AVCaptureMode_FrontFacingPhotoCapture", "PortTypeFront")):
            lso = av[mode]["LiveSourceOptions"]
            self.assertEqual(lso["VideoPort"], port)
            for k in ("Sensor", "Capture", "Preview"):
                self.assertEqual((lso[k]["Width"], lso[k]["Height"], lso[k]["PixelFormatType"]),
                                 (320, 240, "BGRA"), mode + " " + k)

    def test_plist_merge(self):
        import mkrootfs
        d = {"capabilities": {"wifi": True, "still-camera": False}, "x": 1}
        mkrootfs.merge_dict(d, {"capabilities": {"still-camera": True}})
        self.assertEqual(d, {"capabilities": {"wifi": True, "still-camera": True}, "x": 1})


if __name__ == "__main__":
    unittest.main()
