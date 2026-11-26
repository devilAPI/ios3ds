"""Host tests of scripts/music.py: the ALAC writer (decoded back with a minimal
escape-frame decoder), the .m4a box layout and the iTunes-style library."""
import os
import sqlite3
import struct
import sys
import tempfile
import unittest
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import music  # noqa: E402


class BitReader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def get(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v


def decode_escape_frame(frame, channels):
    """ALACDecoder::Decode for escape frames only."""
    r = BitReader(frame)
    tag = r.get(3)
    assert tag == (1 if channels == 2 else 0), tag
    assert r.get(4) == 0 and r.get(12) == 0
    hdr = r.get(4)
    assert hdr & 1, "not an escape frame"
    n = r.get(32) if hdr >> 3 else music.FRAME
    out = []
    for _ in range(n * channels):
        v = r.get(16)
        out.append(v - 0x10000 if v & 0x8000 else v)
    assert r.get(3) == 7, "no ID_END"
    return out


def boxes(data, off=0, end=None):
    end = len(data) if end is None else end
    while off < end:
        size, kind = struct.unpack_from(">I4s", data, off)
        yield kind, off + 8, off + size
        off += size


def find(data, path, off=0, end=None):
    head, rest = path[0], path[1:]
    for kind, b, e in boxes(data, off, end):
        if kind == head:
            if not rest:
                return b, e
            skip = {b"meta": 4, b"stsd": 8}.get(kind, 0)
            return find(data, rest, b + skip, e)
    raise KeyError(path)


class AlacWriter(unittest.TestCase):
    def test_roundtrip(self):
        samples = [((i * 37) % 65536) - 32768 for i in range(2 * (music.FRAME + 1000))]
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "t.m4a")
            ms = music.write_alac_m4a(p, samples, tags={"title": "T", "artist": "A"})
            data = open(p, "rb").read()
        self.assertAlmostEqual(ms, (music.FRAME + 1000) * 1000.0 / music.RATE)
        b, e = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsz"])
        _, _, count = struct.unpack_from(">III", data, b)
        sizes = struct.unpack_from(">%dI" % count, data, b + 12)
        b, e = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stco"])
        off = struct.unpack_from(">I", data, b + 8)[0]
        self.assertEqual(data[off - 4:off], b"mdat")
        got = []
        for s in sizes:
            got += decode_escape_frame(data[off:off + s], 2)
            off += s
        self.assertEqual(got, samples)
        b, e = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsd", b"alac"])
        cookie = data[e - 24:e]
        frame_len, _, depth, _, _, _, ch, _, _, _, rate = struct.unpack(">IBBBBBBHIII", cookie)
        self.assertEqual((frame_len, depth, ch, rate), (music.FRAME, 16, 2, music.RATE))
        b, e = find(data, [b"moov", b"udta", b"meta", b"ilst", b"\xa9nam"])
        self.assertTrue(data[b:e].endswith(b"T"))

    def test_song(self):
        s = music.render_song(seconds=1.0)
        self.assertEqual(len(s), 2 * music.RATE)
        self.assertTrue(all(-32768 <= v <= 32767 for v in s))
        self.assertGreater(max(abs(v) for v in s), 1000)


class PcmSong(unittest.TestCase):
    def test_wav(self):
        """The old-version PCM song is the same audio as the ALAC one."""
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "ios3ds-test-song.wav")
            t = music.make_test_song_wav(p)
            with wave.open(p, "rb") as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()),
                                 (2, 2, music.RATE))
                samples = struct.unpack("<%dh" % (w.getnframes() * 2), w.readframes(w.getnframes()))
        self.assertEqual(len(samples), len(music.render_song()))
        self.assertEqual(list(samples), music.render_song())
        self.assertGreater(max(abs(v) for v in samples), 10000)
        self.assertAlmostEqual(t["ms"], len(samples) * 500.0 / music.RATE)


class Library(unittest.TestCase):
    def test_build(self):
        with tempfile.TemporaryDirectory() as td:
            tracks = []
            for i, (name, artist) in enumerate((("b.mp3", "Zed"), ("a.m4a", None))):
                p = os.path.join(td, name)
                open(p, "wb").write(b"x" * (100 + i))
                tracks.append({"path": p, "title": "Song %d" % i, "artist": artist, "album": None,
                               "genre": "Rock" if i else None, "ms": 1234.0, "rate": 44100, "bitrate": 128})
            out = os.path.join(td, "Media")
            files = music.build_library(tracks, out)
            self.assertEqual(len(files), 2)
            for src, dst in files:
                self.assertEqual(open(os.path.join(out, dst), "rb").read(), open(src, "rb").read())
                self.assertTrue(dst.startswith("iTunes_Control/Music/F"))
            itlp = os.path.join(out, "iTunes_Control", "iTunes", "iTunes Library.itlp")
            lib = sqlite3.connect(os.path.join(itlp, "Library.itdb"))
            rows = lib.execute("SELECT pid, title, artist, album, is_song, total_time_ms FROM item ORDER BY title").fetchall()
            self.assertEqual([r[1:] for r in rows], [("Song 0", "Zed", "Unknown Album", 1, 1234.0),
                                                     ("Song 1", "Unknown Artist", "Unknown Album", 1, 1234.0)])
            master, = lib.execute("SELECT primary_container_pid FROM db_info").fetchone()
            self.assertEqual(lib.execute("SELECT is_hidden FROM container WHERE pid=?", (master,)).fetchone(), (1,))
            self.assertEqual(lib.execute("SELECT COUNT(*) FROM item_to_container WHERE container_pid=?",
                                         (master,)).fetchone(), (2,))
            # Music.app's Playlists tab shows only is_hidden=0; without this row
            # the installed songs are invisible there (issue on the user's card).
            visible = lib.execute("SELECT pid, name, distinguished_kind, media_kinds FROM container "
                                  "WHERE is_hidden=0").fetchall()
            self.assertEqual([(n, k, m) for _, n, k, m in visible], [(music.VISIBLE_PLAYLIST, 0, 1)])
            vpid = visible[0][0]
            self.assertNotEqual(vpid, master)
            self.assertEqual(lib.execute("SELECT COUNT(*) FROM item_to_container WHERE container_pid=?",
                                         (vpid,)).fetchone(), (2,))
            self.assertEqual(lib.execute("SELECT COUNT(*) FROM album").fetchone(), (2,))   # per (album, artist)
            self.assertEqual(lib.execute("SELECT genre FROM genre_map").fetchall(), [("Rock",)])
            loc = sqlite3.connect(os.path.join(itlp, "Locations.itdb"))
            locs = dict(loc.execute("SELECT item_pid, location FROM location").fetchall())
            for pid, _t, *_ in rows:
                self.assertIn(pid, locs)
            self.assertEqual(sorted(loc.execute("SELECT kind_id FROM location").fetchall()), [(1,), (2,)])
            ex = sqlite3.connect(os.path.join(itlp, "Extras.itdb"))
            self.assertEqual(ex.execute("SELECT * FROM db_schema_upgrade_info").fetchall(), [(8, 19)])
            dyn = sqlite3.connect(os.path.join(itlp, "Dynamic.itdb"))
            self.assertEqual(dyn.execute("SELECT COUNT(*) FROM item_stats").fetchone(), (2,))
            self.assertEqual(dyn.execute("SELECT COUNT(*) FROM container_ui").fetchone(), (2,))
            self.assertTrue(os.path.exists(os.path.join(itlp, "Genius.itdb")))
            for c in (lib, loc, ex, dyn):
                c.close()

    def test_collect(self):
        with tempfile.TemporaryDirectory() as td:
            for n in ("x.mp3", "y.flac", "sub/z.M4A", ".hidden.mp3"):
                os.makedirs(os.path.dirname(os.path.join(td, n)), exist_ok=True)
                open(os.path.join(td, n), "w").close()
            files, skipped = music.collect(td)
            self.assertEqual([os.path.relpath(f, td) for f in files], ["sub/z.M4A", "x.mp3"])
            self.assertEqual([os.path.relpath(f, td) for f in skipped], ["y.flac"])


if __name__ == "__main__":
    unittest.main()
