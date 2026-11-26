#!/usr/bin/env python3
"""Unit test for emu/safari_test.py's /next handshake with ios3ds-diag -urlpoll.

No QEMU or Apple files: the handler is called directly with a fake HTTP reply.
"""
import io
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "emu"))
import safari_test  # noqa: E402


class FakeHandler(safari_test.Handler):
    def __init__(self):
        self.headers = []
        self.wfile = io.BytesIO()

    def send_response(self, code, message=None):
        self.code = code

    def send_header(self, key, value):
        self.headers.append((key, value))

    def end_headers(self):
        pass


class FakeResult:
    stdout = ""


def next_body(next_url="", nolink=False):
    old_next, old_nolink = safari_test.state["next"], safari_test.state.get("nolink")
    safari_test.state["next"], safari_test.state["nolink"] = next_url, nolink
    try:
        h = FakeHandler()
        h.path = "/next"
        h.do_GET()
        return h.wfile.getvalue().decode(), h.code
    finally:
        safari_test.state["next"], safari_test.state["nolink"] = old_next, old_nolink


class SafariHarnessTest(unittest.TestCase):
    def test_no_early_page(self):
        body, code = next_body()
        self.assertEqual(code, 200)
        self.assertNotEqual(body, "")
        self.assertFalse(body.startswith("http"),
                         "an empty or URL body before the harness sets p1 makes "
                         "urlpoll open Safari before Wi-Fi is up")

    def test_real_url(self):
        url = "http://10.0.2.2:19423/p1.html"
        self.assertEqual(next_body(url)[0], url)

    def test_nolink_keeps_fallback(self):
        self.assertEqual(next_body("", nolink=True)[0], "")

    def test_boot_arg_only_on_linked_runs(self):
        linked = safari_test.boot_extra(False, 19423)
        self.assertIn("ios3ds_urlwait=1", linked)
        self.assertIn("ios3ds_urlport=19423", linked)
        offline = safari_test.boot_extra(True, 19423)
        self.assertNotIn("ios3ds_urlwait=1", offline)
        self.assertIn("ios3ds_urltest=1", offline)

    def test_log_reads_selected_image(self):
        old_sd = safari_test.SD
        safari_test.SD = "build/sd313.img"
        try:
            calls = []
            def fake_sess(*args, **kw):
                calls.append(args)
                return FakeResult()
            orig_sess = safari_test.sess
            safari_test.sess = fake_sess
            try:
                safari_test.log_text()
            finally:
                safari_test.sess = orig_sess
            self.assertEqual(calls, [("log", "--sd", "build/sd313.img")])
        finally:
            safari_test.SD = old_sd


if __name__ == "__main__":
    unittest.main()
