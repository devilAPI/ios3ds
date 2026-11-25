/* ios3ds GL engine: the EAGL side. OpenGLES.framework loads
 * OpenGLES.framework/MBXGLEngine.bundle (when an "AppleMBXDevice" service
 * matches) and calls GLESGetEGLInterface() for an 11-entry table; this file
 * implements that table on top of the GL core (api.c, draw.c, raster.c).
 *
 * Interface facts (read from 8C148's OpenGLES call sites and the stock
 * MBXGLEngine; see docs/gpu.md):
 *   +0x00 CreateSharegroup(void **sg)                  != 0: ok
 *   +0x04 DestroySharegroup(sg)
 *   +0x08 CreateGC(sg, void **front, void **back, void **gc)   != 0: ok
 *         front/back: two 841-slot __GLIFunctionDispatchRec tables
 *   +0x0C DestroyGC(gc)
 *   +0x10 BindCoreSurface(gc, target, IOSurfaceRef)    -[EAGLContext attachImage:toCoreSurface:invertedRender:]
 *         target GL_RENDERBUFFER (inverted render) or 0x84F5 (texture)
 *   +0x14 BindView(gc, drawable, ifmt, retained)       -[EAGLContext renderbufferStorage:fromDrawable:]
 *   +0x18 FinishTexture                                (OpenGLES never calls it)
 *   +0x1C PresentView(gc)                              -[EAGLContext presentRenderbuffer:]
 *   +0x20 SwapNotification(gc, fb_id, transaction, layer)  CA's GL renderer
 *   +0x24 SetProperty(gc, pname, const void *)         -[EAGLContext setParameter:to:]
 *   +0x28 GetProperty(gc, pname, void *)               -[EAGLContext getParameter:to:]
 *
 * The drawable OpenGLES passes to BindView is QuartzCore's: a block of
 * function pointers called with the drawable itself as first argument:
 *   +0x04 bind(drawable, fourcc, CallbackBlock *)     != 0: ok
 *   +0x08 unbind(drawable)
 *   +0x0C dequeue(drawable) -> IOSurfaceRef           the buffer to draw the next frame into
 *   +0x10 present(drawable, 1)                         != 0: ok
 * CallbackBlock { ctx, create(ctx, IOSurfaceRef) != 0 ok, destroy(ctx, IOSurfaceRef) }
 * is how CA tells the engine about each buffer it allocates for the layer.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"

#define FOURCC_BGRA 0x42475241u      /* 'BGRA': 32 bpp, B G R A in memory = 0xAARRGGBB words */
#define TARGET_SURFACE_TEX 0x84F5u   /* attachImage: target meaning "the bound texture" */

typedef const void *CFTypeRef;
typedef void *IOSurfaceRef;
extern CFTypeRef CFRetain(CFTypeRef);
extern void CFRelease(CFTypeRef);

/* ---------------------------------------------------- dynamic imports */
static struct {
    int done;
    int (*lock)(IOSurfaceRef, uint32_t, uint32_t *);
    int (*unlock)(IOSurfaceRef, uint32_t, uint32_t *);
    void *(*base)(IOSurfaceRef);
    size_t (*width)(IOSurfaceRef), (*height)(IOSurfaceRef), (*bpr)(IOSurfaceRef);
    uint32_t (*fmt)(IOSurfaceRef);
    uint32_t (*seed)(IOSurfaceRef);
    int (*fb_main)(void **);
    int (*fb_swap_signal)(void *, uint32_t, void *);
} io;

static void io_init(void)
{
    void *h, *f;
    if (io.done)
        return;
    io.done = 1;
    h = dlopen("/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface", RTLD_LAZY);
    f = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_LAZY);
    if (h) {
        io.lock = (int (*)(IOSurfaceRef, uint32_t, uint32_t *))dlsym(h, "IOSurfaceLock");
        io.unlock = (int (*)(IOSurfaceRef, uint32_t, uint32_t *))dlsym(h, "IOSurfaceUnlock");
        io.base = (void *(*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetBaseAddress");
        io.width = (size_t (*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetWidth");
        io.height = (size_t (*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetHeight");
        io.bpr = (size_t (*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetBytesPerRow");
        io.fmt = (uint32_t (*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetPixelFormat");
        io.seed = (uint32_t (*)(IOSurfaceRef))dlsym(h, "IOSurfaceGetSeed");
    }
    if (f) {
        io.fb_main = (int (*)(void **))dlsym(f, "IOMobileFramebufferGetMainDisplay");
        io.fb_swap_signal = (int (*)(void *, uint32_t, void *))dlsym(f, "IOMobileFramebufferSwapSignal");
    }
    if (!io.base || !io.width || !io.height || !io.bpr)
        gl_log("IOSurface functions missing");
}

/* Map a surface: its base address, size, bytes per row and pixel format. */
static void *surface_map(IOSurfaceRef s, uint32_t *fmt, int *w, int *h, int *bpr)
{
    void *p;
    io_init();
    if (!s || !io.base)
        return NULL;
    *fmt = io.fmt ? io.fmt(s) : 0;
    p = io.base(s);
    if (!p && io.lock) {                /* not mapped yet: a lock maps it */
        io.lock(s, 0, NULL);
        p = io.base(s);
        io.unlock(s, 0, NULL);
    }
    *w = (int)io.width(s);
    *h = (int)io.height(s);
    *bpr = (int)io.bpr(s);
    return p;
}

static void log_format(uint32_t f, const char *what)
{
    gl_log("IOSurface pixel format '%c%c%c%c' %s", f >> 24, f >> 16 & 0xff, f >> 8 & 0xff, f & 0xff, what);
}

/* Pixels of a surface we can draw into or sample in place: 32 bpp only. */
static uint32_t *surface_pixels(IOSurfaceRef s, int *w, int *h, int *pitch)
{
    uint32_t f;
    int bpr;
    void *p = surface_map(s, &f, w, h, &bpr);
    if (p && f != FOURCC_BGRA && f != 0) {
        log_format(f, "not supported");
        return NULL;
    }
    *pitch = bpr / 4;
    return p;
}

/* 16 bpp surfaces CA samples as textures: PhotoLibrary's album rows (Settings >
 * Wallpaper, Photos; MusicLibrary's 'L555' thumbnail formats from
 * Checkpoint.xml's ImageSpecifications) are IOSurface-backed CGImages in
 * 'L555' (x1 R5 G5 B5, little endian). CA's software compositor reads them
 * directly; we convert them to our 32 bpp texels when CA binds the surface
 * (re-log 62). The copy is a snapshot: CA binds an image's surface again when
 * it makes a new texture for it. */
#define FOURCC_L555 0x4C353535u
#define FOURCC_L565 0x4C353635u
static uint32_t *surface_texels16(IOSurfaceRef s, int *w, int *h)
{
    static int said555, said565;
    uint32_t f, *out;
    int bpr, x, y;
    const unsigned char *p = surface_map(s, &f, w, h, &bpr);
    if (!p || (f != FOURCC_L555 && f != FOURCC_L565) || *w <= 0 || *h <= 0 || bpr < *w * 2)
        return NULL;
    out = malloc((size_t)*w * *h * 4);
    if (!out)
        return NULL;
    if (f == FOURCC_L555 ? !said555++ : !said565++)
        log_format(f, "texture: converted to 32 bpp at bind");
    for (y = 0; y < *h; y++) {
        const uint16_t *src = (const uint16_t *)(p + (size_t)y * bpr);
        uint32_t *dst = out + (size_t)y * *w;
        for (x = 0; x < *w; x++) {
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

/* ------------------------------------------------------------ drawables */
typedef struct CallbackBlock {
    void *ctx;
    int (*create)(void *ctx, IOSurfaceRef s);
    void (*destroy)(void *ctx, IOSurfaceRef s);
} CallbackBlock;

typedef int (*ca_bind_fn)(void *, uint32_t, CallbackBlock *);
typedef void (*ca_unbind_fn)(void *);
typedef IOSurfaceRef (*ca_dequeue_fn)(void *);
typedef int (*ca_present_fn)(void *, int);
#define CA_FN(d, off) (*(void **)((char *)(d) + (off)))

#define MAX_BUFS 8
struct Drawable {
    void *ca;                   /* QuartzCore drawable; NULL: a fixed IOSurface (BindCoreSurface) */
    CallbackBlock cb;           /* handed to ca->bind; must stay put */
    IOSurfaceRef bufs[MAX_BUFS];/* surfaces CA told us about (retained) */
    IOSurfaceRef cur, prev;     /* drawing into / presented last */
    int need_dequeue, began, retained;
    unsigned presents;
};

static int cb_create(void *ctx, IOSurfaceRef s)
{
    struct Drawable *d = ctx;
    int i;
    for (i = 0; i < MAX_BUFS; i++)
        if (!d->bufs[i]) {
            d->bufs[i] = (IOSurfaceRef)CFRetain(s);
            return 1;
        }
    gl_log("drawable: more than %d buffers", MAX_BUFS);
    return 0;
}

static void cb_destroy(void *ctx, IOSurfaceRef s)
{
    struct Drawable *d = ctx;
    int i;
    for (i = 0; i < MAX_BUFS; i++)
        if (d->bufs[i] == s) {
            d->bufs[i] = NULL;
            if (d->cur == s)
                d->cur = NULL, d->need_dequeue = 1;
            if (d->prev == s)
                d->prev = NULL;
            CFRelease(s);
        }
}

/* Called by gl_get_rt() before anything draws into a drawable-backed
 * renderbuffer: make rb->color the buffer of the current frame. */
int drawable_begin(GC *gc, Rb *rb)
{
    struct Drawable *d = rb->dr;
    int w, h, pitch;
    uint32_t *px;
    (void)gc;
    if (d->ca && (d->need_dequeue || !d->cur)) {
        d->cur = ((ca_dequeue_fn)CA_FN(d->ca, 0xC))(d->ca);
        if (!d->cur) {
            gl_log("drawable: dequeue gave no buffer");
            return 0;
        }
        d->need_dequeue = 0;
        d->began = 1;
        px = surface_pixels(d->cur, &w, &h, &pitch);
        if (px && d->retained && d->prev && d->prev != d->cur) {
            int pw, ph, pp, y;              /* retained backing: carry the last frame over */
            uint32_t *src = surface_pixels(d->prev, &pw, &ph, &pp);
            if (src && pw == w && ph == h)
                for (y = 0; y < h; y++)
                    memcpy(px + (size_t)y * pitch, src + (size_t)y * pp, (size_t)w * 4);
        }
    } else {
        px = surface_pixels(d->cur, &w, &h, &pitch);
    }
    if (!px)
        return 0;
    rb->color = px;
    rb->w = w;
    rb->h = h;
    rb->pitch = pitch;
    rb->flip = 1;               /* GL's bottom row is the surface's last row */
    rb->own_color = 0;
    return 1;
}

void drawable_detach(Rb *rb)
{
    struct Drawable *d = rb->dr;
    int i;
    rb->dr = NULL;
    rb->color = NULL;
    if (!d)
        return;
    if (d->ca)
        ((ca_unbind_fn)CA_FN(d->ca, 0x8))(d->ca);
    else if (d->cur)
        CFRelease(d->cur);
    for (i = 0; i < MAX_BUFS; i++)
        if (d->bufs[i])
            CFRelease(d->bufs[i]);
    free(d);
}

/* The surface's modification seed (bumped by every write unlock), if this
   IOSurface has one; 0 = unknown. */
int tex_surface_seed(const Tex *t, uint32_t *seed)
{
    io_init();
    if (!t->surface || !io.seed)
        return 0;
    *seed = io.seed((IOSurfaceRef)t->surface);
    return 1;
}

void tex_release_surface(Tex *t)
{
    if (t->surface)
        CFRelease(t->surface);
    t->surface = NULL;
}

/* ----------------------------------------------------------- interface */
/* Log the caller chain (r7 frame records) once per event kind, to see which
 * framework drives the engine in a process. */
static void log_callers(const char *what)
{
    void **fp = __builtin_frame_address(0);
    char buf[160];
    int n = 0, i;
    buf[0] = 0;
    for (i = 0; i < 8 && fp && ((uintptr_t)fp & 3) == 0; i++) {
        n += snprintf(buf + n, sizeof buf - n, " %p", fp[1]);
        if (n >= (int)sizeof buf)
            break;
        fp = (void **)fp[0];
    }
    gl_log("%s (pid %d) from%s", what, getpid(), buf);
}

static int glesCreateSharegroup(void **out)
{
    Share *sh = gl_share_new();
    *out = sh;
    return sh != NULL;
}

static void glesDestroySharegroup(void *sg)
{
    if (sg)
        gl_share_release(sg);
}

static int glesCreateGC(void *sg, void **front, void **back, void **out)
{
    GC *gc;
    static int said;
    if (!sg)
        return 0;
#ifdef GL_PICA
    if (getenv("IOS3DS_CA") && !pica_ca_gate())
        return 0;                       /* QuartzCore falls back to its software compositor */
#endif
    stereo_init();
    gc = gl_gc_new(sg);
    if (!gc)
        return 0;
    gc->disp_front = front;
    gc->disp_back = back;
    if (front)
        gl_install_dispatch(gc, front);
    if (back)
        gl_install_dispatch(gc, back);
    *out = gc;
    gl_log("context %p uses the %s", (void *)gc, gc->be->name);
    if (!said++)
        log_callers("context created");
    return 1;
}

static void glesDestroyGC(void *gc)
{
    if (gc)
        gl_gc_free(gc);
}

static int glesBindCoreSurface(void *g, GLenum target, IOSurfaceRef s)
{
    GC *gc = g;
    int w, h, pitch;
    uint32_t *px = NULL;
    static unsigned calls;
    int own = 0;
    if (!calls++)
        log_callers("first BindCoreSurface");
    if (s && target != GL_RENDERBUFFER && (px = surface_texels16(s, &w, &h)) != NULL) {
        own = 1;                        /* a 16 bpp image: our 32 bpp copy */
        pitch = w;
    } else if (s && !(px = surface_pixels(s, &w, &h, &pitch))) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return 0;
    }
    if (target == GL_RENDERBUFFER) {
        Rb *rb = gl_rb_lookup(gc, gc->rb);
        struct Drawable *d;
        if (!rb) {
            gl_set_error(gc, GL_INVALID_OPERATION);
            return 0;
        }
        gl_rb_drop_storage(rb);
        if (!s)
            return 1;
        d = calloc(1, sizeof *d);
        if (!d) {
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return 0;
        }
        d->cur = (IOSurfaceRef)CFRetain(s);
        rb->dr = d;
        rb->ifmt = GL_RGBA8;
        return drawable_begin(gc, rb);
    }
    if (target == TARGET_SURFACE_TEX || target == GL_TEXTURE_2D) {
        Tex *t = gl_bound_tex(gc, gc->active_tex);
        if (!t) {
            if (own)
                free(px);
            gl_set_error(gc, GL_INVALID_OPERATION);
            return 0;
        }
        gl_tex_drop_image(t);
        if (s) {
            t->surface = own ? NULL : (void *)CFRetain(s);
            t->px = px;
            t->w = w;
            t->h = h;
            t->pitch = pitch;
            t->base = GL_RGBA;
            t->own_px = own;
        }
        gl_tex_dirty(gc, t);
        return 1;
    }
    if (own)
        free(px);
    gl_set_error(gc, GL_INVALID_ENUM);
    return 0;
}

static int glesBindView(void *g, void *drawable, GLenum ifmt, int retained)
{
    GC *gc = g;
    Rb *rb = gl_rb_lookup(gc, gc->rb);
    struct Drawable *d;
    if (!rb) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return 0;
    }
    switch (ifmt) {
    case 0: ifmt = GL_RGBA8; break;
    case GL_RGB8: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8: case GL_RGB565: break;
    default:
        gl_set_error(gc, GL_INVALID_ENUM);
        return 0;
    }
    gl_rb_drop_storage(rb);
    if (!drawable)
        return 1;
    d = calloc(1, sizeof *d);
    if (!d) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return 0;
    }
    d->ca = drawable;
    d->retained = retained & 1;
    d->need_dequeue = 1;
    d->cb.ctx = d;
    d->cb.create = cb_create;
    d->cb.destroy = cb_destroy;
    /* Every format gets a 32 bpp BGRA layer (the rasterizer's only target
     * format); RGB565/RGB8 renderbuffers just keep alpha at 1. */
    if (!((ca_bind_fn)CA_FN(drawable, 0x4))(drawable, FOURCC_BGRA, &d->cb)) {
        free(d);
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return 0;
    }
    rb->dr = d;
    rb->ifmt = ifmt;
    if (!drawable_begin(gc, rb)) {     /* sizes are known once a buffer is dequeued */
        gl_rb_drop_storage(rb);
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return 0;
    }
    gl_log("view bound: %dx%d%s", rb->w, rb->h, d->retained ? " retained" : "");
    return 1;
}

static int glesFinishTexture(void *gc)
{
    (void)gc;
    return 1;
}

static int glesPresentView(void *g)
{
    GC *gc = g;
    Rb *rb = gl_rb_lookup(gc, gc->rb);
    struct Drawable *d = rb ? rb->dr : NULL;
    RT rt;
    int ok;
    if (!d || !d->ca) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return 0;
    }
    if (!d->began)
        return 1;
    if (gc->be->finish) {
        memset(&rt, 0, sizeof rt);
        rt.w = rb->w; rt.h = rb->h; rt.pitch = rb->pitch; rt.color = rb->color; rt.flip = rb->flip;
        gc->be->finish(gc, &rt);
    }
    ok = ((ca_present_fn)CA_FN(d->ca, 0x10))(d->ca, 1);
    d->began = 0;
    d->need_dequeue = 1;
    d->prev = d->cur;
    gc->frame++;
    if (!d->presents++)
        gl_log("first present (%dx%d)", rb->w, rb->h);
    if (!ok) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return 0;
    }
    return 1;
}

/* IOS3DS_CA_SWAP_DIAG=1 counts CA swaps and the presents behind them in the
 * console log. Only for bring-up: it adds a line per swap, so it is off by
 * default (hardware logs with diag=1 can set it in SpringBoard's plist). */
static void swap_diag(unsigned n, uint32_t transaction, void *layer)
{
    static int on = -1;
    PicaStats s;
    if (on < 0)
        on = getenv("IOS3DS_CA_SWAP_DIAG") != NULL;
    if (!on)
        return;
    pica_backend_stats2(&s);
    gl_log("[swapdiag] swap #%u: transaction %u layer %p; presents %u, arena copies %u, read-backs %u",
           n, transaction, layer, s.presents, s.bounces, s.readbacks);
}

/* CA's own GL renderer reports a finished frame; the MBX engine queued
 * this behind the GPU work, which here is already done. 4.2.1's
 * IOMobileFramebufferGetID is no longer an IOConnect port, so signal the
 * main display the way OpenGLES's own non-MBX path does. */
static int glesSwapNotification(void *gc, uint32_t fb_id, uint32_t transaction, void *layer)
{
    static void *fb;
    static unsigned calls;
    unsigned n = ++calls;
    (void)fb_id;
    if (n == 1)
        log_callers("first SwapNotification");
    if (gc) {
        /* decide the stereo frame (and wipe a non-home frame's class map)
           before the flush writes the surface back */
        stereo_end_frame((GC *)gc);
        if (((GC *)gc)->be->flush_all)
            ((GC *)gc)->be->flush_all(gc);      /* the display surface must be current before the swap */
    }
    ca_frame();
    swap_diag(n, transaction, layer);
    io_init();
    if (!fb && io.fb_main)
        io.fb_main(&fb);
    if (fb && io.fb_swap_signal)
        io.fb_swap_signal(fb, transaction, layer);
    return 1;
}

static int glesSetProperty(void *gc, int pname, const void *v)
{
    (void)gc; (void)pname; (void)v;
    return 0;
}

static int glesGetProperty(void *gc, int pname, void *v)
{
    (void)gc; (void)pname;
    if (v)
        *(int *)v = 0;
    return 0;
}

static void *const interface[11] = {
    (void *)glesCreateSharegroup, (void *)glesDestroySharegroup,
    (void *)glesCreateGC, (void *)glesDestroyGC,
    (void *)glesBindCoreSurface, (void *)glesBindView,
    (void *)glesFinishTexture, (void *)glesPresentView,
    (void *)glesSwapNotification,
    (void *)glesSetProperty, (void *)glesGetProperty,
};

__attribute__((visibility("default"))) const void *GLESGetEGLInterface(void)
{
    return interface;
}
