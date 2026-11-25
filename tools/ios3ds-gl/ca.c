/* ios3ds GL engine: CoreAnimation's compositor on the PICA200 (docs/gpu.md 7).
 *
 * SpringBoard (the render server) runs with IOS3DS_CA=1. QuartzCore's display
 * probes for an EAGL context at start-up; if it gets one, CA composites every
 * frame with OpenGL ES through this engine, otherwise in software. So the
 * decision is made once, at that first context:
 *   1. boot.cfg (gpu=, ca_gpu=) through the GPU service (pica_be.c),
 *   2. the PICA200 back end's bring-up and self-check,
 *   3. ca_scene_check() below: a small CA-like scene (clear, alpha-blended
 *      texture, translucent fill, a rotated quad) drawn on the CPU rasterizer
 *      and on the GPU, compared pixel by pixel.
 * Any failure: no context, CA's own software compositor, one log line.
 * After that a GPU failure turns the back end into the CPU rasterizer for
 * the rest of the run (pica_be.c gpu_fail): the screen keeps working.
 *
 * ca_frame() is called on every CA swap and logs, every 30 s, what a frame
 * costs the render thread.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include "engine.h"
#include "gl_slots.h"
#ifndef GL_HOST
#include <mach/mach_time.h>
#endif

typedef void (*F_u)(GC *, GLenum);
typedef void (*F_uu)(GC *, GLenum, GLenum);
typedef void (*F_ui)(GC *, GLenum, GLuint);
typedef void (*F_uuu)(GC *, GLenum, GLenum, GLint);
typedef void (*F_gen)(GC *, GLsizei, GLuint *);
typedef void (*F_del)(GC *, GLsizei, const GLuint *);
typedef void (*F_teximg)(GC *, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*F_fbtex)(GC *, GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*F_4i)(GC *, GLint, GLint, GLsizei, GLsizei);
typedef void (*F_v)(GC *);
typedef void (*F_6f)(GC *, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*F_4f)(GC *, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*F_3f)(GC *, GLfloat, GLfloat, GLfloat);
typedef void (*F_ptr)(GC *, GLint, GLenum, GLsizei, const void *);
typedef void (*F_draw)(GC *, GLenum, GLint, GLsizei);
typedef void (*F_read)(GC *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef GLenum (*F_err)(GC *);
typedef void (*F_f)(GC *, GLfloat);
typedef void (*F_b)(GC *, GLboolean);
typedef void (*F_store)(GC *, GLenum, GLenum, GLsizei, GLsizei);
typedef void (*F_fbrb)(GC *, GLenum, GLenum, GLenum, GLuint);
#define FN(tab, f, T) ((T)(tab)[SLOT_##f])

enum { S = 96 };                        /* scene size */
#define FBO 0x8D40                      /* GL_FRAMEBUFFER_OES */
#define COL0 0x8CE0                     /* GL_COLOR_ATTACHMENT0_OES */
#define RBUF 0x8D41                     /* GL_RENDERBUFFER_OES */

/* draw the scene through gc's dispatch table into a texture-backed FBO; read it back */
static int scene(GC *gc, void **t, uint8_t *out)
{
    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static const float rep_uv[8] = { 0, 0, 2.5f, 0, 0, 3.5f, 2.5f, 3.5f };
    static const float rect_uv[8] = { 0, 0, 20, 0, 0, 12, 20, 12 };     /* texels of 20x12 */
    static uint32_t px[32 * 32], np[20 * 12];
    static float v[8];
    GLuint tex[4] = { 0, 0, 0, 0 }, fb = 0;
    int x, y;
    for (y = 0; y < 32; y++)                /* RGBA bytes: alpha ramp, colour cells */
        for (x = 0; x < 32; x++)
            px[y * 32 + x] = (uint32_t)(x * 8) << 24 | ((x ^ y) & 4 ? 0x2080F0u : 0xF0C040u);
    for (y = 0; y < 12; y++)                /* opaque blocks, not a power of two (hardware run 15) */
        for (x = 0; x < 20; x++)
            np[y * 20 + x] = 0xFF000000u | ((x < 10) ^ (y < 6) ? 0xF0F0F0u : y < 6 ? 0x30C030u : 0x5050A0u);
    FN(t, glGenTextures, F_gen)(gc, 4, tex);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex[0]);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    FN(t, glGenFramebuffersOES, F_gen)(gc, 1, &fb);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, fb);
    FN(t, glFramebufferTexture2DOES, F_fbtex)(gc, FBO, COL0, GL_TEXTURE_2D, tex[0], 0);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex[1]);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FN(t, glViewport, F_4i)(gc, 0, 0, S, S);
    FN(t, glMatrixMode, F_u)(gc, GL_PROJECTION);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glOrthof, F_6f)(gc, 0, S, 0, S, -1, 1);
    FN(t, glMatrixMode, F_u)(gc, GL_MODELVIEW);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glClearColor, F_4f)(gc, 0.1f, 0.2f, 0.3f, 1.0f);
    FN(t, glClear, F_u)(gc, GL_COLOR_BUFFER_BIT);
    FN(t, glEnable, F_u)(gc, GL_BLEND);
    FN(t, glBlendFunc, F_uu)(gc, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);           /* premultiplied, as CA */
    FN(t, glEnableClientState, F_u)(gc, GL_VERTEX_ARRAY);
    FN(t, glEnableClientState, F_u)(gc, GL_TEXTURE_COORD_ARRAY);
    FN(t, glVertexPointer, F_ptr)(gc, 2, GL_FLOAT, 0, v);
    FN(t, glTexCoordPointer, F_ptr)(gc, 2, GL_FLOAT, 0, quad);
    /* a textured, blended quad */
    FN(t, glEnable, F_u)(gc, GL_TEXTURE_2D);
    FN(t, glTexEnvi, F_uuu)(gc, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    FN(t, glColor4f, F_4f)(gc, 1, 1, 1, 1);
    v[0] = 8; v[1] = 8; v[2] = 72; v[3] = 8; v[4] = 8; v[5] = 72; v[6] = 72; v[7] = 72;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    /* NPOT layers, as CA's are: a 2D texture repeated (pattern colours), and
       a rectangle texture (0x84F5, texel coordinates) clamped */
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex[2]);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, 20, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE, np);
    FN(t, glTexCoordPointer, F_ptr)(gc, 2, GL_FLOAT, 0, rep_uv);
    v[0] = 74; v[1] = 42; v[2] = 94; v[3] = 42; v[4] = 74; v[5] = 92; v[6] = 94; v[7] = 92;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    FN(t, glDisable, F_u)(gc, GL_TEXTURE_2D);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_RECTANGLE_IOS, tex[3]);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_RECTANGLE_IOS, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_RECTANGLE_IOS, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_RECTANGLE_IOS, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_RECTANGLE_IOS, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_RECTANGLE_IOS, 0, GL_RGBA, 20, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                  np);
    FN(t, glEnable, F_u)(gc, GL_TEXTURE_RECTANGLE_IOS);
    FN(t, glTexCoordPointer, F_ptr)(gc, 2, GL_FLOAT, 0, rect_uv);
    v[0] = 74; v[1] = 26; v[2] = 94; v[3] = 26; v[4] = 74; v[5] = 38; v[6] = 94; v[7] = 38;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    FN(t, glDisable, F_u)(gc, GL_TEXTURE_RECTANGLE_IOS);
    FN(t, glTexCoordPointer, F_ptr)(gc, 2, GL_FLOAT, 0, quad);
    /* a translucent fill, rotated */
    FN(t, glDisable, F_u)(gc, GL_TEXTURE_2D);
    FN(t, glBlendFunc, F_uu)(gc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    FN(t, glColor4f, F_4f)(gc, 0.9f, 0.1f, 0.4f, 0.5f);
    FN(t, glTranslatef, F_3f)(gc, 60, 50, 0);
    FN(t, glRotatef, F_4f)(gc, 30, 0, 0, 1);
    v[0] = -24; v[1] = -16; v[2] = 24; v[3] = -16; v[4] = -24; v[5] = 16; v[6] = 24; v[7] = 16;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    FN(t, glDisable, F_u)(gc, GL_BLEND);
    FN(t, glDisableClientState, F_u)(gc, GL_TEXTURE_COORD_ARRAY);
    FN(t, glReadPixels, F_read)(gc, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, out);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, 0);
    FN(t, glDeleteFramebuffersOES, F_del)(gc, 1, &fb);
    FN(t, glDeleteTextures, F_del)(gc, 4, tex);
    return FN(t, glGetError, F_err)(gc) == GL_NO_ERROR;
}

/* The depth probe (hardware run 15: GL_DEPTH_TEST drew nothing on the
   PICA200, everything in QEMU's model). Depth buffer, glClear of depth,
   GL_LESS and GL_GREATER: green at 0.5 over all; red at 0.75 (hidden);
   blue at 0.25 on the left half (shown); yellow at 0.9 with GL_GREATER on
   the top half (shown). */
static void zquad(GC *gc, void **t, float *v, float x0, float y0, float x1, float y1, float z)
{
    v[0] = x0; v[1] = y0; v[3] = x1; v[4] = y0; v[6] = x0; v[7] = y1; v[9] = x1; v[10] = y1;
    v[2] = v[5] = v[8] = v[11] = z;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
}

static int depth_scene(GC *gc, void **t, uint8_t *out)
{
    static float v[12];
    GLuint tex = 0, fb = 0, rb = 0;
    FN(t, glGenTextures, F_gen)(gc, 1, &tex);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    FN(t, glGenFramebuffersOES, F_gen)(gc, 1, &fb);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, fb);
    FN(t, glFramebufferTexture2DOES, F_fbtex)(gc, FBO, COL0, GL_TEXTURE_2D, tex, 0);
    FN(t, glGenRenderbuffersOES, F_gen)(gc, 1, &rb);
    FN(t, glBindRenderbufferOES, F_ui)(gc, RBUF, rb);
    FN(t, glRenderbufferStorageOES, F_store)(gc, RBUF, GL_DEPTH_COMPONENT16, S, S);
    FN(t, glFramebufferRenderbufferOES, F_fbrb)(gc, FBO, GL_DEPTH_ATTACHMENT, RBUF, rb);
    FN(t, glViewport, F_4i)(gc, 0, 0, S, S);
    FN(t, glMatrixMode, F_u)(gc, GL_PROJECTION);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glOrthof, F_6f)(gc, 0, S, 0, S, -1, 1);       /* window depth = (1 - eye z) / 2 */
    FN(t, glMatrixMode, F_u)(gc, GL_MODELVIEW);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glClearColor, F_4f)(gc, 0, 0, 0, 1);
    FN(t, glClearDepthf, F_f)(gc, 1.0f);
    FN(t, glClear, F_u)(gc, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    FN(t, glEnable, F_u)(gc, GL_DEPTH_TEST);
    FN(t, glDepthFunc, F_u)(gc, GL_LESS);
    FN(t, glEnableClientState, F_u)(gc, GL_VERTEX_ARRAY);
    FN(t, glVertexPointer, F_ptr)(gc, 3, GL_FLOAT, 0, v);
    FN(t, glColor4f, F_4f)(gc, 0, 1, 0, 1);
    zquad(gc, t, v, 0, 0, S, S, 0.0f);                  /* 0.5 */
    FN(t, glColor4f, F_4f)(gc, 1, 0, 0, 1);
    zquad(gc, t, v, 0, 0, S, S, -0.5f);                 /* 0.75: behind */
    FN(t, glColor4f, F_4f)(gc, 0, 0, 1, 1);
    zquad(gc, t, v, 0, 0, S / 2, S, 0.5f);              /* 0.25: in front */
    FN(t, glDepthFunc, F_u)(gc, GL_GREATER);
    FN(t, glColor4f, F_4f)(gc, 1, 1, 0, 1);
    zquad(gc, t, v, 0, S / 2, S, S, -0.8f);             /* 0.9, GL_GREATER: shown */
    FN(t, glDisable, F_u)(gc, GL_DEPTH_TEST);
    FN(t, glDepthFunc, F_u)(gc, GL_LESS);
    FN(t, glReadPixels, F_read)(gc, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, out);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, 0);
    FN(t, glDeleteFramebuffersOES, F_del)(gc, 1, &fb);
    FN(t, glDeleteRenderbuffersOES, F_del)(gc, 1, &rb);
    FN(t, glDeleteTextures, F_del)(gc, 1, &tex);
    return FN(t, glGetError, F_err)(gc) == GL_NO_ERROR;
}

/* Draw scene `which` (0 = CA-like, 1 = depth probe) on the CPU rasterizer
   and on the PICA200; the number of pixels that differ by more than 24 in
   a channel, -1 on a GL error. */
int gl_scene_diff(int which, int *maxd)
{
    static void *tab[2][GLI_NSLOTS];
    static uint8_t img[2][S * S * 4];
    const Backend *be[2] = { &sw_backend, &pica_backend };
    int i, ok = 1, edge = 0, max = 0;
    for (i = 0; i < 2 && ok; i++) {
        Share *sh = gl_share_new();
        GC *gc = sh ? gl_gc_new(sh) : NULL;
        if (!gc) {
            ok = 0;
            break;
        }
        gc->be = be[i];
        gl_install_dispatch_native(gc, tab[i]);
        ok = which ? depth_scene(gc, tab[i], img[i]) : scene(gc, tab[i], img[i]);
        gl_gc_free(gc);
        gl_share_release(sh);
    }
    if (!ok)
        return -1;
    for (i = 0; i < S * S; i++) {
        int k, d = 0;
        for (k = 0; k < 4; k++) {
            int e = abs(img[0][i * 4 + k] - img[1][i * 4 + k]);
            d = e > d ? e : d;
        }
        max = d > max ? d : max;
        if (d > 24)
            edge++;                     /* polygon edges: the GPU's fill rule differs slightly */
    }
    *maxd = max;
    return edge;
}

int ca_scene_check(void)
{
    int max = 0, edge = gl_scene_diff(0, &max);
    if (edge < 0) {
        gl_log("CA scene check: GL error while drawing");
        return 0;
    }
    gl_log("CA scene check: GPU vs CPU rasterizer: %d of %d pixels differ by more than 24 (max %d)%s",
           edge, S * S, max, edge > S * S / 100 ? ": too many" : "");
    return edge <= S * S / 100;         /* 1 %: hardware run 15 had 0, QEMU's model ~30 */
}

/* ------------------------------------------------------------------ */
/* per-frame cost of the render thread (log every 30 s)                */
/* ------------------------------------------------------------------ */

#ifndef GL_HOST
typedef unsigned int mach_port_t;
extern mach_port_t mach_thread_self(void);
extern int thread_info(mach_port_t, int, int *, unsigned *);
extern int mach_port_deallocate(mach_port_t, mach_port_t);
extern mach_port_t mach_task_self_;
static uint64_t thread_cpu_us(void)
{
    struct { int us, uus, ss, sus, rest[6]; } bi;
    unsigned n = 10;
    mach_port_t t = mach_thread_self();
    uint64_t r = 0;
    if (thread_info(t, 3 /* THREAD_BASIC_INFO */, (int *)&bi, &n) == 0)
        r = (uint64_t)(bi.us + bi.ss) * 1000000u + (uint64_t)(bi.uus + bi.sus);
    mach_port_deallocate(mach_task_self_, t);
    return r;
}
#else
static uint64_t thread_cpu_us(void) { return 0; }
#endif

/* every CA swap (iface.c SwapNotification); a line every 300 frames.
   IOS3DS_CA_LOG_FRAMES=N (test hook): use N instead, e.g. to see the cost
   before and after a scattered surface within a short QEMU run. */
static unsigned ca_period(void)
{
    static int period = -1;
    if (period < 0) {
        const char *e = getenv("IOS3DS_CA_LOG_FRAMES");
        int n = e ? atoi(e) : 300;
        period = n >= 2 && n <= 100000 ? n : 300;
    }
    return (unsigned)period;
}

/* ------------------------------------------------------------------ */
/* [perf] frame-time histogram (the hardware side of emu/perf.py).     */
/*                                                                     */
/* Every CA swap is one frame. ca_frame() runs on the render thread    */
/* right after the swap's GPU work (flush_all -> present) finished, so */
/* the interval between two calls is the wall time of one frame:       */
/* vsync pacing when the compositor keeps up, longer when the CPU or   */
/* the GPU (its own work or the display mirror's, which the shim        */
/* arbitrates) made it miss a refresh. Once per second while frames    */
/* flow - i.e. during a folder open/close, a scroll or a page load,    */
/* and not at all on an idle home screen - one [perf] line prints the  */
/* frame-time histogram, so a hardware log shows exactly where a given */
/* animation drops frames.                                             */
/* ------------------------------------------------------------------ */
static uint64_t perf_now_us(void)
{
#ifndef GL_HOST
    static mach_timebase_info_data_t tb;
    static int have_tb;
    if (!have_tb) {
        mach_timebase_info(&tb);
        have_tb = 1;
    }
    return mach_absolute_time() / 1000u * tb.numer / tb.denom;
#else
    return 0;
#endif
}

/* log a "[perf] ..." line straight to /dev/console (not "[gl] ...") */
static void perf_log(const char *fmt, ...)
{
    static int fd = -2;
    char buf[256];
    va_list ap;
    int n = snprintf(buf, sizeof buf, "[perf] ");
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n++] = '\n';
#ifndef GL_HOST
    if (fd == -2)
        fd = open("/dev/console", 0x20001 /* O_WRONLY | O_NOCTTY */);
    if (fd < 0)
        fd = 2;
#else
    if (fd == -2)
        fd = 2;
#endif
    write(fd, buf, n);
}

static void perf_frame_tick(void)
{
    static uint64_t last_us, sec_us;
    static unsigned n, hist[5], sum_ms, min_ms, max_ms;
    uint64_t now = perf_now_us(), dt;
    if (!last_us) {
        last_us = sec_us = now;
        return;
    }
    dt = now - last_us;
    last_us = now;
    /* a gap longer than a second: the app was idle. This frame is the first
       of a new animation; its "interval" is the idle gap, not a frame time,
       so start a fresh window and do not record it. */
    if (dt > 1000000u) {
        sec_us = now;
        n = 0;
        memset(hist, 0, sizeof hist);
        sum_ms = min_ms = max_ms = 0;
        return;
    }
    if (!n) {
        min_ms = max_ms = (unsigned)(dt / 1000u);
    } else {
        if (dt / 1000u < min_ms)
            min_ms = (unsigned)(dt / 1000u);
        if (dt / 1000u > max_ms)
            max_ms = (unsigned)(dt / 1000u);
    }
    sum_ms += (unsigned)(dt / 1000u);
    n++;
    {
        unsigned ms = (unsigned)(dt / 1000u);
        if (ms < 17)
            hist[0]++;
        else if (ms < 33)
            hist[1]++;
        else if (ms < 66)
            hist[2]++;
        else if (ms < 100)
            hist[3]++;
        else
            hist[4]++;
    }
    if (now - sec_us >= 1000000u) {
        if (n >= 2)
            perf_log("CA: %u frames in %u ms; frame ms min/avg/max %u/%u/%u; "
                     "<17:%u 17-33:%u 33-66:%u 66-100:%u >=100:%u",
                     n, (unsigned)((now - sec_us) / 1000u), min_ms, n ? sum_ms / n : 0, max_ms,
                     hist[0], hist[1], hist[2], hist[3], hist[4]);
        sec_us = now;
        n = 0;
        memset(hist, 0, sizeof hist);
        sum_ms = 0;
    }
}

void ca_frame(void)
{
    static uint64_t cpu0, frames;
    static PicaStats s0;
    PicaStats s;
    uint64_t cpu, dcpu;
    unsigned period;
    if (!getenv("IOS3DS_CA"))
        return;
    perf_frame_tick();
    period = ca_period();
    if (!frames++) {
        cpu0 = thread_cpu_us();
        pica_backend_stats2(&s0);
        return;
    }
    if (frames % (period + 1) != 1)
        return;
    /* measure the CPU time only at the reporting frames: the thread_info
       Mach call costs a few syscalls per swap and its result is used once
       per `period` frames (the render thread is the hot path). */
    cpu = thread_cpu_us();
    pica_backend_stats2(&s);
    /* a new render thread starts its CPU clock at 0: never subtract backwards */
    dcpu = cpu >= cpu0 ? cpu - cpu0 : 0;
    if (stereo_enabled()) {
        unsigned f, h, fl, stp, wp;
        stereo_stats(&f, &h, &fl, &stp, &wp);
        gl_log("CA stereo3D: %u frames, %u home (stamped), %u flat (wiped), %u stamp passes, %u wipes",
               f, h, fl, stp, wp);
    }
    gl_log("CA: %llu frames; per frame over the last %u: render thread CPU %llu us, %u.%u command lists, "
           "%u.%u GPU presents (%u.%u via an arena copy), %u.%u CPU read-backs (%u KiB), %u.%u texture uploads (%u KiB); "
           "GPU memory: %u KiB held (%u textures, %u KiB), arena %u KiB free, %u evictions%s",
           (unsigned long long)frames - 1, period,
           (unsigned long long)(dcpu / period), (s.lists - s0.lists) / period, (s.lists - s0.lists) * 10 / period % 10,
           (s.presents - s0.presents) / period, (s.presents - s0.presents) * 10 / period % 10,
           (s.bounces - s0.bounces) / period, (s.bounces - s0.bounces) * 10 / period % 10,
           (s.readbacks - s0.readbacks) / period, (s.readbacks - s0.readbacks) * 10 / period % 10,
           (s.readback_kb - s0.readback_kb) / period, (s.uploads - s0.uploads) / period,
           (s.uploads - s0.uploads) * 10 / period % 10, (s.upload_kb - s0.upload_kb) / period, s.arena_kb, s.ntex,
           s.tex_kb, s.arena_free_kb, s.evictions, s.gpu_on ? "" : " (GPU off: CPU rasterizer)");
    cpu0 = cpu;
    s0 = s;
}
