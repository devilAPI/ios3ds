/* Build the binary iTunesDB used by iOS 2.x Music.app from our own test song.
 *
 * iOS 4.x reads the sqlite "iTunes Library.itlp" written by scripts/music.py;
 * 5H11a (2.2.1) reads "iTunes/iTunesDB" instead. libgpod (LGPL-2.1+) knows
 * that format, so this small host helper uses its public API. No Apple code or
 * data is compiled in: the track metadata comes from scripts/music.py.
 *
 *   build/mkitdb OUT.itdb TITLE ARTIST ALBUM IPOD_PATH DURATION_MS [...]
 *
 * The path uses libgpod's ':' separators, e.g.
 * iTunes_Control:Music:F00:00000001.m4a, relative to /var/mobile/Media.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gpod/itdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 7 || (argc - 2) % 5) {
        fprintf(stderr, "usage: %s OUT.itdb TITLE ARTIST ALBUM IPOD_PATH DURATION_MS [...]\n", argv[0]);
        return 2;
    }
    Itdb_iTunesDB *db = itdb_new();
    db->device = itdb_device_new();
    itdb_device_set_sysinfo(db->device, "ModelNumStr", "N72AP");
    Itdb_Playlist *mpl = itdb_playlist_new("iPod", FALSE);
    itdb_playlist_set_mpl(mpl);
    itdb_playlist_add(db, mpl, -1);
    Itdb_Playlist *pl = itdb_playlist_new("ios3ds", FALSE);
    itdb_playlist_add(db, pl, -1);

    for (int i = 2; i + 4 < argc; i += 5) {
        Itdb_Track *t = itdb_track_new();
        t->title = g_strdup(argv[i]);
        t->artist = g_strdup(argv[i + 1]);
        t->album = g_strdup(argv[i + 2]);
        t->ipod_path = g_strdup(argv[i + 3]);
        t->genre = g_strdup("Electronic");
        t->filetype = g_strdup("M4A ");
        t->tracklen = (gint32)strtol(argv[i + 4], NULL, 10);
        t->track_nr = (i - 2) / 5 + 1;
        t->year = 2026;
        t->samplerate = 44100;
        t->mediatype = ITDB_MEDIATYPE_AUDIO;
        itdb_track_add(db, t, -1);
        itdb_playlist_add_track(mpl, t, -1);
        itdb_playlist_add_track(pl, t, -1);
    }
    GError *err = NULL;
    int rc = 0;
    if (!itdb_write_file(db, argv[1], &err)) {
        fprintf(stderr, "mkitdb: %s\n", err && err->message ? err->message : "itdb_write_file failed");
        rc = 1;
    } else {
        printf("mkitdb: %d track(s), db version 0x%x -> %s\n", g_list_length(db->tracks), db->version, argv[1]);
    }
    if (err)
        g_error_free(err);
    itdb_free(db);
    return rc;
}
