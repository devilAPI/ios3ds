/* ios3ds GL engine on iPhone OS 2.x (5H11a): CoreAnimation's compositor on
 * the PICA200 without a GL engine bundle.
 *
 * 2.x OpenGLES.framework *is* the IMG MBX driver (no MBXGLEngine.bundle to
 * replace, re-log 135). QuartzCore's render server (CADisplayH1Server,
 * SpringBoard) talks to it through plain EGL + GLES 1.1 calls:
 *   gles_get_config:   eglEnableInternalSurface(1), eglGetDisplay(0),
 *                      eglInitialize, eglChooseConfig({SURFACE_TYPE, PIXMAP}),
 *                      eglGetConfigAttrib(R,G,B,A,DEPTH,STENCIL,SAMPLES,
 *                      SAMPLE_BUFFERS) -> a config for 'BGRA' (8888, no depth)
 *   new_gles_context:  eglCreateContext(dpy, cfg, 0, 0)
 *   make_buffer_current / create_surface:
 *                      eglCreatePixmapSurface(dpy, cfg, CoreSurfaceBufferRef, 0),
 *                      eglMakeCurrent(dpy, s, s, ctx)
 *   create_surface / bind_image_impl:
 *                      glTexImageCoreSurfaceAPPLE(target, CoreSurfaceBufferRef)
 *   finish_image:      glFinishTextureAPPLE(target)
 * (read from 5H11a QuartzCore with llvm-objdump; interface facts only).
 *
 * This dylib is inserted into SpringBoard only (DYLD_INSERT_LIBRARIES, opt-in
 * build IOS3DS_GPU221=1) and interposes exactly those EGL/GL imports, backed
 * by the ios3ds GL core (api.c, draw.c, pica_be.c). A pixmap surface is a
 * CoreSurface buffer drawn in place (32 bpp BGRA, GL row 0 = last memory
 * row, like a 4.x CAEAGLLayer buffer); the PICA200 back end keeps a GPU copy
 * and writes it back (direct present / arena copy / read-back) when CA
 * flushes, switches surfaces or samples it.
 *
 * eglInitialize runs the same gate as the 4.x engine (boot.cfg ca_gpu/gpu,
 * the back end's self-check, the CA scene check). If it fails, CA gets no
 * EGL display and keeps its software compositor (CA_ENABLE_OGL only tells
 * it to try).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>

#include "../engine.h"
#include "../gl_slots.h"

typedef const void *CFTypeRef;
extern CFTypeRef CFRetain(CFTypeRef);
extern void CFRelease(CFTypeRef);

/* --------------------------------------------------- EGL interface facts */
typedef int EGLint;
typedef unsigned EGLBoolean;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
#define EGL_SUCCESS          0x3000
#define EGL_BAD_SURFACE      0x300D
#define EGL_BAD_MATCH        0x3009
#define EGL_ALPHA_SIZE       0x3021
#define EGL_BLUE_SIZE        0x3022
#define EGL_GREEN_SIZE       0x3023
#define EGL_RED_SIZE         0x3024
#define EGL_BUFFER_SIZE      0x3020
#define EGL_SURFACE_TYPE     0x3033
#define EGL_PIXMAP_BIT       0x0002
#define EGL_WINDOW_BIT       0x0004
#define EGL_CONFIG_ID        0x3028

#define FOURCC_BGRA 0x42475241u
#define FOURCC_ARGB 0x41524742u
#define FOURCC_L565 0x4C353635u
#define FOURCC_L555 0x4C353535u

static EGLint egl_error = EGL_SUCCESS;
static char the_display, the_config;

/* -------------------------------------------------- CoreSurface (dlsym) */
typedef void *CSBuf;
static struct {
    int done;
    void *(*base)(CSBuf);
    unsigned (*width)(CSBuf), (*height)(CSBuf), (*bpr)(CSBuf), (*fmt)(CSBuf);
    int (*lock)(CSBuf, unsigned);
    int (*unlock)(CSBuf);
} cs;

static void cs_init(void)
{
    void *h;
    if (cs.done)
        return;
    cs.done = 1;
    h = dlopen("/System/Library/PrivateFrameworks/CoreSurface.framework/CoreSurface", RTLD_LAZY);
    if (!h) {
        gl_log("ios2: CoreSurface not found");
        return;
    }
    cs.base = (void *(*)(CSBuf))dlsym(h, "CoreSurfaceBufferGetBaseAddress");
    cs.width = (unsigned (*)(CSBuf))dlsym(h, "CoreSurfaceBufferGetWidth");
    cs.height = (unsigned (*)(CSBuf))dlsym(h, "CoreSurfaceBufferGetHeight");
    cs.bpr = (unsigned (*)(CSBuf))dlsym(h, "CoreSurfaceBufferGetBytesPerRow");
    cs.fmt = (unsigned (*)(CSBuf))dlsym(h, "CoreSurfaceBufferGetPixelFormatType");
    cs.lock = (int (*)(CSBuf, unsigned))dlsym(h, "CoreSurfaceBufferLock");
    cs.unlock = (int (*)(CSBuf))dlsym(h, "CoreSurfaceBufferUnlock");
    if (!cs.base || !cs.width || !cs.height || !cs.bpr || !cs.fmt)
        gl_log("ios2: CoreSurface functions missing");
}

static void *cs_map(CSBuf b, uint32_t *fmt, int *w, int *h, int *bpr)
{
    void *p;
    cs_init();
    if (!b || !cs.base)
        return NULL;
    *fmt = cs.fmt(b);
    p = cs.base(b);
    if (!p && cs.lock) {            /* not mapped yet: a lock maps it */
        cs.lock(b, 3);
        p = cs.base(b);
        if (cs.unlock)
            cs.unlock(b);
    }
    *w = (int)cs.width(b);
    *h = (int)cs.height(b);
    *bpr = (int)cs.bpr(b);
    return p;
}

static void log_fmt(uint32_t f, const char *what)
{
    gl_log("ios2: CoreSurface format '%c%c%c%c' %s", f >> 24, f >> 16 & 0xff, f >> 8 & 0xff, f & 0xff, what);
}

/* --------------------------------------------------------- pixmap surfaces */
struct Drawable { int unused; };         /* marks rb as "surface memory" (rt->surface) */

typedef struct Surf {
    CSBuf buf;
    Rb rb;
    struct Drawable dr;
    unsigned seed;                      /* bumped whenever the GPU may have drawn into it */
    struct Surf *next;
} Surf;

static Surf *surfs;
static GC *cur;
static Surf *cur_surf;
static void *tab[GLI_NSLOTS];
static int tab_ready;
static unsigned nsurf, nctx, nframes;

int drawable_begin(GC *gc, Rb *rb) { (void)gc; return rb->color != NULL; }
void drawable_detach(Rb *rb) { rb->dr = NULL; }

void tex_release_surface(Tex *t)
{
    if (t->surface)
        CFRelease(t->surface);
    t->surface = NULL;
}

/* A surface texture's seed: our own counter of GPU writes into that buffer
   (CoreSurface has none); CPU writes (CoreGraphics layer contents) are still
   caught by the back end's texel sample. */
int tex_surface_seed(const Tex *t, uint32_t *seed)
{
    Surf *s;
    for (s = surfs; s; s = s->next)
        if (s->rb.color == t->px) {
            *seed = s->seed;
            return 1;
        }
    return 0;
}

static void surf_rt(Surf *s, RT *rt)
{
    memset(rt, 0, sizeof *rt);
    rt->w = s->rb.w; rt->h = s->rb.h; rt->pitch = s->rb.pitch; rt->color = s->rb.color;
    rt->flip = s->rb.flip; rt->surface = 1;
}

/* the GPU's copy of s must be in memory before anything else reads it */
static void surf_sync(GC *gc, Surf *s)
{
    RT rt;
    if (!gc || !s || !s->rb.color)
        return;
    surf_rt(s, &rt);
    if (gc->be->finish)
        gc->be->finish(gc, &rt);
    s->seed++;
}

/* ------------------------------------------------------ stall watchdog */
/* A render-server stall is invisible in QEMU (no crash, no report). Every
   interposed call records where it is; a watchdog thread logs one line when
   the render thread has been inside one of our calls, or has not called us,
   for more than 4 s while frames were flowing before. */
static volatile const char *wd_op = "none";
static volatile int wd_in;
static volatile uint64_t wd_t;
static uint64_t wd_now_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return mach_absolute_time() / 1000000u * tb.numer / tb.denom;
}
#define WD_IN(name) do { wd_op = name; wd_in = 1; wd_t = wd_now_ms(); } while (0)
#define WD_OUT() do { wd_in = 0; wd_t = wd_now_ms(); } while (0)
static void *wd_main(void *arg)
{
    unsigned said = 0;
    const char *ab = getenv("IOS3DS_GL_WATCHDOG_ABORT");
    (void)arg;
    for (;;) {
        uint64_t now, t;
        int idle_ab;
        sleep(2);
        now = wd_now_ms();
        t = wd_t;
        if (!t)
            continue;
        if (now - t <= 4000) {
            said = 0;
            continue;
        }
        if (wd_in && said++ < 10)
            gl_log("ios2: watchdog: render thread inside %s for %u ms", (const char *)wd_op, (unsigned)(now - t));
        /* "idle:N": also abort when the render thread has not called us for
           40 s once the system is N s up (a stall outside the engine) */
        idle_ab = ab && !strncmp(ab, "idle:", 5) && now / 1000u > (uint64_t)atoi(ab + 5) && now - t > 40000;
        if (ab && ((wd_in && now - t > 20000) || idle_ab)) {
            gl_log("ios2: watchdog: aborting for a crash report (all thread stacks)");
            __builtin_trap();     /* no abort() in the CI stub SDK: a trap gives the same report */
        }
    }
    return NULL;
}
static void wd_start(void)
{
    static int started;
    pthread_t th;
    if (started++ || !getenv("IOS3DS_GL_WATCHDOG"))
        return;
    pthread_create(&th, NULL, wd_main, NULL);
}

/* --------------------------------------------------------------- EGL */
static EGLBoolean my_eglEnableInternalSurface(EGLint on) { (void)on; return 1; }
static EGLDisplay my_eglGetDisplay(void *native) { (void)native; return &the_display; }
static EGLint my_eglGetError(void) { EGLint e = egl_error; egl_error = EGL_SUCCESS; return e; }

static EGLBoolean my_eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min)
{
    static int said;
    if (d != &the_display)
        return 0;
    if (!pica_ca_gate()) {              /* CA keeps its software compositor */
        if (!said++)
            gl_log("ios2: no EGL display for CoreAnimation: software compositor");
        return 0;
    }
    if (maj)
        *maj = 1;
    if (min)
        *min = 1;
    if (!said++)
        gl_log("ios2: EGL display up for CoreAnimation (pid %d)", getpid());
    return 1;
}

static EGLBoolean my_eglChooseConfig(EGLDisplay d, const EGLint *attr, EGLConfig *out, EGLint size, EGLint *num)
{
    (void)d; (void)attr;
    if (num)
        *num = 1;
    if (out && size > 0)
        out[0] = &the_config;
    return 1;
}

static EGLBoolean my_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v)
{
    (void)d; (void)c;
    if (!v)
        return 0;
    switch (a) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: *v = 8; break;
    case EGL_BUFFER_SIZE: *v = 32; break;
    case EGL_SURFACE_TYPE: *v = EGL_PIXMAP_BIT | EGL_WINDOW_BIT; break;
    case EGL_CONFIG_ID: *v = 1; break;
    default: *v = 0; break;             /* depth, stencil, samples, ... */
    }
    return 1;
}

static EGLContext my_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attr)
{
    Share *sh;
    GC *gc;
    (void)d; (void)c; (void)share; (void)attr;
    stereo_init();
    if (!tab_ready) {
        GC *tmp;
        sh = gl_share_new();
        tmp = sh ? gl_gc_new(sh) : NULL;
        if (!tmp)
            return NULL;
        gl_install_dispatch_native(tmp, tab);
        gl_gc_free(tmp);
        gl_share_release(sh);
        tab_ready = 1;
    }
    sh = gl_share_new();
    gc = sh ? gl_gc_new(sh) : NULL;
    if (sh)
        gl_share_release(sh);           /* the GC holds the only reference */
    if (!gc)
        return NULL;
    wd_start();
    if (!nctx++)
        gl_log("ios2: CoreAnimation context %p uses the %s", (void *)gc, gc->be->name);
    return gc;
}

static EGLBoolean my_eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d;
    if (!c)
        return 0;
    WD_IN("eglDestroyContext");
    if (cur == c) {
        surf_sync(cur, cur_surf);
        cur = NULL;
        cur_surf = NULL;
    }
    gl_gc_free(c);
    WD_OUT();
    return 1;
}

static EGLSurface my_eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, void *pixmap, const EGLint *attr)
{
    uint32_t f = 0;
    int w = 0, h = 0, bpr = 0;
    void *p;
    Surf *s;
    (void)d; (void)c; (void)attr;
    p = cs_map(pixmap, &f, &w, &h, &bpr);
    if (!p || (f != FOURCC_BGRA && f != FOURCC_ARGB) || w <= 0 || h <= 0 || bpr < w * 4) {
        static int said;
        if (!said++)
            log_fmt(f, "pixmap surface not supported (only 32 bpp)");
        egl_error = EGL_BAD_MATCH;
        return NULL;
    }
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->buf = (CSBuf)CFRetain(pixmap);
    s->rb.color = p;
    s->rb.w = w;
    s->rb.h = h;
    s->rb.pitch = bpr / 4;
    s->rb.flip = 1;                     /* GL's bottom row is the buffer's last row */
    s->rb.ifmt = GL_RGBA8;
    s->rb.dr = &s->dr;
    s->next = surfs;
    surfs = s;
    if (!nsurf++)
        gl_log("ios2: first pixmap surface %dx%d (pitch %d) at %p", w, h, bpr, p);
    return s;
}

static EGLBoolean my_eglDestroySurface(EGLDisplay d, EGLSurface sv)
{
    Surf *s = sv, **pp;
    (void)d;
    if (!s)
        return 0;
    WD_IN("eglDestroySurface");
    if (cur_surf == s) {
        surf_sync(cur, s);
        if (cur)
            cur->default_rb = NULL;
        cur_surf = NULL;
    }
    for (pp = &surfs; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    gl_be_forget(s->rb.color);
    CFRelease(s->buf);
    free(s);
    WD_OUT();
    return 1;
}

static EGLBoolean my_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    Surf *s = draw;
    (void)d; (void)read;
    WD_IN("eglMakeCurrent");
    if (cur && cur_surf && (cur_surf != s || cur != ctx))
        surf_sync(cur, cur_surf);       /* textures of this buffer must see the frame */
    if (cur && cur != ctx)
        cur->default_rb = NULL;
    cur = ctx;
    cur_surf = ctx ? s : NULL;
    if (cur) {
        cur->default_rb = s ? &s->rb : NULL;
        cur->default_depth = NULL;
        if (s)
            s->seed++;
    }
    WD_OUT();
    return 1;
}

/* ------------------------------------------------------- GL wrappers */
#define STRIP(...) __VA_ARGS__
#define CALL(name, params, args) ((void (*)(GC *, STRIP params))tab[SLOT_##name])(cur, STRIP args)
#define W_V(name, params, args) \
    static void my_##name params { if (cur) { WD_IN(#name); CALL(name, params, args); WD_OUT(); } }
#define W_V0(name) \
    static void my_##name(void) { if (cur) ((void (*)(GC *))tab[SLOT_##name])(cur); }

#define GL2_VOID(F, F0) \
    F(glActiveTexture, (GLenum t), (t)) \
    F(glBindTexture, (GLenum t, GLuint n), (t, n)) \
    F(glBlendFunc, (GLenum s, GLenum d), (s, d)) \
    F(glClear, (GLbitfield m), (m)) \
    F(glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a), (r, g, b, a)) \
    F(glClientActiveTexture, (GLenum t), (t)) \
    F(glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a)) \
    F(glColor4ub, (GLubyte r, GLubyte g, GLubyte b, GLubyte a), (r, g, b, a)) \
    F(glColorPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(glCullFace, (GLenum m), (m)) \
    F(glDeleteTextures, (GLsizei n, const GLuint *t), (n, t)) \
    F(glDisable, (GLenum c), (c)) \
    F(glDisableClientState, (GLenum c), (c)) \
    F(glDrawArrays, (GLenum m, GLint f, GLsizei n), (m, f, n)) \
    F(glDrawElements, (GLenum m, GLsizei n, GLenum t, const GLvoid *i), (m, n, t, i)) \
    F(glEnable, (GLenum c), (c)) \
    F(glEnableClientState, (GLenum c), (c)) \
    F(glFrontFace, (GLenum m), (m)) \
    F(glGenTextures, (GLsizei n, GLuint *t), (n, t)) \
    F(glGetFloatv, (GLenum p, GLfloat *v), (p, v)) \
    F(glGetIntegerv, (GLenum p, GLint *v), (p, v)) \
    F0(glLoadIdentity) \
    F(glLoadMatrixf, (const GLfloat *m), (m)) \
    F(glMatrixMode, (GLenum m), (m)) \
    F(glOrthof, (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f), (l, r, b, t, n, f)) \
    F0(glPopMatrix) \
    F0(glPushMatrix) \
    F(glRotatef, (GLfloat a, GLfloat x, GLfloat y, GLfloat z), (a, x, y, z)) \
    F(glScalef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
    F(glScissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h)) \
    F(glTexCoordPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(glTexEnvfv, (GLenum t, GLenum p, const GLfloat *v), (t, p, v)) \
    F(glTexEnvi, (GLenum t, GLenum p, GLint v), (t, p, v)) \
    F(glTexImage2D, (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const GLvoid *p), \
      (t, l, i, w, h, b, f, ty, p)) \
    F(glTexParameteri, (GLenum t, GLenum p, GLint v), (t, p, v)) \
    F(glTranslatef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
    F(glVertexPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(glViewport, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))

GL2_VOID(W_V, W_V0)

static GLenum my_glGetError(void)
{
    return cur ? ((GLenum (*)(GC *))tab[SLOT_glGetError])(cur) : 0;
}

static const GLubyte *my_glGetString(GLenum n)
{
    static char ext[1024];
    const GLubyte *r;
    /* 5H11a QuartzCore also asks for 0x1F04 (a second, IMG extension list)
       and hands both answers to strstr: never NULL. Without
       GL_APPLE_texture_rectangle + GL_APPLE_core_surface_texture it logs
       "unsupported graphics hardware" and the render server dies. Both are
       what glTexImageCoreSurfaceAPPLE + 0x84F5 below implement. */
    if (!cur)
        return (const GLubyte *)"";
    if (n == 0x1F04)
        n = GL_EXTENSIONS;
    r = ((const GLubyte *(*)(GC *, GLenum))tab[SLOT_glGetString])(cur, n);
    if (n == GL_EXTENSIONS) {
        if (!ext[0])
            snprintf(ext, sizeof ext, "%s GL_APPLE_texture_rectangle GL_APPLE_core_surface_texture",
                     r ? (const char *)r : "");
        return (const GLubyte *)ext;
    }
    return r ? r : (const GLubyte *)"";
}

/* glFlush/glFinish end every CA update: the target goes back to memory
   (the display buffer before CA swaps it onto the CLCD) */
static void frame_end(void)
{
    if (cur_surf)
        cur_surf->seed++;
    if (cur_surf && cur_surf->rb.w >= 320 && cur_surf->rb.h >= 480) {
        if (!nframes++)
            gl_log("ios2: first CA display frame (%dx%d) on the %s", cur_surf->rb.w, cur_surf->rb.h, cur->be->name);
        ca_frame();
    }
}

static void my_glFlush(void)
{
    if (!cur)
        return;
    WD_IN("glFlush");
    stereo_end_frame(cur);
    ((void (*)(GC *))tab[SLOT_glFlush])(cur);
    frame_end();
    WD_OUT();
}

static void my_glFinish(void)
{
    if (!cur)
        return;
    WD_IN("glFinish");
    stereo_end_frame(cur);
    ((void (*)(GC *))tab[SLOT_glFinish])(cur);
    frame_end();
    WD_OUT();
}

/* 16 bpp CoreSurface images: our 32 bpp copy (a snapshot, as on 4.x) */
static uint32_t *texels16(const unsigned char *p, uint32_t f, int w, int h, int bpr)
{
    uint32_t *out = malloc((size_t)w * h * 4);
    int x, y;
    if (!out)
        return NULL;
    for (y = 0; y < h; y++) {
        const uint16_t *src = (const uint16_t *)(p + (size_t)y * bpr);
        uint32_t *dst = out + (size_t)y * w;
        for (x = 0; x < w; x++) {
            unsigned v = src[x], r, g, b;
            if (f == FOURCC_L555) {
                r = v >> 10 & 31; g = v >> 5 & 31; b = v & 31;
                g = g << 3 | g >> 2;
            } else {
                r = v >> 11; g = v >> 5 & 63; b = v & 31;
                g = g << 2 | g >> 4;
            }
            r = r << 3 | r >> 2;
            b = b << 3 | b >> 2;
            dst[x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
    return out;
}

static void my_glTexImageCoreSurfaceAPPLE(GLenum target, void *buf)
{
    uint32_t f = 0, *px = NULL;
    int w = 0, h = 0, bpr = 0, own = 0, pitch = 0;
    void *p;
    Tex *t;
    static unsigned calls;
    if (!cur)
        return;
    WD_IN("glTexImageCoreSurfaceAPPLE");
    if (!calls++)
        gl_log("ios2: first glTexImageCoreSurfaceAPPLE (target %#x)", target);
    t = gl_bound_tex(cur, cur->active_tex);
    if (!t) {
        gl_set_error(cur, GL_INVALID_OPERATION);
        WD_OUT();
        return;
    }
    p = buf ? cs_map(buf, &f, &w, &h, &bpr) : NULL;
    if (buf && p) {
        if (f == FOURCC_BGRA || f == FOURCC_ARGB) {
            px = p;
            pitch = bpr / 4;
        } else if (f == FOURCC_L565 || f == FOURCC_L555) {
            px = texels16(p, f, w, h, bpr);
            own = 1;
            pitch = w;
            static int said;
            if (!said++)
                log_fmt(f, "texture: converted to 32 bpp at bind");
        } else {
            static int said;
            if (!said++)
                log_fmt(f, "texture not supported");
        }
    }
    gl_tex_drop_image(t);
    if (px) {
        t->surface = own ? NULL : (void *)CFRetain(buf);
        t->px = px;
        t->w = w;
        t->h = h;
        t->pitch = pitch;
        t->base = GL_RGBA;
        t->own_px = own;
    } else if (buf) {
        gl_set_error(cur, GL_INVALID_VALUE);
    }
    gl_tex_dirty(cur, t);
    WD_OUT();
}

static void my_glFinishTextureAPPLE(GLenum target) { (void)target; }

/* CADisplayH1::gpu_swap_token / gpu_flush hand every swap to MBX2D, which
   calls into the (absent) MBX hardware through MBXConnect and faults. Our
   GPU work is synchronous: by the time CA swaps, the present is done. */
static int my_mbx2DSwapNotification(void *a, void *b, void *c)
{
    static int said;
    (void)a; (void)b; (void)c;
    if (!said++)
        gl_log("ios2: first CA swap (mbx2DSwapNotification answered here)");
    WD_IN("CA swap (IOMFB)");
    WD_OUT();
    wd_op = "CA swap (IOMFB)";
    return 0;
}
static int my_mbx2DFlush(void) { return 0; }

/* After a GL frame CADisplayIOMFB::finish_update passes swap flag 1 to
   IOMobileFramebufferSwapSetLayer: "wait for the MBX swap token" that
   mbx2DSwapNotification would have queued behind the GPU work. Nothing
   ever signals it here, so every such swap sat in the kernel until its
   timeout (~5 s per frame in QEMU, and the lock fade never finished). The
   frame is complete in memory when CA swaps; pass 0 as the software path
   does. Arguments: fb, layer, buffer, two CGRects (8 words), flags. */
extern int IOMobileFramebufferSwapSetLayer(void);
extern int IOMobileFramebufferSwapWait(void *fb, uint32_t token, uint32_t flags);

/* CADisplayIOMFB::set_enabled(false) (Lock) ends with SwapEnd + SwapWait
   for its blanking swap. Once CA has composited with GL that wait never
   returned in QEMU (crash report taken by the watchdog: render thread in
   IOMobileFramebufferSwapWait, SpringBoard's main thread waiting for it),
   so the lock never finished. Every GL frame is complete in memory before
   CA swaps, so there is nothing to wait for: return at once. */
static int my_IOMobileFramebufferSwapWait(void *fb, uint32_t token, uint32_t flags)
{
    static int said;
    if (!nframes)
        return IOMobileFramebufferSwapWait(fb, token, flags);
    if (!said++)
        gl_log("ios2: IOMFB swap wait (token %u, flags %#x) answered here", token, flags);
    return 0;
}
static int my_IOMobileFramebufferSwapSetLayer(void *fb, int layer, void *buf, uint32_t a0, uint32_t a1,
                                              uint32_t a2, uint32_t a3, uint32_t b0, uint32_t b1, uint32_t b2,
                                              uint32_t b3, uint32_t flags)
{
    static int said;
    if ((flags & 1) && !said++)
        gl_log("ios2: IOMFB swap flags %#x: GPU-token wait dropped", flags);
    return ((int (*)(void *, int, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t, uint32_t, uint32_t))IOMobileFramebufferSwapSetLayer)(
        fb, layer, buf, a0, a1, a2, a3, b0, b1, b2, b3, cur || nframes ? flags & ~1u : flags);
}
extern void mbx2DSwapNotification(void);
extern void mbx2DFlush(void);

/* ----------------------------------------------------------- interpose */
extern EGLBoolean eglEnableInternalSurface(EGLint);
extern EGLDisplay eglGetDisplay(void *);
extern EGLint eglGetError(void);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
extern EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
extern EGLSurface eglCreatePixmapSurface(EGLDisplay, EGLConfig, void *, const EGLint *);
extern EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
#define DECL_V(name, params, args) extern void name params;
#define DECL_V0(name) extern void name(void);
GL2_VOID(DECL_V, DECL_V0)
extern GLenum glGetError(void);
extern const GLubyte *glGetString(GLenum);
extern void glFlush(void);
extern void glFinish(void);
extern void glTexImageCoreSurfaceAPPLE(GLenum, void *);
extern void glFinishTextureAPPLE(GLenum);

#define IP(name) { (const void *)my_##name, (const void *)name },
#define IP_V(name, params, args) IP(name)
#define IP_V0(name) IP(name)
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *rep, *orig; } interpose[] = {
    IP(eglEnableInternalSurface) IP(eglGetDisplay) IP(eglGetError) IP(eglInitialize) IP(eglChooseConfig)
    IP(eglGetConfigAttrib) IP(eglCreateContext) IP(eglDestroyContext) IP(eglCreatePixmapSurface)
    IP(eglDestroySurface) IP(eglMakeCurrent)
    GL2_VOID(IP_V, IP_V0)
    IP(glGetError) IP(glGetString) IP(glFlush) IP(glFinish)
    IP(glTexImageCoreSurfaceAPPLE) IP(glFinishTextureAPPLE)
    IP(mbx2DSwapNotification) IP(mbx2DFlush) IP(IOMobileFramebufferSwapSetLayer) IP(IOMobileFramebufferSwapWait)
};
