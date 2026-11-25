/* ios3ds GL engine: the PICA200 Backend (docs/pica200.md).
 *
 * The front end (draw.c) hands over lit clip-space triangles; this file
 * turns them, plus the GC's raster state, into PICA200 command lists:
 *
 *   Vtx array  -> vertex buffer in the GPU arena (the struct is the vertex format)
 *   state      -> viewport/scissor/depth/blend/alpha registers, TEV stages
 *   textures   -> 8x8-tiled RGBA8 copies in the arena (Tex.be_priv)
 *   RT         -> a tiled colour + D24S8 depth buffer in the arena per render
 *                 target; rt->color (linear BGRA: IOSurfaces, renderbuffers,
 *                 texture images) is brought in with a display transfer when
 *                 the GPU copy is stale and written back by finish()
 *
 * Draws are batched into one command list per flush. The GPU is reached
 * through pica_dev_t: the ios3ds_gpu sysctl on iOS, the software model
 * in the host build. Anything this back end cannot do falls back to the
 * software rasterizer for that render target, with a log line.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "pica.h"
#include "pica_dev.h"
#include "stereo.h"

#define MAX_TARGETS 4
#define MAX_DEPTHS  4
#define VTX_CAP     6144                /* vertices per command list */
#define CMD_WORDS   24576               /* 96 KiB, below the driver's staging buffer */
#define CMD_RESERVE 1024                /* room one draw's state can take */
#define MAX_DIM     1024

typedef struct PTex {
    uint32_t pa, pw, ph, bytes;
    int w, h;                           /* the engine image's size (cache key) */
    int uw, uh;                         /* the part of pw x ph the image fills */
    int sx, sy;                         /* stretched to pw / ph (NPOT with REPEAT) */
    unsigned seq;
    uint32_t sseed, shash;              /* surface-backed images: seed and texel sample at upload */
    unsigned used;                      /* P.draw_id of the last draw that sampled it */
    Tex *tex;
    struct PTex *next;                  /* P.texs: every texture with GPU memory */
} PTex;

typedef struct PTarget {
    const uint32_t *key;                /* rt->color */
    int w, h, pw, ph;
    uint32_t color_pa;
    int gpu_valid;                      /* the GPU buffer holds the image */
    int dirty;                          /* ...and it is newer than rt->color */
    RT rt;                              /* where to write it back */
    unsigned used;
    uint32_t swz_pa;                    /* != 0: written back by the GPU itself (present_target) */
    uint32_t bnc_pa;                    /* scattered surface: linear arena copy the shim copies out */
    int bounce;                         /* the shim refused PRESENT: not one contiguous range */
    uint32_t *swz_list;                 /* cached swizzle command list + its vertices */
    uint32_t swz_words;                 /* (built once per target: the swizzle pass is identical) */
    int swz_opaque;                     /* the rt.opaque the list was built for */
    Vtx swz_v[6];
} PTarget;

typedef struct PDepth {
    const uint16_t *key;                /* rt->depth */
    int pw, ph;
    uint32_t pa;
    unsigned used;
} PDepth;

/* everything that is register state for one draw */
typedef struct DState {
    uint32_t vp[4];
    int sc_on;
    uint32_t sc[4];
    float clip[16];
    pica_raster_t rs;
    unsigned units;
    pica_tex_t tex[2];
    float tscale[2][2];
    pica_tev_t tev[PICA_TEV_STAGES];
} DState;

static struct {
    const pica_dev_t *dev;
    int state;                          /* 0 = not probed, 1 = up, -1 = unusable */
    uint32_t vbuf_pa;
    Vtx *vstage;
    int nvtx;
    uint32_t *list;
    pica_cmd_t cmd;
    int list_open;
    PTarget *cur;                       /* target of the open list */
    PDepth *cur_depth;
    DState last;
    int last_valid;
    PTarget target[MAX_TARGETS];
    PDepth depth[MAX_DEPTHS];
    uint32_t *scratch;
    size_t scratch_words;
    unsigned tick, warned;
    unsigned lists, draws;
    unsigned readbacks, readback_kb, uploads, upload_kb, presents, bounces;
    int present_checked;
    int no_present;                     /* the shim refused PRESENT once: read back from now on */
    uint32_t rb_pa, rb_bytes;           /* read-back staging kept while no_present (every frame needs it) */
    PTex *texs;
    unsigned draw_id;
    uint32_t arena_bytes, tex_bytes, ntex, evictions;
    int zconv;                          /* PICA_Z_*, chosen by the depth probe */
    int depth_broken;                   /* no convention drew the probe right: depth tests on the CPU */
} P;

enum { WARN_FOG = 1, WARN_TEXENV = 2, WARN_BIG = 4, WARN_MEM = 8, WARN_TEXBIG = 16 };

static void warn_once(unsigned bit, const char *msg)
{
    if (P.warned & bit)
        return;
    P.warned |= bit;
    gl_log("pica: %s", msg);
}

/* A GPU operation failed: say so and stop using the GPU. */
static void gpu_fail(const char *what, uint32_t st)
{
    if (P.state == 1)
        gl_log("pica: %s failed: %s. GPU rendering is OFF from here on, using the software rasterizer",
               what, pica_strerror(st));
    P.state = -1;
    P.list_open = 0;
    P.nvtx = 0;
}

static uint32_t *scratch(size_t words)
{
    if (words > P.scratch_words) {
        free(P.scratch);
        P.scratch = malloc(words * 4);
        P.scratch_words = P.scratch ? words : 0;
    }
    return P.scratch;
}

/* A constant-colour quad over a 64x64 RGBA8 target at `color`: the list a
   clear is made of (no PSC fills: the fill engine cannot write FCRAM, where
   the arena is - hardware run 13). Returns the list size in bytes. */
/* ------------------------------------------------------------------ */
/* GPU memory                                                          */
/* ------------------------------------------------------------------ */

static void flush_list(void);
static void sync_target(PTarget *t);
static void drop_target(PTarget *t);
static void drop_depth(PDepth *d);

static void arena_release(uint32_t pa, uint32_t bytes)
{
    if (!pa)
        return;
    P.dev->free(pa);
    P.arena_bytes -= bytes;
}

/* drop the GPU copy of a texture; the engine keeps its image and the next
   use uploads it again */
static void evict_texture(PTex *p)
{
    PTex **pp;
    for (pp = &P.texs; *pp; pp = &(*pp)->next)
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    arena_release(p->pa, p->bytes);
    P.tex_bytes -= p->bytes;
    P.ntex--;
    if (p->tex)
        p->tex->be_priv = NULL;
    free(p);
}

/* GPU memory for this process. When the arena is full, the GPU copies of
   the least recently sampled textures are dropped until the block fits
   (hardware run 15: CA's layer textures filled the 8 MiB arena and later
   ones sampled black). Textures the current draw samples stay. 0 = no room
   even then. A big block (render target, depth buffer, read-back staging)
   is placed from the top of the arena and textures from the bottom
   (pica_drv.c), so for one the textures highest in the arena go first: that
   frees the space next to the big blocks, about as much as is missing,
   where LRU order dropped textures from everywhere (test_pica's alloc test:
   about 4x fewer bytes evicted and uploaded again). */
#define BIG_BLOCK 0x40000u              /* PICA_DRV_BIG */
static int arena_alloc(uint32_t bytes, uint32_t *pa)
{
    *pa = 0;
    if (P.dev->alloc(bytes, 0, pa) == PICA_OK) {
        P.arena_bytes += bytes;
        return 1;
    }
    flush_list();                       /* queued draws may sample any texture */
    if (P.state != 1)
        return 0;
    for (;;) {
        PTex *q, *lru = NULL;
        for (q = P.texs; q; q = q->next)
            if (q->used != P.draw_id && (!lru || (bytes >= BIG_BLOCK ? q->pa > lru->pa : (int)(q->used - lru->used) < 0)))
                lru = q;
        if (!lru)
            return 0;
        evict_texture(lru);
        P.evictions++;
        if (P.dev->alloc(bytes, 0, pa) == PICA_OK) {
            P.arena_bytes += bytes;
            return 1;
        }
    }
}

/* The GPU cannot do what this process needs (no arena memory even after
   evicting every texture, or depth tests that do not work): write every
   GPU image back, give the arena memory back, and render on the CPU from
   now on. Never draws black or wrong; one log line. */
static void cpu_from_now(const char *why, uint32_t bytes)
{
    pica_info_t info;
    uint32_t held = P.arena_bytes;
    int i;
    if (P.state != 1)
        return;
    flush_list();
    while (P.texs)
        evict_texture(P.texs);
    for (i = 0; i < MAX_DEPTHS; i++)
        if (P.depth[i].key)
            drop_depth(&P.depth[i]);
    if (P.rb_pa)                        /* sync_target below reads back without it */
        arena_release(P.rb_pa, P.rb_bytes);
    P.rb_pa = 0;
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key) {
            sync_target(&P.target[i]);
            drop_target(&P.target[i]);
        }
    memset(&info, 0, sizeof info);
    P.dev->info(&info);
    gl_log("pica: %s (%u KiB wanted; this process held %u KiB, arena %u KiB free): this process draws on the "
           "CPU from now on", why, bytes >> 10, held >> 10, info.arena_free >> 10);
    P.state = -1;
    P.cur = NULL;
    P.cur_depth = NULL;
    P.list_open = 0;
    P.nvtx = 0;
}

static uint32_t solid_list(uint32_t *list, uint32_t cap, uint32_t color, uint32_t vbuf, uint32_t tev_color)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    pica_cmd_t c;
    pica_fb_t fb;
    pica_raster_t rs;
    pica_tev_t tev[PICA_TEV_STAGES];
    pica_vtx_layout_t l;
    float clip[16];
    int i;
    pica_cmd_init(&c, list, cap);
    pica_emit_base_state(&c);
    pica_ff_emit_program(&c);
    memset(&fb, 0, sizeof fb);
    fb.color_pa = color;
    fb.width = fb.height = 64;
    fb.color_fmt = PICA_CB_RGBA8;
    pica_emit_framebuffer(&c, &fb);
    pica_emit_viewport(&c, 0, 0, 64, 64);
    pica_emit_scissor(&c, 0, 0, 0, 0, 0);
    pica_raster_defaults(&rs);
    rs.write_mask = PICA_WRITE_COLOR;
    pica_emit_raster(&c, &rs);
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&tev[i]);
    tev[0].src_rgb[0] = tev[0].src_a[0] = PICA_TEVSRC_CONSTANT;
    tev[0].color = tev_color;
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(&c, (unsigned)i, &tev[i]);
    pica_emit_texunits(&c, 0);
    pica_ff_clip_matrix(clip, ident, ident);
    pica_ff_emit_matrices(&c, clip, NULL, NULL, 1.0f);
    memset(&l, 0, sizeof l);
    l.count = PICA_FF_ATTRS;
    l.attr[PICA_FF_POSITION].type = PICA_FLOAT;  l.attr[PICA_FF_POSITION].size = 4;
    l.attr[PICA_FF_COLOR].type = PICA_FLOAT;     l.attr[PICA_FF_COLOR].size = 4;
    l.attr[PICA_FF_TEXCOORD0].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD0].size = 2;
    l.attr[PICA_FF_TEXCOORD1].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD1].size = 2;
    l.nbuffers = 1;
    l.buffer[0].stride = sizeof(Vtx);
    l.base_pa = vbuf;
    pica_emit_vtx_layout(&c, &l);
    pica_emit_draw_arrays(&c, PICA_TRIANGLES, 0, 6);
    pica_emit_fb_flush(&c);
    return pica_cmd_finish(&c);
}

/* Before the first draw: one round trip through everything every frame
   relies on and the model cannot vouch for on hardware - the transport, the
   shim's cache maintenance, the PPF in both directions (8x8 tiling) and a
   P3D draw into the arena (FCRAM). 64x64 RGBA8: a pattern up and back, then
   a constant-colour quad and back. 0 = usable; otherwise logs what came
   back and the caller uses the CPU. */
static int self_check(void)
{
    enum { N = 64 * 64 };
    uint32_t a = 0, b = 0, st, i, *buf = malloc(N * 4), dim = 64u << 16 | 64u, bad = 0, first = 0;
    const char *what = "allocating";
    if (!buf)
        return -1;
    st = P.dev->alloc(N * 4, 0, &a);
    if (st == PICA_OK)
        st = P.dev->alloc(N * 4, 0, &b);
    for (i = 0; i < N; i++)
        buf[i] = i * 2654435761u ^ 0x5A5AA5A5u;
    if (st == PICA_OK && (what = "upload", st = P.dev->write(a, buf, N * 4)) == PICA_OK &&
        (what = "linear->tiled transfer", st = P.dev->transfer(a, dim, b, dim, PPF_OUT_TILED |
            PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
        (what = "tiled->linear transfer", st = P.dev->transfer(b, dim, a, dim,
            PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
        (what = "read-back", st = P.dev->read(a, buf, N * 4)) == PICA_OK) {
        for (i = 0; i < N; i++)
            if (buf[i] != (i * 2654435761u ^ 0x5A5AA5A5u) && !bad++)
                first = i;
        if (bad) {
            gl_log("pica: GPU self-check: %u of %u words wrong after a transfer round trip (word %u: 0x%08x, "
                   "expected 0x%08x): not using the GPU", bad, (unsigned)N, first, buf[first],
                   first * 2654435761u ^ 0x5A5AA5A5u);
            st = PICA_ERR_RANGE;
            what = NULL;
        } else {
            static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
            uint32_t vb = 0, *list = malloc(4096 * 4), bytes = 0;
            Vtx q[6];
            memset(q, 0, sizeof q);
            for (i = 0; i < 6; i++) {
                q[i].x = corner[i][0];
                q[i].y = corner[i][1];
                q[i].w = 1;
            }
            what = "allocating";
            st = list ? P.dev->alloc(sizeof q, 0, &vb) : PICA_ERR_NO_MEM;
            if (st == PICA_OK && !(bytes = solid_list(list, 4096, b, vb, 0x87654321u)))
                st = PICA_ERR_RANGE;
            if (st == PICA_OK && (what = "a P3D draw", st = P.dev->submit(list, bytes, vb, q, sizeof q)) == PICA_OK &&
                (what = "tiled->linear transfer", st = P.dev->transfer(b, dim, a, dim,
                    PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
                (what = "read-back", st = P.dev->read(a, buf, N * 4)) == PICA_OK) {
                for (i = 0; i < N; i++)
                    if (buf[i] != 0x21436587u && !bad++)
                        first = i;
                if (bad) {
                    gl_log("pica: GPU self-check: a constant-colour quad left %u of %u words wrong (word %u: 0x%08x, "
                           "expected 0x21436587): not using the GPU", bad, (unsigned)N, first, buf[first]);
                    st = PICA_ERR_RANGE;
                    what = NULL;
                }
            }
            if (vb)
                P.dev->free(vb);
            free(list);
        }
    }
    if (st != PICA_OK && what)
        gl_log("pica: GPU self-check: %s failed: %s: not using the GPU", what, pica_strerror(st));
    if (a) P.dev->free(a);
    if (b) P.dev->free(b);
    free(buf);
    return st == PICA_OK ? 0 : -1;
}

/* Hardware run 15: with depth stored as GL's d (a clear writes 1.0)
   nothing passed GL_LESS on the PICA200, while QEMU's model drew it. Draw a
   depth probe (ca.c depth_scene) with every convention and compare it with
   the CPU rasterizer; use citro3d's reversed depth if it matches (what
   homebrew uses on hardware), else the scaled standard one; if none
   matches, depth-tested drawing goes to the CPU (cpu_from_now on first use).
   IOS3DS_GL_ZCONV=0/1/2 forces a convention. One log line. */
static void depth_probe(void)
{
    static const char *name[3] = { "reversed (citro3d)", "standard", "unscaled (run 15)" };
    int d[3], m[3], c, pick = -1, lim = 96 * 96 / 50;   /* ca.c S x S */
    const char *force = getenv("IOS3DS_GL_ZCONV");
    for (c = 0; c < 3 && P.state == 1; c++) {
        P.zconv = c;
        d[c] = gl_scene_diff(1, &m[c]);
        if (pick < 0 && c < 2 && d[c] >= 0 && d[c] <= lim)
            pick = c;
    }
    if (P.state != 1)
        return;
    if (force && *force >= '0' && *force <= '2')
        pick = *force - '0';
    P.zconv = pick < 0 ? PICA_Z_REVERSED : pick;
    P.depth_broken = pick < 0;
    gl_log("pica: depth probe, GPU vs CPU rasterizer (pixels off by more than 24 of 9216): %s %d %s, %s %d %s, "
           "%s %d %s: %s%s", name[0], d[0], d[0] >= 0 && d[0] <= lim ? "ok" : "FAIL", name[1], d[1],
           d[1] >= 0 && d[1] <= lim ? "ok" : "FAIL", name[2], d[2], d[2] >= 0 && d[2] <= lim ? "ok" : "FAIL",
           pick < 0 ? "none works, depth-tested drawing goes to the CPU" : "using ", pick < 0 ? "" : name[pick]);
}

int pica_backend_init(void)
{
    pica_info_t info;
    uint32_t st;
    if (P.state)
        return P.state > 0;
    P.state = -1;
#ifdef GL_HOST
    P.dev = pica_dev_host();
#else
    P.dev = pica_dev_ios();
#endif
    memset(&info, 0, sizeof info);
    st = P.dev->info(&info);
    if (st != PICA_OK || info.state != PICA_OK) {
        gl_log("pica: GPU not available: %s (PDN_GPU_CNT 0x%x)", pica_strerror(st != PICA_OK ? st : info.state),
               info.gpu_cnt);
        return 0;
    }
    if (info.version != PICA_PROTO_VERSION) {
        gl_log("pica: shim speaks protocol %u, this engine %u: not using the GPU", info.version, PICA_PROTO_VERSION);
        return 0;
    }
    if (self_check())
        return 0;
    P.vstage = malloc(sizeof(Vtx) * VTX_CAP);
    P.list = malloc(CMD_WORDS * 4);
    st = P.dev->alloc(sizeof(Vtx) * VTX_CAP, 0, &P.vbuf_pa);
    if (!P.vstage || !P.list || st != PICA_OK) {
        gl_log("pica: no memory for the vertex buffer (%s)", pica_strerror(st));
        return 0;
    }
    P.arena_bytes = sizeof(Vtx) * VTX_CAP;
    P.state = 1;
    gl_log("pica: GPU up (self-check passed): arena %u KiB at PA 0x%08x (%u KiB free), PDN_GPU_CNT 0x%x, %s",
           info.arena_size >> 10, info.arena_pa, info.arena_free >> 10, info.gpu_cnt, P.dev->name);
    depth_probe();
    return P.state == 1;
}

/* ------------------------------------------------------------------ */
/* command list                                                        */
/* ------------------------------------------------------------------ */

static void flush_list(void)
{
    uint32_t bytes, st;
    if (!P.list_open)
        return;
    P.list_open = 0;
    pica_emit_fb_flush(&P.cmd);
    bytes = pica_cmd_finish(&P.cmd);
    if (!bytes) {
        gpu_fail("building a command list (overflow)", PICA_ERR_RANGE);
        return;
    }
    st = P.dev->submit(P.list, bytes, P.vbuf_pa, P.vstage, (uint32_t)(P.nvtx * sizeof(Vtx)));
    P.nvtx = 0;
    P.lists++;
    if (st != PICA_OK)
        gpu_fail("running a command list", st);
}

/* IOS3DS_GL_SCATTER=1: ask the shim to refuse FCRAM presents so the
   PICA_OP_SCATTER path runs (test hook; VRAM keeps its direct present). */
static int force_scatter(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("IOS3DS_GL_SCATTER") != NULL;
    return on;
}

/*
 * Write a surface target back without the CPU: one more GPU pass turns the
 * colour buffer (sampled as a power-of-two texture, 1:1, nearest) into a
 * w x h buffer whose RGBA8 bytes are the surface's B,G,R,A (the PICA200 has
 * no BGRA format: TEV rebuilds r = a, g = r, b = g, a = b), and a display
 * transfer (PICA_OP_PRESENT) puts it straight into the IOSurface's physical
 * memory. 0 = done; else the caller reads back through the CPU.
 */
static int present_target(PTarget *t)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    static const float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    pica_cmd_t c;
    pica_fb_t fb;
    pica_tex_t tx;
    pica_tev_t tev[PICA_TEV_STAGES];
    pica_raster_t rs;
    pica_vtx_layout_t l;
    float clip[16];
    uint32_t bytes, dim = (uint32_t)t->h << 16 | (uint32_t)t->w, st, flags;
    uint32_t sbytes = (uint32_t)t->w * t->h * 4;
    int i;
    /* The swizzle pass is identical every frame for a given target (same
       colour buffer, swizzle buffer, dimensions and opacity), so build the
       command list and its six vertices once and resubmit them.  Building it
       on every frame cost the render thread a few hundred instructions per
       present; a frame's present is the hot path. */
    if (!t->swz_list || t->swz_opaque != (t->rt.opaque != 0)) {
        free(t->swz_list);
        t->swz_list = malloc(2048 * 4);
        if (!t->swz_list)
            return -1;
        t->swz_opaque = t->rt.opaque != 0;
        pica_cmd_init(&c, t->swz_list, 2048);
        pica_emit_base_state(&c);
        pica_ff_emit_program(&c);
        memset(&fb, 0, sizeof fb);
        fb.color_pa = t->swz_pa;
        fb.width = (uint16_t)t->w;
        fb.height = (uint16_t)t->h;
        fb.color_fmt = PICA_CB_RGBA8;
        pica_emit_framebuffer(&c, &fb);
        pica_emit_viewport(&c, 0, 0, (uint32_t)t->w, (uint32_t)t->h);
        pica_emit_scissor(&c, 0, 0, 0, 0, 0);
        pica_raster_defaults(&rs);
        rs.write_mask = PICA_WRITE_COLOR;
        pica_emit_raster(&c, &rs);
        memset(&tx, 0, sizeof tx);
        tx.pa = t->color_pa;
        tx.width = (uint16_t)t->pw;
        tx.height = (uint16_t)t->ph;
        tx.fmt = PICA_TEX_RGBA8;
        tx.mag = tx.min = PICA_NEAREST;
        tx.wrap_s = tx.wrap_t = PICA_CLAMP_TO_EDGE;
        pica_emit_texture(&c, 0, &tx);
        pica_emit_texunits(&c, 1);
        for (i = 0; i < PICA_TEV_STAGES; i++)
            pica_tev_passthrough(&tev[i]);
        for (i = 0; i < 3; i++) {
            static const uint8_t op[3] = { PICA_TEVOP_RGB_ALPHA, PICA_TEVOP_RGB_R, PICA_TEVOP_RGB_G };
            static const uint32_t k[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u };     /* R | G<<8 | B<<16 | A<<24 */
            tev[i].src_rgb[0] = PICA_TEVSRC_TEXTURE0;
            tev[i].src_rgb[1] = PICA_TEVSRC_CONSTANT;
            tev[i].src_rgb[2] = i ? PICA_TEVSRC_PREVIOUS : PICA_TEVSRC_CONSTANT;
            tev[i].op_rgb[0] = op[i];
            tev[i].op_rgb[1] = PICA_TEVOP_RGB_COLOR;
            tev[i].op_rgb[2] = PICA_TEVOP_RGB_COLOR;
            tev[i].func_rgb = i ? PICA_TEV_MULT_ADD : PICA_TEV_MODULATE;
            tev[i].color = k[i];
            /* alpha = the sample's blue (the surface's B byte), or opaque */
            tev[i].src_a[0] = i == 2 && !t->rt.opaque ? PICA_TEVSRC_TEXTURE0 : i == 2 ? PICA_TEVSRC_CONSTANT : PICA_TEVSRC_PREVIOUS;
            tev[i].op_a[0] = i == 2 && !t->rt.opaque ? PICA_TEVOP_A_B : PICA_TEVOP_A_ALPHA;
            tev[i].func_a = PICA_TEV_REPLACE;
        }
        tev[0].src_a[0] = PICA_TEVSRC_CONSTANT;    /* stage 0 has no "previous" */
        for (i = 0; i < PICA_TEV_STAGES; i++)
            pica_emit_tev(&c, (unsigned)i, &tev[i]);
        pica_ff_clip_matrix(clip, ident, ident);
        pica_ff_emit_matrices(&c, clip, NULL, NULL, 1.0f);
        memset(&l, 0, sizeof l);
        l.count = PICA_FF_ATTRS;
        l.attr[PICA_FF_POSITION].type = PICA_FLOAT;  l.attr[PICA_FF_POSITION].size = 4;
        l.attr[PICA_FF_COLOR].type = PICA_FLOAT;     l.attr[PICA_FF_COLOR].size = 4;
        l.attr[PICA_FF_TEXCOORD0].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD0].size = 2;
        l.attr[PICA_FF_TEXCOORD1].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD1].size = 2;
        l.nbuffers = 1;
        l.buffer[0].stride = sizeof(Vtx);
        l.base_pa = P.vbuf_pa;
        pica_emit_vtx_layout(&c, &l);
        pica_emit_draw_arrays(&c, PICA_TRIANGLES, 0, 6);
        pica_emit_fb_flush(&c);
        t->swz_words = pica_cmd_finish(&c);
        /* GL row y of the target is GL row y of the texture (both stored top
           first), so pixel (x, y) samples texel (x, y): s = x / pw, t = y / ph */
        memset(t->swz_v, 0, sizeof t->swz_v);
        for (i = 0; i < 6; i++) {
            t->swz_v[i].x = corner[i][0] * 2 - 1;
            t->swz_v[i].y = corner[i][1] * 2 - 1;
            t->swz_v[i].w = 1;
            t->swz_v[i].c[0] = t->swz_v[i].c[1] = t->swz_v[i].c[2] = t->swz_v[i].c[3] = 1;
            t->swz_v[i].t[0][0] = corner[i][0] * (float)t->w / (float)t->pw;
            t->swz_v[i].t[0][1] = corner[i][1] * (float)t->h / (float)t->ph;
            t->swz_v[i].fog = 1;
        }
    }
    bytes = t->swz_words;
    if (!bytes || P.dev->submit(t->swz_list, bytes, P.vbuf_pa, t->swz_v, sizeof t->swz_v) != PICA_OK)
        return -1;
    /* XNU maps a user page that has not been written since its modified bit
       was cleared read-only and makes it writable on the first write fault
       (seen in QEMU: CA's display surface, PA 0x18000000, AP=10 before the
       first frame). The shim only lets the GPU write pages the caller may
       write right now, so write every page once ourselves (same value). */
    {
        volatile uint32_t *pg = (volatile uint32_t *)t->rt.color;
        uint32_t n = (uint32_t)t->w * t->h, k;
        for (k = 0; k < n; k += 1024)
            pg[k] = pg[k];
        pg[n - 1] = pg[n - 1];
    }
    /* the transfer stores the first stored row (GL top) first: right for
       flipped surfaces (row 0 = GL top); for the others flip it */
    flags = PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8) | (t->rt.flip ? 0 : PPF_FLIP_V) |
            (force_scatter() ? PICA_PRESENT_TEST_SCATTER : 0);
    st = t->bounce ? PICA_ERR_NOT_CONTIG : P.dev->present(t->swz_pa, dim, t->rt.color, sbytes, flags);
    if (st == PICA_ERR_NOT_CONTIG) {
        /* A surface in scattered pages (CA's second 320x480 surface after an
           app returns to the home screen; hardware runs 21/25/29, New 2DS XL
           run 1): the same transfer into a linear arena buffer, then
           PICA_OP_SCATTER copies it page by page into the caller's pages
           with their cache lines maintained. Bytes are already in the
           surface's order: no CPU pixel work, and every other surface keeps
           its direct present. */
        if (!t->bounce)
            gl_log("pica: surface %p (%dx%d) is not physically contiguous: the GPU writes it through an arena "
                   "copy", (void *)t->rt.color, t->w, t->h);
        t->bounce = 1;
        if (!t->bnc_pa && !arena_alloc(sbytes, &t->bnc_pa))
            return -1;                  /* no room this frame: the caller reads it back */
        st = P.dev->transfer(t->swz_pa, dim, t->bnc_pa, dim, flags);
        if (st == PICA_OK)
            st = P.dev->scatter(t->bnc_pa, t->rt.color, sbytes);
        if (st == PICA_OK) {
            P.bounces++;
            P.presents++;
            return 0;
        }
    }
    if (st != PICA_OK) {
        /* any other refusal or GPU error: read back, for the whole process */
        gl_log("pica: writing surface %p (%dx%d) back on the GPU failed (%s): reading surfaces back through the "
               "CPU from now on", (void *)t->rt.color, t->w, t->h, pica_strerror(st));
        P.no_present = 1;
        return -1;
    }
    P.presents++;
    return 0;
}

/* the CPU read-back: the GPU image of a target into dst (rt.pitch layout);
   0 = done, -1 = no memory, else the GPU error */
static uint32_t read_back(PTarget *t, uint32_t *dst)
{
    uint32_t dim = (uint32_t)t->ph << 16 | (uint32_t)t->pw, st, *lin;
    int y, x;
    uint32_t bytes = (uint32_t)t->pw * t->ph * 4, lin_pa;
    lin = scratch((size_t)t->pw * t->ph);
    if (!lin)
        return (uint32_t)-1;
    /* the linear staging buffer only lives for the transfer (run 15: arena),
       unless every frame is read back (no_present): then one buffer is kept,
       instead of taking 1 MiB from the arena and evicting textures for it on
       every frame (hardware runs 21 and 24: "arena full: 1048576 bytes
       wanted" 256 times, 1 MiB read back per frame) */
    if (P.rb_pa && P.rb_bytes >= bytes) {
        lin_pa = P.rb_pa;
    } else {
        if (P.rb_pa) {
            arena_release(P.rb_pa, P.rb_bytes);
            P.rb_pa = 0;
        }
        if (!arena_alloc(bytes, &lin_pa))
            return (uint32_t)-1;
        if (P.no_present) {
            P.rb_pa = lin_pa;
            P.rb_bytes = bytes;
        }
    }
    st = P.dev->transfer(t->color_pa, dim, lin_pa, dim, PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8));
    P.readbacks++;
    P.readback_kb += bytes >> 10;
    if (st == PICA_OK)
        st = P.dev->read(lin_pa, lin, bytes);
    if (lin_pa != P.rb_pa)
        arena_release(lin_pa, bytes);
    if (st != PICA_OK)
        return st;
    /* Transfer row r is GL row ph-1-r; a GPU word is R<<24|G<<16|B<<8|A and
       the engine's is A<<24|R<<16|G<<8|B: rotate right by 8. */
    for (y = 0; y < t->h; y++) {
        const uint32_t *s = lin + (size_t)(t->ph - 1 - y) * t->pw;
        uint32_t *d = dst + (size_t)(t->rt.flip ? t->h - 1 - y : y) * t->rt.pitch;
        for (x = 0; x < t->w; x++)
            d[x] = (s[x] >> 8 | s[x] << 24) | t->rt.opaque;
    }
    return 0;
}

/* Until one non-trivial frame has matched, compare what the GPU presented
   with the CPU read-back of the same frame (the swizzle pass's TEV setup and
   the transfer into the surface are not verified on hardware). Allows +-2
   per channel (TEV rounding). A mismatch: the read-back from now on, and
   this frame is replaced by the read-back. */
static void check_present(PTarget *t)
{
    uint32_t n = (uint32_t)t->w * t->h, i, bad = 0, uniform = 0, fx = 0, fy = 0, got = 0, want = 0, st;
    uint32_t *ref = malloc((size_t)n * 4);
    const uint32_t *surf = t->rt.color;
    int k;
    if (!ref)
        return;
    if ((st = read_back(t, ref)) != 0) {
        free(ref);
        if (st != (uint32_t)-1)
            gpu_fail("reading a frame back", st);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t a = surf[i], b = ref[i];
        if (a == ref[0] || b == ref[0])
            uniform++;
        for (k = 0; k < 32; k += 8) {
            int d = (int)(a >> k & 0xFF) - (int)(b >> k & 0xFF);
            if (d > 2 || d < -2) {
                if (!bad++) {
                    fx = i % (uint32_t)t->w;
                    fy = i / (uint32_t)t->w;
                    got = a;
                    want = b;
                }
                break;
            }
        }
    }
    if (bad) {
        gl_log("pica: GPU present check FAILED: %u of %u pixels differ from the CPU read-back (first at %u,%u: "
               "0x%08x, expected 0x%08x): reading surfaces back through the CPU from now on",
               bad, n, fx, fy, got, want);
        memcpy(t->rt.color, ref, (size_t)n * 4);
        P.no_present = 1;
    } else if (uniform < n - n / 100) {
        gl_log("pica: GPU present check passed (%ux%u, identical to the CPU read-back within 2)", t->w, t->h);
        P.present_checked = 1;
    }
    free(ref);
}

/* write the GPU image of a target back into its CPU memory */
static void sync_target(PTarget *t)
{
    uint32_t st;
    if (P.cur == t)
        flush_list();
    if (!t->dirty || P.state != 1)
        return;
    t->dirty = 0;
    if (t->swz_pa && !P.no_present && present_target(t) == 0) {
        if (!P.present_checked && !getenv("IOS3DS_GL_NO_PRESENT_CHECK"))
            check_present(t);
        return;
    }
    if ((st = read_back(t, t->rt.color)) != 0 && st != (uint32_t)-1)
        gpu_fail("reading a frame back", st);
    else if (st == (uint32_t)-1)
        gl_log("pica: no memory to read a %dx%d target back: its last GPU frame is lost", t->w, t->h);
    else if (t->rt.surface)
        /* The read-back wrote the final frame straight into the IOSurface
           from userland. PRESENT/SCATTER tell the mirror about their frames
           themselves, but this path needs an explicit announcement too:
           stereo3d=1 keeps the stamp by reading the display target back,
           and a refused present switches every surface to this path. */
        P.dev->dirty();
}

/* bring rt->color into the GPU buffer */
static int load_target(PTarget *t)
{
    uint32_t dim = (uint32_t)t->ph << 16 | (uint32_t)t->pw, st, *lin = scratch((size_t)t->pw * t->ph), lin_pa;
    uint32_t bytes = (uint32_t)t->pw * t->ph * 4;
    int y, x;
    if (!lin)
        return 0;
    if (!arena_alloc(bytes, &lin_pa)) {
        cpu_from_now("GPU arena full (render target upload)", bytes);
        return 0;
    }
    memset(lin, 0, (size_t)t->pw * t->ph * 4);
    for (y = 0; y < t->h; y++) {
        uint32_t *d = lin + (size_t)(t->ph - 1 - y) * t->pw;
        const uint32_t *s = t->rt.color + (size_t)(t->rt.flip ? t->h - 1 - y : y) * t->rt.pitch;
        for (x = 0; x < t->w; x++)
            d[x] = s[x] << 8 | s[x] >> 24;
    }
    st = P.dev->write(lin_pa, lin, bytes);
    if (st == PICA_OK)
        st = P.dev->transfer(lin_pa, dim, t->color_pa, dim,
                             PPF_OUT_TILED | PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8));
    arena_release(lin_pa, bytes);
    if (st != PICA_OK) {
        gpu_fail("uploading a render target", st);
        return 0;
    }
    t->gpu_valid = 1;
    return 1;
}

static void drop_target(PTarget *t)
{
    if (P.cur == t) {
        P.cur = NULL;
        P.list_open = 0;                /* its pending draws can no longer be seen */
        P.nvtx = 0;
    }
    arena_release(t->color_pa, (uint32_t)t->pw * t->ph * 4);
    arena_release(t->swz_pa, (uint32_t)t->w * t->h * 4);
    arena_release(t->bnc_pa, (uint32_t)t->w * t->h * 4);
    free(t->swz_list);
    memset(t, 0, sizeof *t);
}

static void drop_depth(PDepth *d)
{
    if (P.cur_depth == d) {
        flush_list();
        P.cur_depth = NULL;
    }
    arena_release(d->pa, (uint32_t)d->pw * d->ph * 4);
    memset(d, 0, sizeof *d);
}

/* The engine is about to free or reuse this memory (renderbuffer storage,
   texture image): forget GPU copies keyed on it. */
void pica_forget(const void *mem)
{
    int i;
    stereo_forget(mem);
    if (!mem || P.state != 1)
        return;
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key == mem)
            drop_target(&P.target[i]);
    for (i = 0; i < MAX_DEPTHS; i++)
        if (P.depth[i].key == mem)
            drop_depth(&P.depth[i]);
}

static int pad_dim(int n)
{
    n = (n + 15) & ~15;
    return n < 64 ? 64 : n;             /* display transfers below 64x64 are unreliable */
}

static PTarget *get_target(const RT *rt)
{
    PTarget *t = NULL, *lru = &P.target[0];
    uint32_t bytes;
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        PTarget *c = &P.target[i];
        if (c->key == rt->color && c->w == rt->w && c->h == rt->h) {
            t = c;
            break;
        }
        if (c->used < lru->used)
            lru = c;
    }
    if (!t) {
        if (rt->w > MAX_DIM || rt->h > MAX_DIM) {
            warn_once(WARN_BIG, "render target larger than 1024: drawn by the software rasterizer");
            return NULL;
        }
        /* With stereo3d=1 the depth class lives in the render target's alpha
           byte. The direct present's swizzle carries the target's alpha into
           the surface's alpha byte unchanged (gltest's alpha_stamp_check),
           so the stereo display target keeps the GPU present too; the CPU
           read-back it used before (re-log 106) cost a 600 KiB copy per
           frame on the ARM11 (re-log 127). */
        int present = (rt->surface || getenv("IOS3DS_GL_PRESENT_ALL")) && !P.no_present && !(rt->w & 7) && !(rt->h & 7) && rt->pitch == rt->w &&
                      !((uintptr_t)rt->color & 7) && !getenv("IOS3DS_GL_NO_PRESENT");
        t = lru;
        if (t->key) {
            sync_target(t);
            drop_target(t);
        }
        /* An IOSurface (CA's display) is written back by the GPU itself
           (present_target): its colour buffer is then sampled as a texture,
           so it is allocated power-of-two (NPOT textures sample wrongly on
           the PICA200, hardware run 13). */
        t->pw = present ? (int)pica_pot((uint32_t)(rt->w < 64 ? 64 : rt->w)) : pad_dim(rt->w);
        t->ph = present ? (int)pica_pot((uint32_t)(rt->h < 64 ? 64 : rt->h)) : pad_dim(rt->h);
        bytes = (uint32_t)t->pw * t->ph * 4;
        t->w = rt->w;
        t->h = rt->h;
        if (!arena_alloc(bytes, &t->color_pa) ||
            (present && !arena_alloc((uint32_t)rt->w * rt->h * 4, &t->swz_pa))) {
            drop_target(t);
            cpu_from_now("GPU arena full (render target)", bytes);
            return NULL;
        }
        t->key = rt->color;
    }
    t->rt = *rt;
    t->used = ++P.tick;
    return t;
}

static PDepth *get_depth(const RT *rt, const PTarget *t)
{
    PDepth *d = NULL, *lru = &P.depth[0];
    uint32_t bytes = (uint32_t)t->pw * t->ph * 4;
    int i;
    if (!rt->depth)
        return NULL;
    for (i = 0; i < MAX_DEPTHS; i++) {
        PDepth *c = &P.depth[i];
        if (c->key == rt->depth && c->pw == t->pw && c->ph == t->ph) {
            d = c;
            break;
        }
        if (c->used < lru->used)
            lru = c;
    }
    if (!d) {
        d = lru;
        if (d->key)
            drop_depth(d);
        if (!arena_alloc(bytes, &d->pa)) {
            memset(d, 0, sizeof *d);
            cpu_from_now("GPU arena full (depth buffer)", bytes);
            return NULL;
        }
        d->key = rt->depth;
        d->pw = t->pw;
        d->ph = t->ph;
        {   /* far: a constant, so tiling does not matter; written by the CPU because
               the fill engine cannot reach the (FCRAM) arena */
            uint32_t *z = scratch(bytes / 4), k, far = pica_depth_stored((unsigned)P.zconv, 1.0f);
            if (z) {
                for (k = 0; k < bytes / 4; k++)
                    z[k] = far;
                P.dev->write(d->pa, z, bytes);
            }
        }
    }
    d->used = ++P.tick;
    return d;
}

/* Make rt the target of the open command list. NULL: use the software path. */
static PTarget *bind_target(const RT *rt)
{
    PTarget *t;
    PDepth *d;
    if (P.state != 1)
        return NULL;
    if (P.cur && (P.cur->key != rt->color || P.cur->w != rt->w || P.cur->h != rt->h))
        sync_target(P.cur);             /* one target is ahead of its CPU copy at a time */
    t = get_target(rt);
    if (!t || P.state != 1)
        return NULL;
    d = get_depth(rt, t);
    if (P.state != 1)
        return NULL;
    if (t != P.cur || d != P.cur_depth) {
        flush_list();
        P.cur = t;
        P.cur_depth = d;
    }
    return P.state == 1 ? t : NULL;
}

static void open_list(void)
{
    pica_fb_t fb;
    if (P.list_open)
        return;
    pica_cmd_init(&P.cmd, P.list, CMD_WORDS);
    pica_emit_base_state(&P.cmd);
    pica_ff_emit_program(&P.cmd);
    memset(&fb, 0, sizeof fb);
    fb.color_pa = P.cur->color_pa;
    fb.depth_pa = P.cur_depth ? P.cur_depth->pa : 0;
    fb.width = (uint16_t)P.cur->pw;
    fb.height = (uint16_t)P.cur->ph;
    fb.color_fmt = PICA_CB_RGBA8;
    fb.depth_fmt = PICA_DEPTH24S8;
    pica_emit_framebuffer(&P.cmd, &fb);
    P.list_open = 1;
    P.last_valid = 0;
    P.nvtx = 0;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

static void pica_tex_deleted(Tex *t)
{
    PTex *p = t->be_priv;
    if (!p)
        return;
    if (P.state == 1)
        flush_list();                   /* pending draws may sample it */
    evict_texture(p);
    t->be_priv = NULL;
}

static void pica_tex_changed(GC *gc, Tex *t)
{
    (void)gc;
    /* the image of a render-to-texture target was replaced on the CPU side */
    if (t->px && P.state == 1) {
        int i;
        for (i = 0; i < MAX_TARGETS; i++)
            if (P.target[i].key == t->px) {
                if (P.cur == &P.target[i])
                    flush_list();
                P.target[i].gpu_valid = P.target[i].dirty = 0;
            }
    }
}

/* REPEAT on an axis whose size is not a power of two: the GPU texture is
   power-of-two (run 13: NPOT textures sample garbage), so a padded image
   would repeat with its padding. Such an axis is stretched to the
   power-of-two size instead (nearest), and its texture coordinates stay. */
static int wraps(GLenum w)
{
    int v = pica_gl_wrap(w);
    return v < 0 || v == PICA_REPEAT || v == PICA_MIRRORED_REPEAT;
}

/* A sparse sample of an image's texels (64 words over the whole image):
   a camera or video frame written into a recycled IOSurface changes it. */
static uint32_t texel_sample(const Tex *t)
{
    uint32_t hsh = 2166136261u;
    if (!t->px || t->w <= 0 || t->h <= 0)
        return 0;
    for (int i = 0; i < 64; i++) {
        int y = (int)((uint32_t)(2 * (i >> 3) + 1) * (uint32_t)t->h / 16u);
        int x = (int)((uint32_t)(2 * (i & 7) + 1) * (uint32_t)t->w / 16u + (uint32_t)i % 3u);
        if (x >= t->w)
            x = t->w - 1;
        hsh = (hsh ^ t->px[(size_t)y * t->pitch + x]) * 16777619u;
    }
    return hsh;
}

/* Has a surface-backed image changed since its upload? CA binds a surface
   once and keeps the texture while the producer (vcd's camera pool, video)
   rewrites the surface: run 30 showed 0 texture uploads per frame while
   the camera streamed, so the preview looped over the pool's first frames. */
static int surface_changed(const Tex *t, const PTex *p, uint32_t *seed, uint32_t *hsh)
{
    int have_seed;
    *seed = 0;
    *hsh = 0;
    if (!t->surface || t->own_px)
        return 0;
    have_seed = tex_surface_seed(t, seed);
    *hsh = texel_sample(t);
    return (have_seed && *seed != p->sseed) || *hsh != p->shash;
}

/* the tiled GPU copy of a texture, uploaded when the image changed */
static PTex *use_texture(Tex *t)
{
    PTex *p = t->be_priv;
    uint32_t pw = pica_pot((uint32_t)t->w), ph = pica_pot((uint32_t)t->h), bytes = pw * ph * 4, st;
    int sx = (uint32_t)t->w != pw && wraps(t->wrap_s), sy = (uint32_t)t->h != ph && wraps(t->wrap_t);
    int uw = sx ? (int)pw : t->w, uh = sy ? (int)ph : t->h;
    const uint32_t *src = t->px;
    uint32_t *stretched = NULL, src_pitch = (uint32_t)t->pitch * 4;
    void *tiled;
    if (pw > MAX_DIM || ph > MAX_DIM) {
        warn_once(WARN_TEXBIG, "texture larger than 1024: sampled as opaque black");
        return NULL;
    }
    uint32_t sseed = 0, shash = 0;
    if (p && p->seq == t->be_seq && p->w == t->w && p->h == t->h && p->sx == sx && p->sy == sy &&
        !surface_changed(t, p, &sseed, &shash)) {
        p->used = P.draw_id;
        return p;
    }
    if (t->surface && !t->own_px && !shash) {
        tex_surface_seed(t, &sseed);
        shash = texel_sample(t);
    }
    flush_list();                       /* earlier draws must see the old texels */
    if (P.state != 1)
        return NULL;
    if (p && p->bytes != bytes) {
        evict_texture(p);
        p = NULL;
    }
    if (!p) {
        p = calloc(1, sizeof *p);
        if (!p)
            return NULL;
        if (!arena_alloc(bytes, &p->pa)) {
            free(p);
            cpu_from_now("GPU arena full (texture), even after evicting every texture", bytes);
            return NULL;
        }
        p->bytes = bytes;
        p->tex = t;
        p->next = P.texs;
        P.texs = p;
        P.tex_bytes += bytes;
        P.ntex++;
        t->be_priv = p;
    }
    p->used = P.draw_id;
    if (sx || sy) {
        int x, y;
        stretched = malloc((size_t)uw * uh * 4);
        if (!stretched)
            return NULL;
        for (y = 0; y < uh; y++) {
            const uint32_t *row = t->px + (size_t)(sy ? (2 * y + 1) * t->h / (2 * uh) : y) * t->pitch;
            for (x = 0; x < uw; x++)
                stretched[(size_t)y * uw + x] = row[sx ? (2 * x + 1) * t->w / (2 * uw) : x];
        }
        src = stretched;
        src_pitch = (uint32_t)uw * 4;
    }
    tiled = scratch(pw * ph);
    if (!tiled) {
        free(stretched);
        return NULL;
    }
    if ((uint32_t)uw != pw || (uint32_t)uh != ph)
        memset(tiled, 0, bytes);
    /* px words are 0xAARRGGBB = bytes B, G, R, A */
    pica_tex_upload(tiled, PICA_TEX_RGBA8, pw, ph, 0, 0, src, PICA_SRC_BGRA8888, (uint32_t)uw, (uint32_t)uh,
                    src_pitch);
    free(stretched);
    if ((uint32_t)uw != pw || (uint32_t)uh != ph)
        pica_tex_pad_edges(tiled, PICA_TEX_RGBA8, pw, ph, (uint32_t)uw, (uint32_t)uh);
    st = P.dev->write(p->pa, tiled, bytes);
    P.uploads++;
    P.upload_kb += bytes >> 10;
    if (st != PICA_OK) {
        gpu_fail("uploading a texture", st);
        return NULL;
    }
    p->pw = pw;
    p->ph = ph;
    p->w = t->w;
    p->h = t->h;
    p->uw = uw;
    p->uh = uh;
    p->sx = sx;
    p->sy = sy;
    p->seq = t->be_seq;
    p->sseed = sseed;
    p->shash = shash;
    P.last_valid = 0;                   /* re-send the texture state: drops the GPU's texel cache */
    return p;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static uint32_t rgba_u32(const float c[4])
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        v |= (uint32_t)(f * 255.0f + 0.5f) << (8 * i);
    }
    return v;
}

static int ifunc(GLenum f, int dflt)
{
    int v = pica_gl_func(f);
    return v < 0 ? dflt : v;
}

static int ifactor(GLenum f, int dflt)
{
    int v = pica_gl_blend_factor(f);
    return v < 0 ? dflt : v;
}

/* Viewport, scissor and the clip matrix. The GPU's viewport origin cannot be
   negative and must lie inside the target, so a GL viewport that does not
   fit is replaced by the whole target plus a scale/offset folded into the
   clip matrix, and the scissor box takes over the clipping. */
static void state_viewport(DState *s, const GC *gc, const PTarget *t, const int vp[4], int scissor, const int sc[4])
{
    float m[16], ident[16];
    int x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;
    int gl_scissor = scissor && sc;     /* the GL scissor test itself is on */
    int fits = vp[0] >= 0 && vp[1] >= 0 && vp[2] > 0 && vp[3] > 0 && vp[0] + vp[2] <= t->pw && vp[1] + vp[3] <= t->ph;
    memset(m, 0, sizeof m);
    memset(ident, 0, sizeof ident);
    m[0] = m[5] = m[10] = m[15] = 1;
    ident[0] = ident[5] = ident[10] = ident[15] = 1;
    (void)gc;
    if (fits) {
        s->vp[0] = (uint32_t)vp[0]; s->vp[1] = (uint32_t)vp[1]; s->vp[2] = (uint32_t)vp[2]; s->vp[3] = (uint32_t)vp[3];
    } else {
        s->vp[0] = s->vp[1] = 0;
        s->vp[2] = (uint32_t)t->pw;
        s->vp[3] = (uint32_t)t->ph;
        /* window x = vx + (x/w + 1) vw/2  must equal  (x'/w + 1) pw/2 */
        m[0] = (float)vp[2] / (float)t->pw;
        m[12] = (2.0f * (float)vp[0] + (float)vp[2]) / (float)t->pw - 1.0f;
        m[5] = (float)vp[3] / (float)t->ph;
        m[13] = (2.0f * (float)vp[1] + (float)vp[3]) / (float)t->ph - 1.0f;
        if (vp[0] > x0) x0 = vp[0];
        if (vp[1] > y0) y0 = vp[1];
        if (vp[0] + vp[2] < x1) x1 = vp[0] + vp[2];
        if (vp[1] + vp[3] < y1) y1 = vp[1] + vp[3];
        scissor = 1;
    }
    if (gl_scissor) {
        if (sc[0] > x0) x0 = sc[0];
        if (sc[1] > y0) y0 = sc[1];
        if (sc[0] + sc[2] < x1) x1 = sc[0] + sc[2];
        if (sc[1] + sc[3] < y1) y1 = sc[1] + sc[3];
    }
    s->sc_on = scissor;
    if (scissor && x1 > x0 && y1 > y0) {
        s->sc[0] = (uint32_t)x0; s->sc[1] = (uint32_t)y0; s->sc[2] = (uint32_t)(x1 - x0); s->sc[3] = (uint32_t)(y1 - y0);
    }                                   /* else w = h = 0: nothing is drawn */
    pica_ff_clip_matrix(s->clip, m, ident);
}

static void build_state(DState *s, GC *gc, const RT *rt, const PTarget *t)
{
    const pica_gl_texenv_t *units[2] = { NULL, NULL };
    pica_gl_texenv_t env[2];
    pica_raster_t *r = &s->rs;
    int i, v;

    memset(s, 0, sizeof *s);
    state_viewport(s, gc, t, gc->vp, gc->scissor_on, gc->scissor);

    pica_raster_defaults(r);
    r->depth_conv = (uint8_t)P.zconv;
    P.draw_id++;                        /* textures this draw samples are not evicted */
    if (gc->cull && gc->cull_mode != GL_FRONT_AND_BACK)
        r->cull = (gc->cull_mode == GL_BACK) == (gc->front_face == GL_CCW) ? PICA_CULL_BACK_CCW : PICA_CULL_FRONT_CCW;
    r->depth_test = gc->depth_test && P.cur_depth;
    r->depth_func = (uint8_t)ifunc(gc->depth_func, PICA_LESS);
    r->write_mask = (uint8_t)((gc->color_mask[0] ? PICA_WRITE_R : 0) | (gc->color_mask[1] ? PICA_WRITE_G : 0) |
                              (gc->color_mask[2] ? PICA_WRITE_B : 0) |
                              (gc->color_mask[3] && !rt->opaque ? PICA_WRITE_A : 0) |
                              (gc->depth_mask ? PICA_WRITE_DEPTH : 0));
    r->alpha_test = (uint8_t)gc->alpha_test;
    r->alpha_func = (uint8_t)ifunc(gc->alpha_func, PICA_ALWAYS);
    v = (int)(gc->alpha_ref * 255.0f + 0.5f);
    r->alpha_ref = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    r->blend = (uint8_t)gc->blend;
    if (gc->blend) {
        v = pica_gl_blend_eq(gc->blend_eq_rgb); r->blend_eq_rgb = (uint8_t)(v < 0 ? PICA_BLEND_ADD : v);
        v = pica_gl_blend_eq(gc->blend_eq_a);   r->blend_eq_a = (uint8_t)(v < 0 ? PICA_BLEND_ADD : v);
        r->blend_src_rgb = (uint8_t)ifactor(gc->blend_src_rgb, PICA_BF_ONE);
        r->blend_dst_rgb = (uint8_t)ifactor(gc->blend_dst_rgb, PICA_BF_ZERO);
        r->blend_src_a = (uint8_t)ifactor(gc->blend_src_a, PICA_BF_ONE);
        r->blend_dst_a = (uint8_t)ifactor(gc->blend_dst_a, PICA_BF_ZERO);
    }
    r->depth_near = gc->depth_near;
    r->depth_far = gc->depth_far;
    /* glPolygonOffset: only the constant term exists on this GPU; the slope
       term is approximated by one unit per unit of factor */
    if (gc->poly_offset_on)
        r->depth_bias = (gc->poly_offset_units + gc->poly_offset_factor) / 16777215.0f;

    for (i = 0; i < 2; i++) {
        const TexUnit *tu = &gc->tu[i];
        Tex *tx = tu->enabled ? gl_bound_tex(gc, i) : NULL;
        PTex *p;
        pica_gl_texenv_t *e = &env[i];
        if (!tx || !tx->px || !tx->base)
            continue;
        p = use_texture(tx);
        if (P.state != 1)
            return;
        s->units |= 1u << i;
        if (p) {
            s->tex[i].pa = p->pa;
            s->tex[i].width = (uint16_t)p->pw;
            s->tex[i].height = (uint16_t)p->ph;
            s->tscale[i][0] = (float)p->uw / (float)p->pw;
            s->tscale[i][1] = (float)p->uh / (float)p->ph;
        } else {                        /* no GPU copy: address 0 samples opaque black */
            s->tex[i].width = s->tex[i].height = 8;
            s->tscale[i][0] = s->tscale[i][1] = 1;
        }
        s->tex[i].fmt = PICA_TEX_RGBA8;
        s->tex[i].mag = tx->mag_filter == GL_LINEAR ? PICA_LINEAR : PICA_NEAREST;
        s->tex[i].min = tx->min_filter == GL_NEAREST || tx->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                        tx->min_filter == GL_NEAREST_MIPMAP_LINEAR ? PICA_NEAREST : PICA_LINEAR;
        v = pica_gl_wrap(tx->wrap_s); s->tex[i].wrap_s = (uint8_t)(v < 0 ? PICA_REPEAT : v);
        v = pica_gl_wrap(tx->wrap_t); s->tex[i].wrap_t = (uint8_t)(v < 0 ? PICA_REPEAT : v);
        memset(e, 0, sizeof *e);
        e->mode = tu->env.mode;
        e->base_format = tx->base;
        e->env_color = rgba_u32(tu->env.color);
        e->comb_rgb = tu->env.comb_rgb;
        e->comb_a = tu->env.comb_a;
        for (v = 0; v < 3; v++) {
            e->src_rgb[v] = tu->env.src_rgb[v];
            e->src_a[v] = tu->env.src_a[v];
            e->op_rgb[v] = tu->env.op_rgb[v];
            e->op_a[v] = tu->env.op_a[v];
        }
        e->scale_rgb = (uint8_t)tu->env.rgb_scale;
        e->scale_a = (uint8_t)tu->env.a_scale;
        units[i] = e;
    }
    if (pica_ff_texenv(s->tev, units) < 0) {
        warn_once(WARN_TEXENV, "unsupported glTexEnv combination: drawn as GL_MODULATE");
        for (i = 0; i < 2; i++)
            if (units[i])
                env[i].mode = GL_MODULATE;
        pica_ff_texenv(s->tev, units);
    }
    if (gc->fog)
        warn_once(WARN_FOG, "glFog is not implemented on the GPU path yet: drawn without fog");
}

static void emit_state(const DState *s)
{
    pica_cmd_t *c = &P.cmd;
    float tm[2][16];
    int i;
    if (P.last_valid && !memcmp(s, &P.last, sizeof *s))
        return;
    pica_emit_viewport(c, s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    pica_emit_scissor(c, s->sc_on, s->sc[0], s->sc[1], s->sc[2], s->sc[3]);
    pica_emit_raster(c, &s->rs);
    for (i = 0; i < 2; i++) {
        memset(tm[i], 0, sizeof tm[i]);
        tm[i][0] = tm[i][5] = tm[i][10] = tm[i][15] = 1;
        if (s->units & (1u << i)) {
            pica_emit_texture(c, (unsigned)i, &s->tex[i]);
            tm[i][0] = s->tscale[i][0];
            tm[i][5] = s->tscale[i][1];
        }
    }
    pica_emit_texunits(c, s->units);
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(c, (unsigned)i, &s->tev[i]);
    pica_ff_emit_matrices(c, s->clip, tm[0], tm[1], 1.0f);
    P.last = *s;
    P.last_valid = 1;
}

/* the vertex format is struct Vtx itself */
static void emit_draw(const DState *s, const Vtx *v, int n)
{
    while (n > 0 && P.state == 1) {
        pica_vtx_layout_t l;
        int k;
        if (P.list_open && (P.nvtx + 3 > VTX_CAP || P.cmd.len + CMD_RESERVE > CMD_WORDS))
            flush_list();
        if (P.state != 1)
            return;
        if (!P.list_open) {
            open_list();
            memset(&l, 0, sizeof l);
            l.count = PICA_FF_ATTRS;
            l.attr[PICA_FF_POSITION].type = PICA_FLOAT;  l.attr[PICA_FF_POSITION].size = 4;
            l.attr[PICA_FF_COLOR].type = PICA_FLOAT;     l.attr[PICA_FF_COLOR].size = 4;
            l.attr[PICA_FF_TEXCOORD0].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD0].size = 2;
            l.attr[PICA_FF_TEXCOORD1].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD1].size = 2;
            l.nbuffers = 1;
            l.buffer[0].stride = sizeof(Vtx);
            l.base_pa = P.vbuf_pa;
            pica_emit_vtx_layout(&P.cmd, &l);
        }
        emit_state(s);
        k = VTX_CAP - P.nvtx;
        k -= k % 3;
        if (k > n)
            k = n;
        memcpy(P.vstage + P.nvtx, v, (size_t)k * sizeof(Vtx));
        pica_emit_draw_arrays(&P.cmd, PICA_TRIANGLES, (uint32_t)P.nvtx, (uint32_t)k);
        P.nvtx += k;
        P.draws++;
        v += k;
        n -= k;
    }
}

/* ------------------------------------------------------------------ */
/* Backend                                                             */
/* ------------------------------------------------------------------ */

static void pica_triangles(GC *gc, RT *rt, const Vtx *v, int n)
{
    PTarget *t;
    DState s;
    if (P.depth_broken && gc->depth_test && rt->depth)
        cpu_from_now("depth tests do not draw right on this GPU (depth probe)", 0);
    t = bind_target(rt);
    if (t && !t->gpu_valid)
        load_target(t);
    if (!t || P.state != 1) {
        sw_backend.triangles(gc, rt, v, n);
        return;
    }
    if (gc->cull && gc->cull_mode == GL_FRONT_AND_BACK)
        return;
    build_state(&s, gc, rt, t);
    if (P.state != 1) {
        sw_backend.triangles(gc, rt, v, n);
        return;
    }
    emit_draw(&s, v, n);
    t->dirty = 1;
}

static void pica_clear(GC *gc, RT *rt, GLbitfield mask)
{
    PTarget *t;
    int color = (mask & GL_COLOR_BUFFER_BIT) != 0, depth, full, all_ch, i;
    float cc[4];
    if (P.depth_broken && (mask & GL_DEPTH_BUFFER_BIT) && rt->depth)
        cpu_from_now("depth tests do not draw right on this GPU (depth probe)", 0);
    t = bind_target(rt);
    if (!t) {
        sw_backend.clear(gc, rt, mask);
        return;
    }
    depth = (mask & GL_DEPTH_BUFFER_BIT) && P.cur_depth && gc->depth_mask;
    if (!color && !depth)
        return;
    for (i = 0; i < 4; i++)
        cc[i] = gc->clear_color[i];
    if (rt->opaque)
        cc[3] = 1;
    full = !gc->scissor_on || (gc->scissor[0] <= 0 && gc->scissor[1] <= 0 &&
                               gc->scissor[0] + gc->scissor[2] >= t->w && gc->scissor[1] + gc->scissor[3] >= t->h);
    all_ch = gc->color_mask[0] && gc->color_mask[1] && gc->color_mask[2] && (gc->color_mask[3] || rt->opaque);

    /* Every clear is a quad at the clear depth (depth function ALWAYS). The
       PSC fill engine would be faster but cannot write FCRAM, and the arena
       is FCRAM (hardware run 13). A clear of all colour channels over the
       whole target needs no upload of the old picture first. */
    if (full && color && all_ch)
        t->gpu_valid = 1;

    /* a quad at the clear depth */
    {
        static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        static const int whole_vp[4] = { 0, 0, 0, 0 };
        DState s;
        Vtx q[6];
        int vp[4];
        float z = (gc->clear_depth < 0 ? 0 : gc->clear_depth > 1 ? 1 : gc->clear_depth) * 2.0f - 1.0f;
        (void)whole_vp;
        if (!t->gpu_valid && !load_target(t)) {
            sw_backend.clear(gc, rt, mask);
            return;
        }
        memset(&s, 0, sizeof s);
        vp[0] = vp[1] = 0; vp[2] = t->pw; vp[3] = t->ph;
        state_viewport(&s, gc, t, vp, 1, gc->scissor_on ? gc->scissor : vp);
        pica_raster_defaults(&s.rs);
        s.rs.depth_conv = (uint8_t)P.zconv;
        s.rs.depth_test = (uint8_t)depth;
        s.rs.depth_func = PICA_ALWAYS;
        s.rs.write_mask = (uint8_t)((depth ? PICA_WRITE_DEPTH : 0) |
            (!color ? 0 : (gc->color_mask[0] ? PICA_WRITE_R : 0) | (gc->color_mask[1] ? PICA_WRITE_G : 0) |
                          (gc->color_mask[2] ? PICA_WRITE_B : 0) | (gc->color_mask[3] && !rt->opaque ? PICA_WRITE_A : 0)));
        pica_ff_texenv(s.tev, (const pica_gl_texenv_t *[2]){ NULL, NULL });
        memset(q, 0, sizeof q);
        for (i = 0; i < 6; i++) {
            q[i].x = corner[i][0]; q[i].y = corner[i][1]; q[i].z = z; q[i].w = 1;
            memcpy(q[i].c, cc, sizeof cc);
            q[i].fog = 1;
        }
        emit_draw(&s, q, 6);
        if (P.state != 1) {
            sw_backend.clear(gc, rt, mask);
            return;
        }
        t->dirty = 1;
    }
}

static void pica_finish(GC *gc, RT *rt)
{
    int i;
    (void)gc;
    if (P.state != 1)
        return;
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key == rt->color && P.target[i].w == rt->w && P.target[i].h == rt->h) {
            P.target[i].rt = *rt;
            sync_target(&P.target[i]);
        }
}

static void pica_flush_all(GC *gc)
{
    int i;
    if (P.state != 1)
        return;
    /* stereo stamp: decide the frame and wipe a non-home frame's class map
       before anything is written back (docs/stereo3d.md section 4) */
    stereo_end_frame(gc);
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key && P.target[i].dirty)
            sync_target(&P.target[i]);
}

const Backend pica_backend = {
    "ios3ds PICA200",
    pica_clear,
    pica_triangles,
    pica_tex_changed,
    pica_tex_deleted,
    pica_finish,
    pica_flush_all,
};

/* CoreAnimation's compositor (SpringBoard, IOS3DS_CA=1): it may only run on
   our engine when the GPU does the work - CA's own software compositor beats
   GL on the CPU rasterizer. Decided once per process: boot.cfg (ca_gpu, gpu)
   through the GPU service, then the back end's bring-up and self-check.
   0 = refuse: QuartzCore's display probe fails and CA composites in software. */
int pica_ca_gate(void)
{
    static int decided = -1;
    uint32_t flags = 0, st;
    const pica_dev_t *dev;
    if (decided >= 0)
        return decided;
    decided = 0;
#ifdef GL_HOST
    dev = pica_dev_host();
#else
    dev = pica_dev_ios();
#endif
    if ((st = dev->config(&flags)) != PICA_OK) {
        gl_log("CA compositor: software (GPU service: %s)", pica_strerror(st));
        return 0;
    }
    if (!(flags & PICA_CFG_CA_GPU)) {
        gl_log("CA compositor: software (boot.cfg ca_gpu=0 or gpu=0)");
        return 0;
    }
    if (!pica_backend_init()) {
        gl_log("CA compositor: software (the PICA200 back end is not usable, see the pica lines above)");
        return 0;
    }
    if (!ca_scene_check()) {
        gl_log("CA compositor: software (the GPU does not draw a CA-like scene like the CPU rasterizer)");
        return 0;
    }
    gl_log("CA compositor: OpenGL ES on the PICA200 (GPU self-check and scene check passed)");
    decided = 1;
    return 1;
}

void pica_backend_stats2(PicaStats *s)
{
    s->lists = P.lists;
    s->draws = P.draws;
    s->readbacks = P.readbacks;
    s->readback_kb = P.readback_kb;
    s->uploads = P.uploads;
    s->upload_kb = P.upload_kb;
    s->presents = P.presents;
    s->bounces = P.bounces;
    s->arena_kb = P.arena_bytes >> 10;
    s->tex_kb = P.tex_bytes >> 10;
    s->ntex = P.ntex;
    s->evictions = P.evictions;
    s->gpu_on = P.state == 1;
    s->arena_free_kb = 0;
    if (P.state == 1) {
        pica_info_t info;
        memset(&info, 0, sizeof info);
        if (P.dev->info(&info) == PICA_OK)
            s->arena_free_kb = info.arena_free >> 10;
    }
}

void pica_backend_stats(unsigned *lists, unsigned *draws)
{
    *lists = P.lists;
    *draws = P.draws;
}
