/* ios3ds GL engine: internal types.
 *
 * Layering (see docs/gpu.md):
 *   api_*.c   GL ES 1.1 state machine: what OpenGLES.framework's dispatch table calls
 *   draw.c    front end: vertex fetch, transform, lighting, fog, primitive assembly.
 *             Hands *clip-space* triangles plus the GL raster state to a Backend
 *   raster.c  the software Backend (clip, viewport, triangle setup, spans)
 *   iface.c   EAGL glue: GLESGetEGLInterface, CAEAGLLayer/IOSurface drawables
 *
 * A PICA200 back end replaces raster.c by filling in another struct Backend.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ENGINE_H
#define ENGINE_H
#include "gl_defs.h"

#define MAX_TEX_UNITS 2
#define MAX_LIGHTS 8
#define MAX_CLIP_PLANES 1
#define MV_STACK 16
#define PROJ_STACK 2
#define TEX_STACK 2
#define MAX_TEX_SIZE 1024
#define MAX_PALETTE_MATRICES 16
#define MAX_MIP_LEVELS 11

typedef struct GC GC;

typedef struct Tex {
    GLuint name;
    int w, h, pitch;            /* pitch in pixels */
    GLenum base;                /* GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA; 0 = no image */
    uint32_t *px;               /* 0xAARRGGBB words (BGRA in memory), row 0 = t 0 */
    int own_px;                 /* px is malloc'ed by us (else: an IOSurface) */
    void *surface;              /* IOSurfaceRef when the image is surface backed */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    int crop[4];
    int gen_mipmap;
    uint32_t *mip[MAX_MIP_LEVELS];  /* glGenerateMipmapOES: 0 = base image */
    int n_mip;                      /* number of levels stored (1 = base only) */
    void *be_priv;              /* back end's copy (PICA200: texture in linear memory) */
    unsigned be_seq;            /* bumped whenever px changes */
    int rect;                   /* last bound as GL_TEXTURE_RECTANGLE (0x84F5): texcoords in texels */
    int stereo_opq;             /* stereo stamp: 0 unknown, 1 opaque, 2 has alpha < 255 */
} Tex;

/* QuartzCore binds IOSurface-backed images and its display surface with
   0x84F5 (GL_TEXTURE_RECTANGLE_ARB / _NV): same object namespace as 2D */
#define GL_TEXTURE_RECTANGLE_IOS 0x84F5
#define IS_TEX_TARGET(t) ((t) == GL_TEXTURE_2D || (t) == GL_TEXTURE_RECTANGLE_IOS)

typedef struct Buf {
    GLuint name;
    uint8_t *data;
    size_t size;
    GLenum usage;
    int mapped;
} Buf;

struct Drawable;
typedef struct Rb {
    GLuint name;
    int w, h, pitch;
    GLenum ifmt;
    uint32_t *color;            /* colour storage (ours or the drawable's current IOSurface) */
    uint16_t *depth;
    int own_color;
    int flip;                   /* GL row 0 is the LAST row in memory */
    struct Drawable *dr;
} Rb;

typedef struct Fb {
    GLuint name;
    GLenum color_type, depth_type;          /* 0, GL_RENDERBUFFER, GL_TEXTURE */
    GLuint color_name, depth_name, stencil_name;
} Fb;

typedef struct NameTab { void **obj; unsigned n, next; } NameTab;

typedef struct Share {
    int refs;
    NameTab tex, buf, rb, fb;
} Share;

/* The render target the back end draws into. */
typedef struct RT {
    int w, h, pitch;            /* pitch in pixels */
    uint32_t *color;
    uint16_t *depth;            /* NULL: no depth buffer */
    int dpitch;
    int flip;                   /* GL row 0 is the last row in memory */
    uint32_t opaque;            /* OR-ed into every colour written: 0xff000000
                                   for formats without alpha (RGB565, RGB8) */
    int surface;                /* color is IOSurface memory (CA's display, a drawable) */
} RT;

typedef struct Mat4 { float m[16]; } Mat4;          /* column major, like GL */

typedef struct Array {
    int enabled, size, stride;
    GLenum type;
    const void *ptr;
    GLuint buf;
} Array;

typedef struct TexEnv {
    GLenum mode;
    float color[4];
    GLenum comb_rgb, comb_a;
    GLenum src_rgb[3], src_a[3], op_rgb[3], op_a[3];
    float rgb_scale, a_scale;
} TexEnv;

typedef struct TexUnit {
    int enabled;                /* GL_TEXTURE_2D */
    GLuint bound;
    TexEnv env;
    Mat4 stack[TEX_STACK];
    int depth;
    int identity;               /* top of the stack is the identity matrix */
} TexUnit;

typedef struct Light {
    int enabled;
    float ambient[4], diffuse[4], specular[4], pos[4], spot_dir[3];
    float spot_exp, spot_cutoff, att[3];
} Light;

typedef struct Material { float ambient[4], diffuse[4], specular[4], emission[4], shininess; } Material;

/* A vertex as the back end sees it: clip coordinates, lit colour, texture
 * coordinates after the texture matrix, fog factor (1 = no fog). */
typedef struct Vtx {
    float x, y, z, w;
    float c[4];                 /* 0..1 */
    float t[MAX_TEX_UNITS][2];
    float fog;
} Vtx;

typedef struct Backend {
    const char *name;
    /* glClear: honours scissor, colour/depth masks */
    void (*clear)(GC *, RT *, GLbitfield mask);
    /* n/3 triangles in clip space; face culling, clipping, viewport and all
     * per-fragment state (gc->...) are the back end's job */
    void (*triangles)(GC *, RT *, const Vtx *v, int n);
    /* texture image changed / is going away (optional) */
    void (*tex_changed)(GC *, Tex *);
    void (*tex_deleted)(Tex *);
    /* make sure everything drawn so far is in rt->color (optional) */
    void (*finish)(GC *, RT *);
    /* glFlush / glFinish / CA's swap: write every target the back end holds
       newer than its memory back (optional; QuartzCore flushes with FBO 0
       bound and expects its IOSurfaces to be current) */
    void (*flush_all)(GC *);
} Backend;

struct GC {
    Share *share;
    void **disp_front, **disp_back;
    const Backend *be;
    GLenum error;

    /* transform */
    GLenum matrix_mode;
    Mat4 mv[MV_STACK], proj[PROJ_STACK];
    Mat4 palette[MAX_PALETTE_MATRICES];      /* OES_matrix_palette */
    int palette_cur;
    int mv_depth, proj_depth;
    Mat4 mvp;
    int mvp_dirty;
    int vp[4];
    float depth_near, depth_far;
    float clip_plane[4];
    int clip_plane_on;

    /* current values, arrays */
    float color[4], normal[3], texcoord[MAX_TEX_UNITS][4];
    Array a_vertex, a_normal, a_color, a_texcoord[MAX_TEX_UNITS], a_psize;
    Array a_matrix_idx, a_weight;            /* OES_matrix_palette */
    GLuint array_buf, elem_buf;
    int active_tex, client_tex;

    /* lighting, fog */
    int lighting, color_material, normalize, rescale_normal, two_side;
    Light light[MAX_LIGHTS];
    Material mat;
    float light_model_ambient[4];
    GLenum shade_model;
    int fog;
    GLenum fog_mode;
    float fog_density, fog_start, fog_end, fog_color[4];

    /* raster state */
    TexUnit tu[MAX_TEX_UNITS];
    int cull, scissor_on, alpha_test, blend, depth_test, dither, poly_offset_on, multisample;
    GLenum cull_mode, front_face;
    int scissor[4];
    GLenum alpha_func;
    float alpha_ref;
    GLenum blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    GLenum depth_func;
    int depth_mask;
    int color_mask[4];
    float clear_color[4], clear_depth;
    float point_size, line_width, poly_offset_factor, poly_offset_units;
    int point_sprite, point_smooth, line_smooth;
    int pack_align, unpack_align;

    /* framebuffer objects */
    GLuint fb, rb;
    Rb *default_rb;             /* BindCoreSurface target (framebuffer 0) */
    Rb *default_depth;

    /* scratch */
    Vtx *vcache;
    int vcache_n;
    void *emit;                 /* draw.c batch buffer */
    unsigned frame;
};

/* shared helpers */
void gl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gl_set_error(GC *gc, GLenum e);
void *nt_get(NameTab *t, GLuint name);
Tex *gl_bound_tex(GC *gc, int unit);
int gl_get_rt(GC *gc, RT *rt);
void gl_install_dispatch(GC *gc, void **table);
void gl_install_dispatch_native(GC *gc, void **table);
GC *gl_gc_new(Share *sh);
void gl_gc_free(GC *gc);
Share *gl_share_new(void);
void gl_share_release(Share *sh);
Rb *gl_rb_lookup(GC *gc, GLuint name);
void gl_rb_drop_storage(Rb *rb);        /* frees storage, detaches a drawable */
void gl_tex_drop_image(Tex *t);
void gl_tex_dirty(GC *gc, Tex *t);

/* iface.c hooks used by the core (no-ops in the host build) */
int  drawable_begin(GC *gc, Rb *rb);        /* make rb->color point at the buffer to draw into */
void drawable_detach(Rb *rb);
void tex_release_surface(Tex *t);
/* IOSurface-backed images change behind GL's back (camera frames, video:
   the producer writes a recycled surface and CA keeps its texture): the
   back end compares this seed and a sparse sample of the texels */
int tex_surface_seed(const Tex *t, uint32_t *seed);

extern const Backend sw_backend;
extern const Backend pica_backend;      /* pica_be.c, built with -DGL_PICA */
int pica_backend_init(void);            /* 1 = the GPU is usable */
int pica_ca_gate(void);
typedef struct {
    unsigned lists, draws, readbacks, readback_kb, uploads, upload_kb, presents, bounces;
    unsigned arena_kb, tex_kb, ntex, evictions, arena_free_kb, gpu_on;
} PicaStats;
void pica_backend_stats2(PicaStats *s);
int gl_scene_diff(int which, int *maxd);   /* ca.c: CPU vs GPU pixels off by > 24 (0 CA scene, 1 depth probe) */
int ca_scene_check(void);               /* ca.c: GPU vs CPU on a CA-like scene, 1 = close enough */
void ca_frame(void);                    /* ca.c: one CA swap (statistics) */                 /* 1 = CoreAnimation may composite with this engine (on the GPU) */

/* stereo.c: the stereoscopic 3D stamp (docs/stereo3d.md). Off unless the
   boot-args say ios3ds_stereo=1 and the process is the CA render server. */
void stereo_init(void);                 /* once per process (lazy from stereo_prepare) */
int  stereo_enabled(void);
int  stereo_prepare(GC *gc, RT *rt, const Vtx *v, int n);  /* classify one draw: class >= 0 to stamp, -1 otherwise */
int  stereo_tex_opaque(Tex *t);
void gl_be_triangles(GC *gc, RT *rt, const Vtx *v, int n, int cls);  /* pass 1 + stamp pass */
int  stereo_end_frame(GC *gc);          /* decide + wipe; 1 = a wipe was drawn (sync the target) */
void stereo_stats(unsigned *frames, unsigned *home, unsigned *flat, unsigned *stamps, unsigned *wipes);
void stereo_forget(const void *mem);     /* a target is freed: drop its pending stamps/wipe */
void stereo_batch_stats(unsigned *batches, unsigned *verts);  /* deferred stamp draws, vertices */
void pica_forget(const void *mem);
void pica_backend_stats(unsigned *lists, unsigned *draws);
const Backend *gl_pick_backend(void);
void gl_be_forget(const void *mem);
#endif
