"""Host tests of the ARM9 boot.cfg parser (boot/arm9/bootcfg.c).

No 3DS hardware or Apple files: the parser is compiled for the host with
stubs for the two freestanding helpers and for log_warn, so the tests exercise
the real key table, including the boot_delay/countdown_ms alias from issue #1.
"""
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STUBS = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bootcfg.h"

int warnings;
void log_color(u32 color, const char *fmt, ...)
{
    va_list ap;
    (void)color;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    warnings++;
}

void strlcpy_(char *dst, const char *src, size_t size)
{
    if (!size) return;
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

u32 parse_u32(const char *s, const char **end)
{
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (end) *end = e;
    return (u32)v;
}
'''

TEST = r'''
#include <stdio.h>
#include <string.h>
#include "bootcfg.h"

static int fails;
extern int warnings;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void parse(const char *text)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", text);
    parse_bootcfg(buf);
}

int main(void)
{
    parse("");
    CHECK(cfg.countdown_ms == 3000);            /* no boot.cfg: safe bring-up default */
    CHECK(cfg.jump == 1);

    parse("boot_delay=0\n");
    CHECK(cfg.countdown_ms == 0);               /* issue #1: skip by default */

    parse("boot_delay=250\nboot_delay=0\n");
    CHECK(cfg.countdown_ms == 0);

    parse("boot_delay=750\n");
    CHECK(cfg.countdown_ms == 750);
    CHECK(cfg.jump == 1);                       /* other defaults untouched */

    parse("# boot_delay=0\ncountdown_ms=1250\n");
    CHECK(cfg.countdown_ms == 1250);            /* old key still works; comments ignored */

    parse("boot_delay=0\ncountdown_ms=900\n");
    CHECK(cfg.countdown_ms == 900);             /* last line wins if both are given */

    parse("boot_delay=0\nno_such_key=1\n");
    CHECK(cfg.countdown_ms == 0);
    CHECK(warnings >= 1);                       /* unknown keys are still reported */

    parse("boot_delay=0\ngps=52.5200,13.4050\ncmdline=debug=0x8\n");
    CHECK(cfg.countdown_ms == 0);
    CHECK(!strcmp(cfg.gps, "52.5200,13.4050"));
    CHECK(!strcmp(cfg.cmdline, "debug=0x8"));

    parse("boot_delay=0\n");
    CHECK(cfg.hwtest == 0);                     /* hardware test off by default */
    parse("hwtest=1\n");
    CHECK(cfg.hwtest == 1);
    parse("hwtest=2\n");
    CHECK(cfg.hwtest == 2);

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class BootCfg(unittest.TestCase):
    def test_boot_delay_key(self):
        with tempfile.TemporaryDirectory() as td:
            for name, body in (("stubs.c", STUBS), ("test.c", TEST)):
                with open(os.path.join(td, name), "w") as f:
                    f.write(textwrap.dedent(body))
            exe = os.path.join(td, "test_bootcfg")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            "-I" + os.path.join(ROOT, "boot", "arm9"),
                            os.path.join(ROOT, "boot", "arm9", "bootcfg.c"),
                            os.path.join(td, "stubs.c"), os.path.join(td, "test.c"),
                            "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
