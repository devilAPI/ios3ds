#!/usr/bin/env python3
"""Songs for the iPod's Music app, built on the host (docs/audio.md section 10).

iOS 4.2.1's MusicLibrary reads the library iTunes writes at sync time:
/var/mobile/Media/iTunes_Control/iTunes/iTunes Library.itlp/{Library,Locations,
Dynamic,Extras,Genius}.itdb (sqlite, schema "user_version 8": the version its
/System/Library/Lockdown/SQLMusicLibraryPostProcessCommands.plist upgrades
from) with the audio files under iTunes_Control/Music/Fxx/. The schema and the
rows below follow what iTunes 9 writes, as documented by libgpod's
itdb_sqlite.c / itdb_sqlite_queries.h (LGPL-2.1+, Nikias Bassen 2009). The
device does the rest itself on the Music app's first start: itdbprepserver
runs the post-processing (sort keys, triggers, artist/album tables). The one
thing iTunes adds that we cannot, the signed Locations.itdb.cbk, is switched off
by a dyld-cache patch (rootfs/recipe.json, MusicLibrary _AOU243378_7_1).

  scripts/music.py --song out.m4a           render the test song (ALAC .m4a)
  scripts/music.py --library DIR [FILES...] build iTunes_Control/ in DIR

mkrootfs.py calls apply() with the test song plus the user's files
(scripts/mksd.py / install.py --music DIR).

The test song is ours: a melody composed for this file and synthesised here
(additive sine voices), written as Apple Lossless in an .m4a with uncompressed
("escape") ALAC frames, which every ALAC decoder accepts. No third-party audio.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import wave

RATE = 44100
FRAME = 4096                    # ALAC frame length (samples per channel)
MEDIA = "/private/var/mobile/Media"
EXTS = {".mp3": ("MP3 ", "MPEG audio file", 301), ".m4a": ("M4A ", "AAC audio file", 502),
        ".m4b": ("M4B ", "AAC audio book", 502), ".wav": ("WAV ", "WAV audio file", 0),
        ".aif": ("AIFF", "AIFF audio file", 0), ".aiff": ("AIFF", "AIFF audio file", 0)}
TEST_SONG = {"title": "ios3ds Test Song", "artist": "ios3ds", "album": "ios3ds Test Tones",
             "genre": "Electronic", "year": 2026, "track": 1}
# The hidden master playlist ("iPod") is not shown anywhere in Music.app's
# Playlists tab: a user who opens the app sees only "Add Playlist..." and
# concludes that no song was installed. Give the tracks one ordinary, visible
# playlist as well; the master stays the db_info primary container.
VISIBLE_PLAYLIST = "ios3ds"

# ---- the test song --------------------------------------------------------------

# (note, beats): note = semitones above A3 (220 Hz), None = rest. 92 bpm, 4/4.
MELODY = [(7, 1), (10, 1), (12, 1), (14, 1), (12, 2), (10, 1), (7, 1),
          (5, 1), (7, 1), (10, 2), (None, 1), (3, 1), (5, 1), (7, 1),
          (10, 1), (12, 1), (14, 1), (17, 1), (15, 2), (14, 1), (12, 1),
          (10, 1), (12, 1), (7, 3), (None, 1),
          (7, 1), (10, 1), (12, 1), (14, 1), (19, 2), (17, 1), (14, 1),
          (12, 1), (10, 1), (12, 2), (None, 1), (10, 1), (7, 1), (5, 1),
          (3, 1), (5, 1), (7, 1), (10, 1), (7, 2), (5, 1), (3, 1),
          (0, 4)]
BASS = [-12, -9, -7, -5]          # one root per bar, cycled (A2 C3 D3 E3)
BPM = 92


def hz(n):
    return 220.0 * 2 ** (n / 12.0)


def render_song(seconds=None):
    """Return interleaved 16-bit stereo samples (list of ints) of the test song."""
    beat = 60.0 / BPM
    total_beats = sum(b for _, b in MELODY)
    n = int((seconds or total_beats * beat + 1.0) * RATE)
    left = [0.0] * n
    right = [0.0] * n

    def voice(f, t0, dur, amp, pan, harm):
        s0 = int(t0 * RATE)
        ln = min(int(dur * RATE), n - s0)
        att, rel = int(0.01 * RATE), int(min(0.25, dur / 2) * RATE)
        w = [2 * math.pi * f * k / RATE for k in range(1, len(harm) + 1)]
        for i in range(max(0, ln)):
            env = min(1.0, i / att) * (1.0 if i < ln - rel else (ln - i) / rel) * math.exp(-1.6 * i / RATE)
            v = amp * env * sum(h * math.sin(wk * i) for h, wk in zip(harm, w))
            left[s0 + i] += v * (1 - pan)
            right[s0 + i] += v * (1 + pan)

    t = 0.0
    for note, beats in MELODY:
        if note is not None:
            voice(hz(note), t, beats * beat * 0.95, 0.30, 0.25, (1.0, 0.35, 0.12))
        t += beats * beat
    for bar in range(int(total_beats // 4)):
        voice(hz(BASS[bar % len(BASS)]), bar * 4 * beat, 4 * beat * 0.9, 0.22, -0.3, (1.0, 0.5, 0.25, 0.1))
    peak = max(1e-9, max(max(abs(x) for x in left), max(abs(x) for x in right)))
    g = 0.7 * 32767 / peak
    out = []
    for a, b in zip(left, right):
        out.append(int(a * g))
        out.append(int(b * g))
    return out


# ---- ALAC .m4a writer (uncompressed frames) ----------------------------------------------


class Bits:
    def __init__(self):
        self.b = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, nbits):
        self.acc = (self.acc << nbits) | (v & ((1 << nbits) - 1))
        self.n += nbits
        while self.n >= 8:
            self.n -= 8
            self.b.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def bytes(self):
        if self.n:
            self.put(0, 8 - self.n)
        return bytes(self.b)


def alac_frame(samples, channels):
    """One ALAC frame in escape (verbatim) mode: ALACEncoder.cpp EncodeStereoEscape/EncodeMono."""
    ns = len(samples) // channels
    bb = Bits()
    bb.put(1 if channels == 2 else 0, 3)        # ID_CPE / ID_SCE
    bb.put(0, 4)                                # element instance tag
    bb.put(0, 12)                               # unused
    partial = ns != FRAME
    bb.put((partial << 3) | 1, 4)               # has-size, shift 0, escape flag
    if partial:
        bb.put(ns, 32)
    for v in samples:
        bb.put(v, 16)
    bb.put(7, 3)                                # ID_END
    return bb.bytes()


def box(kind, *payload):
    data = b"".join(payload)
    return struct.pack(">I4s", 8 + len(data), kind) + data


def full(kind, version, flags, *payload):
    return box(kind, struct.pack(">I", (version << 24) | flags), *payload)


def write_alac_m4a(path, samples, channels=2, rate=RATE, tags=None):
    frames = [alac_frame(samples[i:i + FRAME * channels], channels)
              for i in range(0, len(samples), FRAME * channels)]
    nsamp = len(samples) // channels
    ftyp = box(b"ftyp", b"M4A ", struct.pack(">I", 0), b"M4A mp42isom\0\0\0\0")
    mdat_off = len(ftyp) + 8
    cookie = struct.pack(">IBBBBBBHIII", FRAME, 0, 16, 40, 10, 14, channels, 255,
                         max(len(f) for f in frames), rate * channels * 16, rate)
    alac_entry = box(b"alac", b"\0" * 6, struct.pack(">HHHIHHHHI", 1, 0, 0, 0, channels, 16, 0, 0, rate << 16),
                     full(b"alac", 0, 0, cookie))
    durs = [len(f) for f in frames]
    last = nsamp - FRAME * (len(frames) - 1)
    stts = [(len(frames) - 1, FRAME)] + [(1, last)] if len(frames) > 1 else [(1, last)]
    stbl = box(b"stbl",
               full(b"stsd", 0, 0, struct.pack(">I", 1), alac_entry),
               full(b"stts", 0, 0, struct.pack(">I", len(stts)), b"".join(struct.pack(">II", *e) for e in stts)),
               full(b"stsc", 0, 0, struct.pack(">IIII", 1, 1, len(frames), 1)),
               full(b"stsz", 0, 0, struct.pack(">II", 0, len(frames)), b"".join(struct.pack(">I", d) for d in durs)),
               full(b"stco", 0, 0, struct.pack(">II", 1, mdat_off)))
    matrix = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)
    now = int(time.time()) + 2082844800 if os.environ.get("SOURCE_DATE_EPOCH") is None \
        else int(os.environ["SOURCE_DATE_EPOCH"]) + 2082844800
    mvhd = full(b"mvhd", 0, 0, struct.pack(">IIII", now, now, rate, nsamp), struct.pack(">IH", 0x10000, 0x100),
                b"\0" * 10, matrix, b"\0" * 24, struct.pack(">I", 2))
    tkhd = full(b"tkhd", 0, 7, struct.pack(">IIIII", now, now, 1, 0, nsamp), b"\0" * 8,
                struct.pack(">HHHH", 0, 0, 0x100, 0), matrix, struct.pack(">II", 0, 0))
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIIIHH", now, now, rate, nsamp, 0x55C4, 0))
    hdlr = full(b"hdlr", 0, 0, struct.pack(">I", 0), b"soun", b"\0" * 12, b"SoundHandler\0")
    minf = box(b"minf", full(b"smhd", 0, 0, b"\0" * 4),
               box(b"dinf", full(b"dref", 0, 0, struct.pack(">I", 1), full(b"url ", 0, 1))), stbl)
    trak = box(b"trak", tkhd, box(b"mdia", mdhd, hdlr, minf))
    udta = b""
    if tags:
        items = []
        for key, val in ((b"\xa9nam", tags.get("title")), (b"\xa9ART", tags.get("artist")),
                         (b"\xa9alb", tags.get("album")), (b"\xa9gen", tags.get("genre")),
                         (b"\xa9day", str(tags["year"]) if tags.get("year") else None)):
            if val:
                items.append(box(key, full(b"data", 0, 1, b"\0\0\0\0", val.encode())))
        if tags.get("track"):
            items.append(box(b"trkn", full(b"data", 0, 0, b"\0\0\0\0", struct.pack(">HHHH", 0, tags["track"], 1, 0))))
        meta = full(b"meta", 0, 0, full(b"hdlr", 0, 0, struct.pack(">I", 0), b"mdir", b"appl", b"\0" * 9),
                    box(b"ilst", *items))
        udta = box(b"udta", meta)
    moov = box(b"moov", mvhd, trak, udta)
    with open(path, "wb") as f:
        f.write(ftyp)
        f.write(struct.pack(">I4s", 8 + sum(durs), b"mdat"))
        for fr in frames:
            f.write(fr)
        f.write(moov)
    return nsamp * 1000.0 / rate


def make_test_song(path):
    ms = write_alac_m4a(path, render_song(), tags=TEST_SONG)
    return dict(TEST_SONG, path=path, ms=ms, rate=RATE, bitrate=RATE * 32 // 1000)


def make_test_song_wav(path):
    """The same melody as plain PCM for older firmwares whose software ALAC
    decoder cannot start a stream from our escape-frame .m4a (re-log NEXT)."""
    import array
    samples = render_song()
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(array.array("h", samples).tobytes())
    ms = len(samples) * 500.0 / RATE
    return dict(TEST_SONG, path=path, ms=ms, rate=RATE, bitrate=RATE * 32 // 1000)


# ---- metadata of the user's files -------------------------------------------------------


def probe(path):
    """title/artist/album/genre/year/track/ms/rate/bitrate of an audio file: ffprobe when the
    host has it, else the file name (and the exact length for .wav)."""
    stem = os.path.splitext(os.path.basename(path))[0]
    info = {"title": stem, "artist": None, "album": None, "genre": None, "year": 0, "track": 0,
            "ms": 0.0, "rate": 44100, "bitrate": 0, "codec": None}
    ff = shutil.which("ffprobe")
    if ff:
        r = subprocess.run([ff, "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path],
                           capture_output=True, text=True)
        if r.returncode == 0:
            j = json.loads(r.stdout)
            fmt = j.get("format", {})
            tags = {k.lower(): v for k, v in fmt.get("tags", {}).items()}
            st = next((s for s in j.get("streams", []) if s.get("codec_type") == "audio"), {})
            tags.update({k.lower(): v for k, v in st.get("tags", {}).items() if k.lower() not in tags})
            info["title"] = tags.get("title") or stem
            info["artist"] = tags.get("artist") or tags.get("album_artist")
            info["album"] = tags.get("album")
            info["genre"] = tags.get("genre")
            try:
                info["year"] = int(str(tags.get("date", "0"))[:4])
            except ValueError:
                pass
            try:
                info["track"] = int(str(tags.get("track", "0")).split("/")[0])
            except ValueError:
                pass
            info["ms"] = float(fmt.get("duration") or st.get("duration") or 0) * 1000
            info["rate"] = int(st.get("sample_rate") or 44100)
            info["bitrate"] = int(fmt.get("bit_rate") or st.get("bit_rate") or 0) // 1000
            info["codec"] = st.get("codec_name")
            return info
    if path.lower().endswith(".wav"):
        try:
            with wave.open(path) as w:
                info["ms"] = w.getnframes() * 1000.0 / w.getframerate()
                info["rate"] = w.getframerate()
        except (wave.Error, EOFError):
            pass
    return info


def collect(music_dir):
    """Audio files of music_dir (recursive) the Music app can play; the rest is reported."""
    files, skipped = [], []
    for d, _, fs in os.walk(music_dir):
        for fn in sorted(fs):
            p = os.path.join(d, fn)
            if fn.startswith("."):
                continue
            (files if os.path.splitext(fn)[1].lower() in EXTS else skipped).append(p)
    return sorted(files), skipped


# ---- the library ----------------------------------------------------------------------

LIBRARY_SQL = """
CREATE TABLE version_info (id INTEGER PRIMARY KEY, major INTEGER, minor INTEGER, compatibility INTEGER DEFAULT 0, update_level INTEGER DEFAULT 0, device_update_level INTEGER DEFAULT 0, platform INTEGER DEFAULT 0);
CREATE TABLE db_info (pid INTEGER NOT NULL, primary_container_pid INTEGER, media_folder_url TEXT, audio_language INTEGER, subtitle_language INTEGER, genius_cuid TEXT, bib BLOB, rib BLOB, PRIMARY KEY (pid));
CREATE TABLE item (pid INTEGER NOT NULL, revision_level INTEGER, media_kind INTEGER DEFAULT 0, is_song INTEGER DEFAULT 0, is_audio_book INTEGER DEFAULT 0, is_music_video INTEGER DEFAULT 0, is_movie INTEGER DEFAULT 0, is_tv_show INTEGER DEFAULT 0, is_ringtone INTEGER DEFAULT 0, is_voice_memo INTEGER DEFAULT 0, is_rental INTEGER DEFAULT 0, is_podcast INTEGER DEFAULT 0, date_modified INTEGER DEFAULT 0, date_backed_up INTEGER DEFAULT 0, year INTEGER DEFAULT 0, content_rating INTEGER DEFAULT 0, content_rating_level INTEGER DEFAULT 0, is_compilation INTEGER, is_user_disabled INTEGER DEFAULT 0, remember_bookmark INTEGER DEFAULT 0, exclude_from_shuffle INTEGER DEFAULT 0, part_of_gapless_album INTEGER DEFAULT 0, artwork_status INTEGER, artwork_cache_id INTEGER DEFAULT 0, start_time_ms REAL DEFAULT 0, stop_time_ms REAL DEFAULT 0, total_time_ms REAL DEFAULT 0, total_burn_time_ms REAL, track_number INTEGER DEFAULT 0, track_count INTEGER DEFAULT 0, disc_number INTEGER DEFAULT 0, disc_count INTEGER DEFAULT 0, bpm INTEGER DEFAULT 0, relative_volume INTEGER, eq_preset TEXT, radio_stream_status TEXT, genius_id INTEGER DEFAULT 0, genre_id INTEGER DEFAULT 0, category_id INTEGER DEFAULT 0, album_pid INTEGER DEFAULT 0, artist_pid INTEGER DEFAULT 0, composer_pid INTEGER DEFAULT 0, title TEXT, artist TEXT, album TEXT, album_artist TEXT, composer TEXT, sort_title TEXT, sort_artist TEXT, sort_album TEXT, sort_album_artist TEXT, sort_composer TEXT, title_order INTEGER, artist_order INTEGER, album_order INTEGER, genre_order INTEGER, composer_order INTEGER, album_artist_order INTEGER, album_by_artist_order INTEGER, series_name_order INTEGER, comment TEXT, grouping TEXT, description TEXT, description_long TEXT, PRIMARY KEY (pid));
CREATE TABLE avformat_info (item_pid INTEGER NOT NULL, sub_id INTEGER NOT NULL DEFAULT 0, audio_format INTEGER, bit_rate INTEGER DEFAULT 0, sample_rate REAL DEFAULT 0, duration INTEGER, gapless_heuristic_info INTEGER, gapless_encoding_delay INTEGER, gapless_encoding_drain INTEGER, gapless_last_frame_resynch INTEGER, analysis_inhibit_flags INTEGER, audio_fingerprint INTEGER, volume_normalization_energy INTEGER, PRIMARY KEY (item_pid,sub_id));
CREATE TABLE video_info (item_pid INTEGER NOT NULL, has_alternate_audio INTEGER, has_subtitles INTEGER, characteristics_valid INTEGER, has_closed_captions INTEGER, is_self_contained INTEGER, is_compressed INTEGER, is_anamorphic INTEGER, season_number INTEGER, audio_language INTEGER, audio_track_index INTEGER, audio_track_id INTEGER, subtitle_language INTEGER, subtitle_track_index INTEGER, subtitle_track_id INTEGER, series_name TEXT, sort_series_name TEXT, episode_id TEXT, episode_sort_id INTEGER, network_name TEXT, extended_content_rating TEXT, movie_info TEXT, PRIMARY KEY (item_pid));
CREATE TABLE video_characteristics (item_pid INTEGER, sub_id INTEGER DEFAULT 0, track_id INTEGER, height INTEGER, width INTEGER, depth INTEGER, codec INTEGER, frame_rate REAL, percentage_encrypted REAL, bit_rate INTEGER, peak_bit_rate INTEGER, buffer_size INTEGER, profile INTEGER, level INTEGER, complexity_level INTEGER, UNIQUE (item_pid,sub_id,track_id));
CREATE TABLE store_info (item_pid INTEGER NOT NULL, store_kind INTEGER, date_purchased INTEGER DEFAULT 0, date_released INTEGER DEFAULT 0, account_id INTEGER, key_versions INTEGER, key_platform_id INTEGER, key_id INTEGER, key_id2 INTEGER, store_item_id INTEGER, artist_id INTEGER, composer_id INTEGER, genre_id INTEGER, playlist_id INTEGER, storefront_id INTEGER, store_link_id INTEGER, relevance REAL, popularity REAL, PRIMARY KEY (item_pid));
CREATE TABLE store_link (id INTEGER NOT NULL, url TEXT, PRIMARY KEY (id));
CREATE TABLE podcast_info (item_pid INTEGER NOT NULL, date_released INTEGER DEFAULT 0, external_guid TEXT, feed_url TEXT, feed_keywords TEXT, PRIMARY KEY (item_pid));
CREATE TABLE container (pid INTEGER NOT NULL, distinguished_kind INTEGER, date_created INTEGER, date_modified INTEGER, name TEXT, name_order INTEGER, parent_pid INTEGER, media_kinds INTEGER, workout_template_id INTEGER, is_hidden INTEGER, smart_is_folder INTEGER, smart_is_dynamic INTEGER, smart_is_filtered INTEGER, smart_is_genius INTEGER, smart_enabled_only INTEGER, smart_is_limited INTEGER, smart_limit_kind INTEGER, smart_limit_order INTEGER, smart_limit_value INTEGER, smart_reverse_limit_order INTEGER, smart_criteria BLOB, description TEXT, PRIMARY KEY (pid));
CREATE TABLE item_to_container (item_pid INTEGER, container_pid INTEGER, physical_order INTEGER, shuffle_order INTEGER);
CREATE TABLE container_seed (container_pid INTEGER NOT NULL, item_pid INTEGER NOT NULL, seed_order INTEGER DEFAULT 0, UNIQUE (container_pid,item_pid));
CREATE TABLE album (pid INTEGER NOT NULL, kind INTEGER, artwork_status INTEGER, artwork_item_pid INTEGER, artist_pid INTEGER, user_rating INTEGER, name TEXT, name_order INTEGER, all_compilations INTEGER, feed_url TEXT, season_number INTEGER, PRIMARY KEY (pid));
CREATE TABLE item_to_album (item_pid INTEGER, album_pid INTEGER);
CREATE TABLE artist (pid INTEGER NOT NULL, kind INTEGER, artwork_status INTEGER, artwork_album_pid INTEGER, name TEXT, name_order INTEGER, sort_name TEXT, PRIMARY KEY (pid));
CREATE TABLE item_to_artist (item_pid INTEGER, artist_pid INTEGER);
CREATE TABLE composer (pid INTEGER NOT NULL, name TEXT, name_order INTEGER, sort_name TEXT, PRIMARY KEY (pid));
CREATE TABLE item_to_composer (item_pid INTEGER, composer_pid INTEGER);
CREATE TABLE location_kind_map (id INTEGER NOT NULL, kind TEXT NOT NULL, PRIMARY KEY (id), UNIQUE (kind));
CREATE TABLE genre_map (id INTEGER NOT NULL, genre TEXT NOT NULL, genre_order INTEGER DEFAULT 0, PRIMARY KEY (id), UNIQUE (genre));
CREATE TABLE category_map (id INTEGER NOT NULL, category TEXT NOT NULL, PRIMARY KEY (id), UNIQUE (category));
CREATE TRIGGER insert_item AFTER INSERT ON item BEGIN UPDATE item SET is_song=((new.media_kind&1)!=0), is_audio_book=((new.media_kind&8)!=0), is_music_video=((new.media_kind&32)!=0), is_movie=((new.media_kind&2)!=0), is_tv_show=((new.media_kind&64)!=0), is_ringtone=((new.media_kind&16384)!=0), is_podcast=((new.media_kind&4)!=0), is_rental=((new.media_kind&32768)!=0) WHERE pid = new.pid; END;
CREATE TRIGGER update_item_media_kind AFTER UPDATE OF media_kind ON item BEGIN UPDATE item SET is_song=((new.media_kind&1)!=0), is_audio_book=((new.media_kind&8)!=0), is_music_video=((new.media_kind&32)!=0), is_movie=((new.media_kind&2)!=0), is_tv_show=((new.media_kind&64)!=0), is_ringtone=((new.media_kind&16384)!=0), is_podcast=((new.media_kind&4)!=0), is_rental=((new.media_kind&32768)!=0) WHERE pid = new.pid; END;
"""
LOCATIONS_SQL = """
CREATE TABLE location (item_pid INTEGER NOT NULL, sub_id INTEGER NOT NULL DEFAULT 0, base_location_id INTEGER DEFAULT 0, location_type INTEGER, location TEXT, extension INTEGER, kind_id INTEGER DEFAULT 0, date_created INTEGER DEFAULT 0, file_size INTEGER DEFAULT 0, file_creator INTEGER, file_type INTEGER, num_dir_levels_file INTEGER, num_dir_levels_lib INTEGER, PRIMARY KEY (item_pid,sub_id));
CREATE TABLE base_location (id INTEGER NOT NULL, path TEXT, PRIMARY KEY (id));
"""
DYNAMIC_SQL = """
CREATE TABLE item_stats (item_pid INTEGER NOT NULL, has_been_played INTEGER DEFAULT 0, date_played INTEGER DEFAULT 0, play_count_user INTEGER DEFAULT 0, play_count_recent INTEGER DEFAULT 0, date_skipped INTEGER DEFAULT 0, skip_count_user INTEGER DEFAULT 0, skip_count_recent INTEGER DEFAULT 0, bookmark_time_ms REAL, bookmark_time_ms_common REAL, user_rating INTEGER DEFAULT 0, user_rating_common INTEGER DEFAULT 0, rental_expired INTEGER DEFAULT 0, hidden INTEGER DEFAULT 0, deleted INTEGER DEFAULT 0, has_changes INTEGER DEFAULT 0, PRIMARY KEY (item_pid));
CREATE TABLE container_ui (container_pid INTEGER NOT NULL, play_order INTEGER DEFAULT 0, is_reversed INTEGER DEFAULT 0, album_field_order INTEGER DEFAULT 0, repeat_mode INTEGER DEFAULT 0, shuffle_items INTEGER DEFAULT 0, has_been_shuffled INTEGER DEFAULT 0, PRIMARY KEY (container_pid));
CREATE TABLE rental_info (item_pid INTEGER NOT NULL, rental_date_started INTEGER DEFAULT 0, rental_duration INTEGER DEFAULT 0, rental_playback_date_started INTEGER DEFAULT 0, rental_playback_duration INTEGER DEFAULT 0, is_demo INTEGER DEFAULT 0, PRIMARY KEY (item_pid));
"""
EXTRAS_SQL = """
CREATE TABLE chapter (item_pid INTEGER NOT NULL, data BLOB, PRIMARY KEY (item_pid));
CREATE TABLE lyrics (item_pid INTEGER NOT NULL, checksum INTEGER, lyrics TEXT, PRIMARY KEY (item_pid));
CREATE TABLE db_schema_upgrade_info (user_version INTEGER, device_version INTEGER);
INSERT INTO db_schema_upgrade_info VALUES(8,19);
"""
GENIUS_SQL = """
CREATE TABLE genius_metadata (genius_id INTEGER NOT NULL, version INTEGER, data BLOB, PRIMARY KEY (genius_id));
CREATE TABLE genius_similarities (genius_id INTEGER NOT NULL, version INTEGER, data BLOB, PRIMARY KEY (genius_id));
CREATE TABLE genius_config (id INTEGER NOT NULL, version INTEGER, default_num_results INTEGER DEFAULT 0, min_num_results INTEGER DEFAULT 0, data BLOB, PRIMARY KEY (id), UNIQUE (version));
"""
MAC_EPOCH = 978307200           # 2001-01-01, the epoch of the date columns
UNKNOWN_ARTIST, UNKNOWN_ALBUM = "Unknown Artist", "Unknown Album"


def pid(*parts):
    """Stable signed 64-bit persistent id (iTunes uses random ones)."""
    h = hashlib.sha1("\0".join(str(p) for p in parts).encode()).digest()
    return struct.unpack(">q", h[:8])[0] or 1


def orders(values):
    """iTunes-style *_order columns: rank * 100 in sorted order (libgpod compute_key_orders)."""
    return {v: (i + 1) * 100 for i, v in enumerate(sorted(set(values), key=lambda s: s.casefold()))}


def db(path, sql):
    if os.path.exists(path):
        os.remove(path)
    c = sqlite3.connect(path)
    c.execute("PRAGMA legacy_file_format = ON")     # iOS 4.2.1's sqlite 3.6
    c.executescript("BEGIN;" + sql + "COMMIT;")
    return c


def build_library(tracks, out):
    """tracks: dicts from probe()/make_test_song() with 'path'. Writes out/iTunes_Control/
    (Music/F00.., iTunes/iTunes Library.itlp/*.itdb) and returns the list of
    (local path, device path relative to /var/mobile/Media)."""
    ctl = os.path.join(out, "iTunes_Control")
    itlp = os.path.join(ctl, "iTunes", "iTunes Library.itlp")
    os.makedirs(itlp, exist_ok=True)
    now = int(os.environ.get("SOURCE_DATE_EPOCH", time.time())) - MAC_EPOCH
    lib_pid = pid("ios3ds library")
    master = pid("ios3ds master playlist")
    user_playlist = pid("ios3ds visible playlist")
    for t in tracks:
        t["artist"] = t.get("artist") or UNKNOWN_ARTIST
        t["album"] = t.get("album") or UNKNOWN_ALBUM
        t["pid"] = pid("item", t["title"], t["artist"], t["album"], os.path.basename(t["path"]))
    o_title = orders(t["title"] for t in tracks)
    o_artist = orders(t["artist"] for t in tracks)
    o_album = orders(t["album"] for t in tracks)
    o_genre = orders(t["genre"] for t in tracks if t.get("genre"))

    lib = db(os.path.join(itlp, "Library.itdb"), LIBRARY_SQL)
    lib.execute("INSERT INTO version_info VALUES(1,2,40,0,0,0,2)")
    lib.execute("INSERT INTO db_info VALUES(?,?,NULL,0,0,NULL,NULL,NULL)", (lib_pid, master))
    genres = {}
    for g in sorted({t["genre"] for t in tracks if t.get("genre")}):
        genres[g] = len(genres) + 1
        lib.execute("INSERT INTO genre_map (id,genre,genre_order) VALUES(?,?,0)", (genres[g], g))
    files = []
    kinds = {}
    seen_album, seen_artist = set(), set()
    for i, t in enumerate(tracks):
        ext = os.path.splitext(t["path"])[1].lower()
        marker, kind, audio_format = EXTS[ext]
        folder = "F%02d" % (i % 50)
        name = "%s%s" % (("%08X" % (t["pid"] & 0xFFFFFFFF))[:4] + "%04d" % i, ext)
        rel = "%s/%s" % (folder, name)
        files.append((t["path"], "iTunes_Control/Music/" + rel))
        t["rel"] = rel
        artist_pid = pid("artist", t["artist"])
        album_pid = pid("album", t["album"], t["artist"])
        lib.execute(
            "INSERT INTO item (pid,media_kind,date_modified,year,is_compilation,remember_bookmark,"
            "exclude_from_shuffle,artwork_status,artwork_cache_id,start_time_ms,stop_time_ms,total_time_ms,"
            "total_burn_time_ms,track_number,track_count,disc_number,disc_count,bpm,relative_volume,eq_preset,"
            "radio_stream_status,genre_id,album_pid,artist_pid,composer_pid,title,artist,album,album_artist,"
            "composer,sort_title,sort_artist,sort_album,sort_album_artist,sort_composer,title_order,artist_order,"
            "album_order,genre_order,composer_order,album_artist_order,album_by_artist_order,series_name_order,"
            "comment,grouping,description,description_long) VALUES(?,1,?,?,0,0,0,2,0,0,0,?,NULL,?,0,1,1,0,0,NULL,"
            "NULL,?,?,?,0,?,?,?,?,NULL,?,?,?,?,NULL,?,?,?,?,100,?,?,100,NULL,NULL,NULL,NULL)",
            (t["pid"], now, t.get("year") or 0, float(t.get("ms") or 0), t.get("track") or 0,
             genres.get(t.get("genre"), 0), album_pid, artist_pid, t["title"], t["artist"], t["album"], t["artist"],
             t["title"], t["artist"], t["album"], t["artist"], o_title[t["title"]], o_artist[t["artist"]],
             o_album[t["album"]], o_genre.get(t.get("genre"), 100), o_artist[t["artist"]], o_album[t["album"]]))
        if kind not in kinds:
            kinds[kind] = len(kinds) + 1
            lib.execute("INSERT INTO location_kind_map VALUES(?,?)", (kinds[kind], kind))
        t["kind_id"] = kinds[kind]
        lib.execute("INSERT INTO avformat_info VALUES(?,0,?,?,?,0,0,0,0,0,0,0,0)",
                    (t["pid"], audio_format, t.get("bitrate") or 0, float(t.get("rate") or 44100)))
        lib.execute("INSERT INTO item_to_container VALUES(?,?,?,NULL)", (t["pid"], master, i))
        lib.execute("INSERT INTO item_to_container VALUES(?,?,?,NULL)", (t["pid"], user_playlist, i))
        if album_pid not in seen_album:
            seen_album.add(album_pid)
            lib.execute("INSERT INTO album VALUES(?,2,0,0,?,0,?,?,0,NULL,0)",
                        (album_pid, artist_pid, t["album"], o_album[t["album"]]))
        if artist_pid not in seen_artist:
            seen_artist.add(artist_pid)
            lib.execute("INSERT INTO artist VALUES(?,2,0,0,?,?,?)",
                        (artist_pid, t["artist"], o_artist[t["artist"]], t["artist"]))
    lib.execute("INSERT INTO container VALUES(?,0,?,?,?,0,0,1,0,1,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,"
                "NULL,NULL)", (master, now, now, "iPod"))
    # iTunes writes an ordinary playlist with distinguished_kind 0, is_hidden 0
    # (libgpod itdb_sqlite.c). This is the one Music.app shows on its first tab.
    lib.execute("INSERT INTO container VALUES(?,0,?,?,?,1,0,1,0,0,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,"
                "NULL,NULL)", (user_playlist, now, now, VISIBLE_PLAYLIST))
    lib.commit()
    lib.close()

    loc = db(os.path.join(itlp, "Locations.itdb"), LOCATIONS_SQL)
    loc.executemany("INSERT INTO base_location (id, path) VALUES (?,?)",
                    [(1, "iTunes_Control/Music"), (4, "Podcasts"), (6, "iTunes_Control/Ringtones")])
    for t in tracks:
        marker = EXTS[os.path.splitext(t["path"])[1].lower()][0]
        loc.execute("INSERT INTO location (item_pid,sub_id,base_location_id,location_type,location,extension,"
                    "kind_id,date_created,file_size) VALUES(?,0,1,?,?,?,?,?,?)",
                    (t["pid"], 0x46494C45, t["rel"], struct.unpack(">I", marker.encode())[0], t["kind_id"], now,
                     os.path.getsize(t["path"])))
    loc.commit()
    loc.close()

    dyn = db(os.path.join(itlp, "Dynamic.itdb"), DYNAMIC_SQL)
    dyn.executemany("INSERT INTO item_stats (item_pid,has_been_played,date_played,play_count_user,play_count_recent,"
                    "date_skipped,skip_count_user,skip_count_recent,bookmark_time_ms,bookmark_time_ms_common,"
                    "user_rating,user_rating_common) VALUES(?,0,0,0,0,0,0,0,0,0,0,0)",
                    [(t["pid"],) for t in tracks])
    dyn.execute("INSERT INTO container_ui VALUES(?,7,0,1,0,0,0)", (master,))
    dyn.execute("INSERT INTO container_ui VALUES(?,7,0,1,0,0,0)", (user_playlist,))
    dyn.commit()
    dyn.close()
    for name, sql in (("Extras.itdb", EXTRAS_SQL), ("Genius.itdb", GENIUS_SQL)):
        c = db(os.path.join(itlp, name), sql)
        c.commit()
        c.close()
    for src, dst in files:
        d = os.path.join(out, dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(src, d)
    return files


def gather(music_dir=None, test_song=True, work=None, verbose=True, wav_test=False):
    """Track list: the test song (rendered into work) + the playable files of music_dir."""
    tracks = []
    if test_song:
        if wav_test:
            tracks.append(make_test_song_wav(os.path.join(work, "ios3ds-test-song.wav")))
        else:
            tracks.append(make_test_song(os.path.join(work, "ios3ds-test-song.m4a")))
    if music_dir:
        if not os.path.isdir(music_dir):
            raise SystemExit("[music] no such folder: %s" % music_dir)
        files, skipped = collect(music_dir)
        for p in files:
            t = probe(p)
            t["path"] = p
            if t.get("codec") and os.path.splitext(p)[1].lower() == ".m4a" and t["codec"] not in ("aac", "alac"):
                skipped.append(p)
                continue
            tracks.append(t)
        if verbose:
            for p in skipped:
                print("[music] skipped %s (the Music app plays .mp3, .m4a (AAC/ALAC), .wav, .aif)" % p)
            if files and not shutil.which("ffprobe"):
                print("[music] no ffprobe on this computer: titles come from the file names")
    return tracks


def apply(img, hfs, music_dir=None, test_song=True, verbose=True, legacy_db=None, wav_test=False):
    """Add the library to the rootfs image (mkrootfs.py; hfs = its hfsplus runner).

    legacy_db: path of the host helper that writes iOS 2.x's binary
    iTunes/iTunesDB (tools/music-itdb/mkitdb.c). wav_test: write the generated
    test song as PCM instead of ALAC for old firmwares. 4.x recipes set neither
    and keep the unchanged ALAC/sqlite library.
    """
    with tempfile.TemporaryDirectory() as td:
        tracks = gather(music_dir, test_song, td, verbose, wav_test)
        if not tracks:
            return 0
        out = os.path.join(td, "Media")
        build_library(tracks, out)
        if legacy_db:
            itdb = os.path.join(out, "iTunes", "iTunesDB")
            os.makedirs(os.path.dirname(itdb), exist_ok=True)
            args = [legacy_db, itdb]
            for t in tracks:
                args += [t["title"], t["artist"], t["album"],
                         "iTunes_Control:Music:" + t["rel"].replace("/", ":"),
                         str(int(t.get("ms") or 0))]
            subprocess.run(args, check=True)
            # 5H11a's MusicLibrary resource table names iTunes/iTunesDB; some
            # 2.x builds look under iTunes_Control/iTunes. Install both so the
            # test song is found either way (harmless duplicate, old versions only).
            alt = os.path.join(out, "iTunes_Control", "iTunes", "iTunesDB")
            os.makedirs(os.path.dirname(alt), exist_ok=True)
            shutil.copyfile(itdb, alt)
            if verbose:
                print("[music] iOS 2.x library: iTunes/iTunesDB + iTunes_Control/iTunes/iTunesDB "
                      "(%d song(s))" % len(tracks))
        for d, dirs, fs in os.walk(out):
            rel = os.path.relpath(d, out)
            dev = MEDIA if rel == "." else MEDIA + "/" + rel.replace(os.sep, "/")
            if rel != ".":
                hfs(img, "mkdir", dev, check=False)     # owner: mobile, from /var/mobile/Media
                hfs(img, "chmod", "0755", dev)
            for fn in sorted(fs):
                hfs(img, "add", os.path.join(d, fn), dev + "/" + fn)
                hfs(img, "chmod", "0644", dev + "/" + fn)
        if verbose:
            print("[music] %d song(s) in the Music app: %s" % (len(tracks), ", ".join(
                "%s - %s" % (t["artist"], t["title"]) for t in tracks)))
    return len(tracks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--song", help="write the test song (.m4a) here")
    ap.add_argument("--library", help="build iTunes_Control/ in this folder")
    ap.add_argument("--no-test-song", action="store_true")
    ap.add_argument("--music", help="folder with the user's audio files")
    a = ap.parse_args()
    if a.song:
        t = make_test_song(a.song)
        print("[music] %s: %.1f s ALAC" % (a.song, t["ms"] / 1000))
    if a.library:
        with tempfile.TemporaryDirectory() as td:
            tracks = gather(a.music, not a.no_test_song, td)
            for src, dst in build_library(tracks, a.library):
                print("[music] %s -> %s" % (src, dst))


if __name__ == "__main__":
    sys.exit(main())
