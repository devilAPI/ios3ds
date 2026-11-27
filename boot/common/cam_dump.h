/* Camera frame-dump file names (ARM9 writer + host tools + host test).
 *
 * The ARM9 dumps the shim's two newest 320x240 YUV422 frames at every camera
 * stop. They used to land in sd:/ios3ds/, now they live in sd:/ios3ds/photos/.
 * The name is unchanged: CAMnnnnnnnn.RAW, an 8.3 (FAT-safe) name with eight
 * decimal digits. A sequence number that no longer fits in eight digits wraps
 * so the name stays 8.3; that needs >100 million frames (months of streaming).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAM_DUMP_ROOT       "0:/ios3ds"
#define CAM_PHOTOS_DIR      "0:/ios3ds/photos"
#define CAM_DUMP_DIR        CAM_PHOTOS_DIR
#define CAM_DUMP_EXT        ".RAW"
#define CAM_DUMP_DIGITS     8u
#define CAM_DUMP_NAME_LEN   15u              /* "CAM00000001.RAW" */
#define CAM_DUMP_PATH_LEN   33u              /* "0:/ios3ds/photos/CAM00000001.RAW" + NUL */

static inline char cam_upper_(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : c;
}

/* Length of a NUL-terminated name, without pulling in string.h. */
static inline size_t cam_name_len_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* True for both "CAM00000001.RAW" and a case-insensitive LFN spelling. */
static inline bool cam_is_dump_name(const char *name)
{
    if (cam_name_len_(name) != CAM_DUMP_NAME_LEN) return false;
    if (cam_upper_(name[0]) != 'C' || cam_upper_(name[1]) != 'A' ||
        cam_upper_(name[2]) != 'M' || name[11] != '.') return false;
    for (size_t i = 3; i < 11; i++)
        if (name[i] < '0' || name[i] > '9') return false;
    return cam_upper_(name[12]) == 'R' && cam_upper_(name[13]) == 'A' &&
           cam_upper_(name[14]) == 'W';
}

/* Format "0:/ios3ds/photos/CAMnnnnnnnn.RAW" into dst; returns its length
   (0 when dst is too small). The writer and the host test use the same code. */
static inline size_t cam_dump_path(char *dst, size_t cap, uint32_t seq)
{
    static const char prefix[] = CAM_DUMP_DIR "/CAM";
    const size_t pfx = sizeof(prefix) - 1;
    if (cap < pfx + CAM_DUMP_DIGITS + sizeof(CAM_DUMP_EXT)) return 0;
    for (size_t i = 0; i < pfx; i++) dst[i] = prefix[i];
    seq %= 100000000u;
    for (size_t i = 0; i < CAM_DUMP_DIGITS; i++) {
        dst[pfx + CAM_DUMP_DIGITS - 1 - i] = (char)('0' + seq % 10u);
        seq /= 10u;
    }
    size_t at = pfx + CAM_DUMP_DIGITS;
    for (size_t i = 0; i < sizeof(CAM_DUMP_EXT); i++) dst[at++] = CAM_DUMP_EXT[i];
    return at - 1;
}
