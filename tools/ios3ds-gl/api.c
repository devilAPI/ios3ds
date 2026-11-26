/* ios3ds GL engine: the OpenGL ES 1.1 state machine.
 *
 * Every api_* function has the signature OpenGLES.framework's trampolines
 * call: the engine's GC first, then the GL arguments unchanged (softfp: floats
 * in core registers). gl_install_dispatch() puts them into the framework's
 * __GLIFunctionDispatchRec by slot number (gl_slots.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* dladdr on the build host */
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include "engine.h"
#include "gl_slots.h"
#include "pvrtc.h"

void gl_draw(GC *gc, GLenum mode, int first, int count, GLenum type, const void *indices);
void gl_draw_tex(GC *gc, float x, float y, float z, float w, float h);

/* ------------------------------------------------------------------ util */
void gl_log(const char *fmt, ...)
{
    static int fd = -2;
    char buf[256];
    va_list ap;
    int n = snprintf(buf, sizeof buf, "[gl] ");
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n++] = '\n';
#ifndef GL_HOST
    if (fd == -2)
        fd = open("/dev/console", O_WRONLY | 0x20000 /* O_NOCTTY */);
#endif
    if (fd < 0)
        fd = 2;
    write(fd, buf, n);
}

/* IOS3DS_GL_TRACE=1: log the first error of every call site (address
   relative to the engine's load address: look it up with nm) */
void gl_set_error(GC *gc, GLenum e)
{
    static int trace = -1, n;
    static const void *seen[64];
    if (trace < 0)
        trace = getenv("IOS3DS_GL_TRACE") != NULL;
    if (trace && n < 64) {
        const void *ra = __builtin_return_address(0);
        Dl_info di;
        int k;
        for (k = 0; k < n && seen[k] != ra; k++)
            ;
        if (k == n) {
            seen[n++] = ra;
            gl_log("trace: GL error 0x%x at engine+0x%lx", e,
                   dladdr(ra, &di) ? (unsigned long)((const char *)ra - (const char *)di.dli_fbase) : 0ul);
        }
    }
    if (gc->error == GL_NO_ERROR)
        gc->error = e;
}

void *nt_get(NameTab *t, GLuint name)
{
    return name < t->n ? t->obj[name] : NULL;
}

static int nt_set(NameTab *t, GLuint name, void *o)
{
    if (name >= t->n) {
        unsigned n = t->n ? t->n : 64;
        void **p;
        if (name >= (1u << 20))
            return 0;
        while (n <= name)
            n *= 2;
        p = realloc(t->obj, n * sizeof *p);
        if (!p)
            return 0;
        memset(p + t->n, 0, (n - t->n) * sizeof *p);
        t->obj = p;
        t->n = n;
    }
    t->obj[name] = o;
    return 1;
}

static GLuint nt_free_name(NameTab *t)
{
    GLuint i = t->next ? t->next : 1;
    while (i < t->n && t->obj[i])
        i++;
    t->next = i + 1;
    return i;
}

static float x2f(GLfixed x) { return (float)x * (1.0f / 65536.0f); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* --------------------------------------------------------------- objects */
static Tex *tex_new(GLuint name)
{
    Tex *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->name = name;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    return t;
}

/* Back ends that keep copies keyed on engine memory (the PICA200 one: render
   targets in GPU memory) must drop them before that memory goes away. */
void gl_be_forget(const void *mem)
{
#ifdef GL_PICA
    pica_forget(mem);
#else
    (void)mem;
#endif
}

static void be_tex_deleted(Tex *t)
{
#ifdef GL_PICA
    if (t->be_priv)
        pica_backend.tex_deleted(t);
#else
    (void)t;
#endif
}

/* IOS3DS_GL=pica|auto selects the GPU back end when the GPU is usable;
   anything else (and any failure) means the software rasterizer. */
const Backend *gl_pick_backend(void)
{
#ifdef GL_PICA
    const char *e = getenv("IOS3DS_GL");
    if (getenv("IOS3DS_CA"))
        e = "pica";                     /* CoreAnimation's compositor: only on the GPU (pica_ca_gate) */
    if (e && (!strcmp(e, "pica") || !strcmp(e, "auto"))) {
        if (pica_backend_init())
            return &pica_backend;
        gl_log("IOS3DS_GL=%s but the GPU back end is not usable: software rasterizer", e);
    }
#endif
    return &sw_backend;
}

void gl_tex_drop_image(Tex *t)
{
    int i;
    gl_be_forget(t->px);
    if (t->surface)
        tex_release_surface(t);
    else if (t->own_px)
        free(t->px);
    for (i = 1; i < t->n_mip; i++)
        free(t->mip[i]);
    t->px = NULL;
    t->own_px = 0;
    t->surface = NULL;
    t->w = t->h = t->pitch = 0;
    t->base = 0;
    t->stereo_opq = 0;
    t->stereo_cls = 0;
    for (i = 0; i < MAX_MIP_LEVELS; i++)
        t->mip[i] = NULL;
    t->n_mip = 0;
}

static Tex *tex_lookup(GC *gc, GLuint name, int create)
{
    Tex *t = nt_get(&gc->share->tex, name);
    if (!t && create) {
        t = tex_new(name);
        if (t && !nt_set(&gc->share->tex, name, t)) {
            free(t);
            t = NULL;
        }
    }
    return t;
}

Tex *gl_bound_tex(GC *gc, int unit)
{
    return tex_lookup(gc, gc->tu[unit].bound, 1);
}

Rb *gl_rb_lookup(GC *gc, GLuint name)
{
    return name ? nt_get(&gc->share->rb, name) : NULL;
}

void gl_rb_drop_storage(Rb *rb)
{
    gl_be_forget(rb->color);
    gl_be_forget(rb->depth);
    if (rb->dr)
        drawable_detach(rb);
    if (rb->own_color)
        free(rb->color);
    free(rb->depth);
    rb->color = NULL;
    rb->depth = NULL;
    rb->own_color = 0;
    rb->w = rb->h = rb->pitch = 0;
}

Share *gl_share_new(void)
{
    Share *sh = calloc(1, sizeof *sh);
    if (!sh)
        return NULL;
    sh->refs = 1;
    nt_set(&sh->tex, 0, tex_new(0));            /* the default texture object */
    return sh;
}

void gl_share_release(Share *sh)
{
    unsigned i;
    if (--sh->refs > 0)
        return;
    for (i = 0; i < sh->tex.n; i++)
        if (sh->tex.obj[i]) {
            Tex *t = sh->tex.obj[i];
            be_tex_deleted(t);
            gl_tex_drop_image(t);
            free(t);
        }
    for (i = 0; i < sh->buf.n; i++)
        if (sh->buf.obj[i]) {
            free(((Buf *)sh->buf.obj[i])->data);
            free(sh->buf.obj[i]);
        }
    for (i = 0; i < sh->rb.n; i++)
        if (sh->rb.obj[i]) {
            gl_rb_drop_storage(sh->rb.obj[i]);
            free(sh->rb.obj[i]);
        }
    for (i = 0; i < sh->fb.n; i++)
        free(sh->fb.obj[i]);
    free(sh->tex.obj);
    free(sh->buf.obj);
    free(sh->rb.obj);
    free(sh->fb.obj);
    free(sh);
}

/* ---------------------------------------------------------------- matrix */
static const Mat4 mat_identity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};

static void mat_mul(Mat4 *r, const Mat4 *a, const Mat4 *b)     /* r = a * b */
{
    Mat4 t;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t.m[i * 4 + j] = a->m[j] * b->m[i * 4] + a->m[4 + j] * b->m[i * 4 + 1] +
                             a->m[8 + j] * b->m[i * 4 + 2] + a->m[12 + j] * b->m[i * 4 + 3];
    *r = t;
}

static Mat4 *cur_matrix(GC *gc)
{
    TexUnit *u;
    switch (gc->matrix_mode) {
    case GL_PROJECTION:
        return &gc->proj[gc->proj_depth];
    case GL_TEXTURE:
        u = &gc->tu[gc->active_tex];
        return &u->stack[u->depth];
    default:
        return &gc->mv[gc->mv_depth];
    }
}

static void mat_touch(GC *gc)
{
    if (gc->matrix_mode == GL_TEXTURE) {
        TexUnit *u = &gc->tu[gc->active_tex];
        u->identity = !memcmp(&u->stack[u->depth], &mat_identity, sizeof(Mat4));
    } else
        gc->mvp_dirty = 1;
}

static void mat_apply(GC *gc, const Mat4 *m)
{
    Mat4 *c = cur_matrix(gc);
    mat_mul(c, c, m);
    mat_touch(gc);
}

static void api_MatrixMode(GC *gc, GLenum mode)
{
    if (mode < GL_MODELVIEW || mode > GL_TEXTURE)
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        gc->matrix_mode = mode;
}

static void api_LoadIdentity(GC *gc)
{
    *cur_matrix(gc) = mat_identity;
    mat_touch(gc);
}

static void api_LoadMatrixf(GC *gc, const GLfloat *m)
{
    memcpy(cur_matrix(gc)->m, m, sizeof(Mat4));
    mat_touch(gc);
}

static void api_MultMatrixf(GC *gc, const GLfloat *m)
{
    Mat4 t;
    memcpy(t.m, m, sizeof t);
    mat_apply(gc, &t);
}

static void api_LoadMatrixx(GC *gc, const GLfixed *m)
{
    Mat4 t;
    int i;
    for (i = 0; i < 16; i++)
        t.m[i] = x2f(m[i]);
    api_LoadMatrixf(gc, t.m);
}

static void api_MultMatrixx(GC *gc, const GLfixed *m)
{
    Mat4 t;
    int i;
    for (i = 0; i < 16; i++)
        t.m[i] = x2f(m[i]);
    mat_apply(gc, &t);
}

static void api_PushMatrix(GC *gc)
{
    TexUnit *u = &gc->tu[gc->active_tex];
    int *d, max;
    Mat4 *s;
    switch (gc->matrix_mode) {
    case GL_PROJECTION: d = &gc->proj_depth; max = PROJ_STACK; s = gc->proj; break;
    case GL_TEXTURE:    d = &u->depth; max = TEX_STACK; s = u->stack; break;
    default:            d = &gc->mv_depth; max = MV_STACK; s = gc->mv; break;
    }
    if (*d + 1 >= max) {
        gl_set_error(gc, GL_STACK_OVERFLOW);
        return;
    }
    s[*d + 1] = s[*d];
    ++*d;
}

static void api_PopMatrix(GC *gc)
{
    TexUnit *u = &gc->tu[gc->active_tex];
    int *d = gc->matrix_mode == GL_PROJECTION ? &gc->proj_depth :
             gc->matrix_mode == GL_TEXTURE ? &u->depth : &gc->mv_depth;
    if (*d == 0) {
        gl_set_error(gc, GL_STACK_UNDERFLOW);
        return;
    }
    --*d;
    mat_touch(gc);
}

static void api_Translatef(GC *gc, GLfloat x, GLfloat y, GLfloat z)
{
    Mat4 *m = cur_matrix(gc);
    int i;
    for (i = 0; i < 4; i++)
        m->m[12 + i] += m->m[i] * x + m->m[4 + i] * y + m->m[8 + i] * z;
    mat_touch(gc);
}

static void api_Scalef(GC *gc, GLfloat x, GLfloat y, GLfloat z)
{
    Mat4 *m = cur_matrix(gc);
    int i;
    for (i = 0; i < 4; i++) {
        m->m[i] *= x;
        m->m[4 + i] *= y;
        m->m[8 + i] *= z;
    }
    mat_touch(gc);
}

static void api_Rotatef(GC *gc, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    Mat4 r = mat_identity;
    float len = sqrtf(x * x + y * y + z * z), s, c, t;
    if (len == 0)
        return;
    x /= len; y /= len; z /= len;
    s = sinf(angle * (float)(M_PI / 180));
    c = cosf(angle * (float)(M_PI / 180));
    t = 1 - c;
    r.m[0] = x * x * t + c;     r.m[4] = x * y * t - z * s; r.m[8] = x * z * t + y * s;
    r.m[1] = y * x * t + z * s; r.m[5] = y * y * t + c;     r.m[9] = y * z * t - x * s;
    r.m[2] = z * x * t - y * s; r.m[6] = z * y * t + x * s; r.m[10] = z * z * t + c;
    mat_apply(gc, &r);
}

static void api_Frustumf(GC *gc, GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
    Mat4 m;
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    memset(&m, 0, sizeof m);
    m.m[0] = 2 * n / (r - l);
    m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    mat_apply(gc, &m);
}

static void api_Orthof(GC *gc, GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
    Mat4 m = mat_identity;
    if (l == r || b == t || n == f) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    mat_apply(gc, &m);
}

static void api_Translatex(GC *gc, GLfixed x, GLfixed y, GLfixed z) { api_Translatef(gc, x2f(x), x2f(y), x2f(z)); }
static void api_Scalex(GC *gc, GLfixed x, GLfixed y, GLfixed z) { api_Scalef(gc, x2f(x), x2f(y), x2f(z)); }
static void api_Rotatex(GC *gc, GLfixed a, GLfixed x, GLfixed y, GLfixed z) { api_Rotatef(gc, x2f(a), x2f(x), x2f(y), x2f(z)); }
static void api_Frustumx(GC *gc, GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
{
    api_Frustumf(gc, x2f(l), x2f(r), x2f(b), x2f(t), x2f(n), x2f(f));
}
static void api_Orthox(GC *gc, GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
{
    api_Orthof(gc, x2f(l), x2f(r), x2f(b), x2f(t), x2f(n), x2f(f));
}

/* ------------------------------------------------------- simple state */
static void api_Viewport(GC *gc, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (w < 0 || h < 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    gc->vp[0] = x; gc->vp[1] = y; gc->vp[2] = w; gc->vp[3] = h;
}

static void api_Scissor(GC *gc, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (w < 0 || h < 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    gc->scissor[0] = x; gc->scissor[1] = y; gc->scissor[2] = w; gc->scissor[3] = h;
}

static void api_DepthRangef(GC *gc, GLclampf n, GLclampf f)
{
    gc->depth_near = clampf(n, 0, 1);
    gc->depth_far = clampf(f, 0, 1);
}
static void api_DepthRangex(GC *gc, GLfixed n, GLfixed f) { api_DepthRangef(gc, x2f(n), x2f(f)); }

static void api_ClearColor(GC *gc, GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    gc->clear_color[0] = clampf(r, 0, 1);
    gc->clear_color[1] = clampf(g, 0, 1);
    gc->clear_color[2] = clampf(b, 0, 1);
    gc->clear_color[3] = clampf(a, 0, 1);
}
static void api_ClearColorx(GC *gc, GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{
    api_ClearColor(gc, x2f(r), x2f(g), x2f(b), x2f(a));
}
static void api_ClearDepthf(GC *gc, GLclampf d) { gc->clear_depth = clampf(d, 0, 1); }
static void api_ClearDepthx(GC *gc, GLfixed d) { api_ClearDepthf(gc, x2f(d)); }
static void api_ClearStencil(GC *gc, GLint s) { (void)gc; (void)s; }

static void api_Color4f(GC *gc, GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    gc->color[0] = r; gc->color[1] = g; gc->color[2] = b; gc->color[3] = a;
}
static void api_Color4ub(GC *gc, GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    api_Color4f(gc, r * (1 / 255.0f), g * (1 / 255.0f), b * (1 / 255.0f), a * (1 / 255.0f));
}
static void api_Color4x(GC *gc, GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{
    api_Color4f(gc, x2f(r), x2f(g), x2f(b), x2f(a));
}
static void api_Normal3f(GC *gc, GLfloat x, GLfloat y, GLfloat z)
{
    gc->normal[0] = x; gc->normal[1] = y; gc->normal[2] = z;
}
static void api_Normal3x(GC *gc, GLfixed x, GLfixed y, GLfixed z) { api_Normal3f(gc, x2f(x), x2f(y), x2f(z)); }

static void api_MultiTexCoord4f(GC *gc, GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    unsigned u = target - GL_TEXTURE0;
    if (u >= MAX_TEX_UNITS) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    gc->texcoord[u][0] = s; gc->texcoord[u][1] = t; gc->texcoord[u][2] = r; gc->texcoord[u][3] = q;
}
static void api_MultiTexCoord4x(GC *gc, GLenum target, GLfixed s, GLfixed t, GLfixed r, GLfixed q)
{
    api_MultiTexCoord4f(gc, target, x2f(s), x2f(t), x2f(r), x2f(q));
}

static int valid_func(GLenum f) { return f >= GL_NEVER && f <= GL_ALWAYS; }

static void api_AlphaFunc(GC *gc, GLenum func, GLclampf ref)
{
    if (!valid_func(func)) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    gc->alpha_func = func;
    gc->alpha_ref = clampf(ref, 0, 1);
}
static void api_AlphaFuncx(GC *gc, GLenum func, GLfixed ref) { api_AlphaFunc(gc, func, x2f(ref)); }

static void api_DepthFunc(GC *gc, GLenum func)
{
    if (!valid_func(func))
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        gc->depth_func = func;
}
static void api_DepthMask(GC *gc, GLboolean m) { gc->depth_mask = !!m; }
static void api_ColorMask(GC *gc, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    gc->color_mask[0] = !!r; gc->color_mask[1] = !!g; gc->color_mask[2] = !!b; gc->color_mask[3] = !!a;
}

static void api_BlendFuncSeparate(GC *gc, GLenum srgb, GLenum drgb, GLenum sa, GLenum da)
{
    gc->blend_src_rgb = srgb; gc->blend_dst_rgb = drgb;
    gc->blend_src_a = sa; gc->blend_dst_a = da;
}
static void api_BlendFunc(GC *gc, GLenum s, GLenum d) { api_BlendFuncSeparate(gc, s, d, s, d); }
static void api_BlendEquationSeparate(GC *gc, GLenum rgb, GLenum a)
{
    gc->blend_eq_rgb = rgb;
    gc->blend_eq_a = a;
}
static void api_BlendEquation(GC *gc, GLenum m) { api_BlendEquationSeparate(gc, m, m); }
static void api_BlendColor(GC *gc, GLclampf r, GLclampf g, GLclampf b, GLclampf a) { (void)gc; (void)r; (void)g; (void)b; (void)a; }

static void api_CullFace(GC *gc, GLenum m) { gc->cull_mode = m; }
static void api_FrontFace(GC *gc, GLenum m) { gc->front_face = m; }
static void api_ShadeModel(GC *gc, GLenum m) { gc->shade_model = m; }
static void api_PointSize(GC *gc, GLfloat s) { gc->point_size = s > 0 ? s : 1; }
static void api_PointSizex(GC *gc, GLfixed s) { api_PointSize(gc, x2f(s)); }
static void api_LineWidth(GC *gc, GLfloat w) { gc->line_width = w > 0 ? w : 1; }
static void api_LineWidthx(GC *gc, GLfixed w) { api_LineWidth(gc, x2f(w)); }
static void api_PolygonOffset(GC *gc, GLfloat factor, GLfloat units)
{
    gc->poly_offset_factor = factor;
    gc->poly_offset_units = units;
}
static void api_PolygonOffsetx(GC *gc, GLfixed f, GLfixed u) { api_PolygonOffset(gc, x2f(f), x2f(u)); }
static void api_Hint(GC *gc, GLenum t, GLenum m) { (void)gc; (void)t; (void)m; }
static void api_LogicOp(GC *gc, GLenum op) { (void)gc; (void)op; }
static void api_SampleCoverage(GC *gc, GLclampf v, GLboolean inv) { (void)gc; (void)v; (void)inv; }
static void api_SampleCoveragex(GC *gc, GLfixed v, GLboolean inv) { (void)gc; (void)v; (void)inv; }
static void api_StencilFunc(GC *gc, GLenum f, GLint r, GLuint m) { (void)gc; (void)f; (void)r; (void)m; }
static void api_StencilMask(GC *gc, GLuint m) { (void)gc; (void)m; }
static void api_StencilOp(GC *gc, GLenum a, GLenum b, GLenum c) { (void)gc; (void)a; (void)b; (void)c; }
static void api_PointParameterf(GC *gc, GLenum p, GLfloat v) { (void)gc; (void)p; (void)v; }
static void api_PointParameterfv(GC *gc, GLenum p, const GLfloat *v) { (void)gc; (void)p; (void)v; }
static void api_PointParameterx(GC *gc, GLenum p, GLfixed v) { (void)gc; (void)p; (void)v; }
static void api_PointParameterxv(GC *gc, GLenum p, const GLfixed *v) { (void)gc; (void)p; (void)v; }

static void api_ClipPlanef(GC *gc, GLenum plane, const GLfloat *eq)
{
    if (plane != GL_CLIP_PLANE0) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    memcpy(gc->clip_plane, eq, sizeof gc->clip_plane);  /* accepted, not applied */
}
static void api_ClipPlanex(GC *gc, GLenum plane, const GLfixed *eq)
{
    GLfloat f[4] = {x2f(eq[0]), x2f(eq[1]), x2f(eq[2]), x2f(eq[3])};
    api_ClipPlanef(gc, plane, f);
}

static void api_PixelStorei(GC *gc, GLenum pname, GLint v)
{
    if (pname == 0x85B2)                /* GL_UNPACK_CLIENT_STORAGE_APPLE: we always copy */
        return;
    if (v != 1 && v != 2 && v != 4 && v != 8)
        gl_set_error(gc, GL_INVALID_VALUE);
    else if (pname == GL_UNPACK_ALIGNMENT)
        gc->unpack_align = v;
    else if (pname == GL_PACK_ALIGNMENT)
        gc->pack_align = v;
    else
        gl_set_error(gc, GL_INVALID_ENUM);
}

static int *cap_ptr(GC *gc, GLenum cap)
{
    static int dummy;
    switch (cap) {
    case GL_TEXTURE_2D:
    case GL_TEXTURE_RECTANGLE_IOS: return &gc->tu[gc->active_tex].enabled;
    case GL_CULL_FACE:          return &gc->cull;
    case GL_SCISSOR_TEST:       return &gc->scissor_on;
    case GL_ALPHA_TEST:         return &gc->alpha_test;
    case GL_BLEND:              return &gc->blend;
    case GL_DEPTH_TEST:         return &gc->depth_test;
    case GL_DITHER:             return &gc->dither;
    case GL_LIGHTING:           return &gc->lighting;
    case GL_COLOR_MATERIAL:     return &gc->color_material;
    case GL_NORMALIZE:          return &gc->normalize;
    case GL_RESCALE_NORMAL:     return &gc->rescale_normal;
    case GL_FOG:                return &gc->fog;
    case GL_POLYGON_OFFSET_FILL: return &gc->poly_offset_on;
    case GL_MULTISAMPLE:        return &gc->multisample;
    case GL_POINT_SPRITE_OES:   return &gc->point_sprite;
    case GL_POINT_SMOOTH:       return &gc->point_smooth;
    case GL_LINE_SMOOTH:        return &gc->line_smooth;
    case GL_CLIP_PLANE0:        return &gc->clip_plane_on;
    case GL_STENCIL_TEST: case GL_COLOR_LOGIC_OP: case GL_SAMPLE_ALPHA_TO_COVERAGE:
    case GL_SAMPLE_ALPHA_TO_ONE: case GL_SAMPLE_COVERAGE:
        dummy = 0;
        return &dummy;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS)
        return &gc->light[cap - GL_LIGHT0].enabled;
    return NULL;
}

static Array *client_array(GC *gc, GLenum a)
{
    switch (a) {
    case GL_VERTEX_ARRAY:           return &gc->a_vertex;
    case GL_NORMAL_ARRAY:           return &gc->a_normal;
    case GL_COLOR_ARRAY:            return &gc->a_color;
    case GL_TEXTURE_COORD_ARRAY:    return &gc->a_texcoord[gc->client_tex];
    case GL_POINT_SIZE_ARRAY_OES:   return &gc->a_psize;
    case GL_MATRIX_INDEX_ARRAY_OES: return &gc->a_matrix_idx;
    case GL_WEIGHT_ARRAY_OES:       return &gc->a_weight;
    }
    return NULL;
}

static void set_cap(GC *gc, GLenum cap, int on)
{
    int *p = cap_ptr(gc, cap);
    if (p)
        *p = on;
    else
        gl_set_error(gc, GL_INVALID_ENUM);
}
static void api_Enable(GC *gc, GLenum cap) { set_cap(gc, cap, 1); }
static void api_Disable(GC *gc, GLenum cap) { set_cap(gc, cap, 0); }

static GLboolean api_IsEnabled(GC *gc, GLenum cap)
{
    int *p = cap_ptr(gc, cap);
    Array *a;
    if (p)
        return *p != 0;
    a = client_array(gc, cap);
    if (a)
        return a->enabled != 0;
    gl_set_error(gc, GL_INVALID_ENUM);
    return 0;
}

static void set_client(GC *gc, GLenum a, int on)
{
    Array *p = client_array(gc, a);
    if (p)
        p->enabled = on;
    else
        gl_set_error(gc, GL_INVALID_ENUM);
}
static void api_EnableClientState(GC *gc, GLenum a) { set_client(gc, a, 1); }
static void api_DisableClientState(GC *gc, GLenum a) { set_client(gc, a, 0); }

static void api_ActiveTexture(GC *gc, GLenum t)
{
    if (t - GL_TEXTURE0 >= MAX_TEX_UNITS)
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        gc->active_tex = t - GL_TEXTURE0;
}
static void api_ClientActiveTexture(GC *gc, GLenum t)
{
    if (t - GL_TEXTURE0 >= MAX_TEX_UNITS)
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        gc->client_tex = t - GL_TEXTURE0;
}

static void set_array(GC *gc, Array *a, int size, GLenum type, int stride, const void *ptr)
{
    if (stride < 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    a->size = size; a->type = type; a->stride = stride; a->ptr = ptr; a->buf = gc->array_buf;
}
static void api_VertexPointer(GC *gc, GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(gc, &gc->a_vertex, size, type, stride, p); }
static void api_ColorPointer(GC *gc, GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(gc, &gc->a_color, size, type, stride, p); }
static void api_TexCoordPointer(GC *gc, GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(gc, &gc->a_texcoord[gc->client_tex], size, type, stride, p); }
static void api_NormalPointer(GC *gc, GLenum type, GLsizei stride, const GLvoid *p) { set_array(gc, &gc->a_normal, 3, type, stride, p); }
static void api_PointSizePointerOES(GC *gc, GLenum type, GLsizei stride, const GLvoid *p) { set_array(gc, &gc->a_psize, 1, type, stride, p); }
static void api_MatrixIndexPointerOES(GC *gc, GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{
    if (size < 1 || size > 4) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    set_array(gc, &gc->a_matrix_idx, size, type, stride, p);
}
static void api_WeightPointerOES(GC *gc, GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{
    if (size < 1 || size > 4) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    set_array(gc, &gc->a_weight, size, type, stride, p);
}
static void api_CurrentPaletteMatrixOES(GC *gc, GLuint matrix)
{
    if (matrix >= MAX_PALETTE_MATRICES) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    gc->palette_cur = (int)matrix;
}
static void api_LoadPaletteFromModelViewMatrixOES(GC *gc)
{
    gc->palette[gc->palette_cur] = gc->mv[gc->mv_depth];
}

/* --------------------------------------------------------- lighting, fog */
static void xform_dir(const Mat4 *m, const float *in, float *out)
{
    out[0] = m->m[0] * in[0] + m->m[4] * in[1] + m->m[8] * in[2];
    out[1] = m->m[1] * in[0] + m->m[5] * in[1] + m->m[9] * in[2];
    out[2] = m->m[2] * in[0] + m->m[6] * in[1] + m->m[10] * in[2];
}

static void api_Lightfv(GC *gc, GLenum light, GLenum pname, const GLfloat *v)
{
    Light *l;
    const Mat4 *mv = &gc->mv[gc->mv_depth];
    int i;
    if (light - GL_LIGHT0 >= MAX_LIGHTS) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    l = &gc->light[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT:  memcpy(l->ambient, v, 16); break;
    case GL_DIFFUSE:  memcpy(l->diffuse, v, 16); break;
    case GL_SPECULAR: memcpy(l->specular, v, 16); break;
    case GL_POSITION:
        for (i = 0; i < 4; i++)
            l->pos[i] = mv->m[i] * v[0] + mv->m[4 + i] * v[1] + mv->m[8 + i] * v[2] + mv->m[12 + i] * v[3];
        break;
    case GL_SPOT_DIRECTION: xform_dir(mv, v, l->spot_dir); break;
    case GL_SPOT_EXPONENT:  l->spot_exp = v[0]; break;
    case GL_SPOT_CUTOFF:    l->spot_cutoff = v[0]; break;
    case GL_CONSTANT_ATTENUATION:  l->att[0] = v[0]; break;
    case GL_LINEAR_ATTENUATION:    l->att[1] = v[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->att[2] = v[0]; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}
static void api_Lightf(GC *gc, GLenum light, GLenum pname, GLfloat v) { api_Lightfv(gc, light, pname, &v); }

static int nparams(GLenum pname)
{
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR:
    case GL_TEXTURE_CROP_RECT_OES:
        return 4;
    case GL_SPOT_DIRECTION:
        return 3;
    }
    return 1;
}

static void x2fv(GLenum pname, const GLfixed *in, GLfloat *out)
{
    int i, n = nparams(pname);
    for (i = 0; i < n; i++)
        out[i] = x2f(in[i]);
}

static void api_Lightxv(GC *gc, GLenum light, GLenum pname, const GLfixed *v)
{
    GLfloat f[4];
    x2fv(pname, v, f);
    api_Lightfv(gc, light, pname, f);
}
static void api_Lightx(GC *gc, GLenum light, GLenum pname, GLfixed v) { api_Lightf(gc, light, pname, x2f(v)); }

static void api_Materialfv(GC *gc, GLenum face, GLenum pname, const GLfloat *v)
{
    Material *m = &gc->mat;
    (void)face;
    switch (pname) {
    case GL_AMBIENT:  memcpy(m->ambient, v, 16); break;
    case GL_DIFFUSE:  memcpy(m->diffuse, v, 16); break;
    case GL_AMBIENT_AND_DIFFUSE: memcpy(m->ambient, v, 16); memcpy(m->diffuse, v, 16); break;
    case GL_SPECULAR: memcpy(m->specular, v, 16); break;
    case GL_EMISSION: memcpy(m->emission, v, 16); break;
    case GL_SHININESS: m->shininess = v[0]; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}
static void api_Materialf(GC *gc, GLenum face, GLenum pname, GLfloat v) { api_Materialfv(gc, face, pname, &v); }
static void api_Materialxv(GC *gc, GLenum face, GLenum pname, const GLfixed *v)
{
    GLfloat f[4];
    x2fv(pname, v, f);
    api_Materialfv(gc, face, pname, f);
}
static void api_Materialx(GC *gc, GLenum face, GLenum pname, GLfixed v) { api_Materialf(gc, face, pname, x2f(v)); }

static void api_LightModelfv(GC *gc, GLenum pname, const GLfloat *v)
{
    if (pname == GL_LIGHT_MODEL_AMBIENT)
        memcpy(gc->light_model_ambient, v, 16);
    else if (pname == GL_LIGHT_MODEL_TWO_SIDE)
        gc->two_side = v[0] != 0;
    else
        gl_set_error(gc, GL_INVALID_ENUM);
}
static void api_LightModelf(GC *gc, GLenum pname, GLfloat v)
{
    if (pname == GL_LIGHT_MODEL_AMBIENT)        /* vector-only parameter */
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        api_LightModelfv(gc, pname, &v);
}
static void api_LightModelxv(GC *gc, GLenum pname, const GLfixed *v)
{
    GLfloat f[4];
    x2fv(pname, v, f);
    api_LightModelfv(gc, pname, f);
}
static void api_LightModelx(GC *gc, GLenum pname, GLfixed v) { api_LightModelf(gc, pname, x2f(v)); }

static void api_Fogfv(GC *gc, GLenum pname, const GLfloat *v)
{
    switch (pname) {
    case GL_FOG_MODE:    gc->fog_mode = (GLenum)v[0]; break;
    case GL_FOG_DENSITY: gc->fog_density = v[0]; break;
    case GL_FOG_START:   gc->fog_start = v[0]; break;
    case GL_FOG_END:     gc->fog_end = v[0]; break;
    case GL_FOG_COLOR:   memcpy(gc->fog_color, v, 16); break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}
static void api_Fogf(GC *gc, GLenum pname, GLfloat v) { api_Fogfv(gc, pname, &v); }
static void api_Fogxv(GC *gc, GLenum pname, const GLfixed *v)
{
    GLfloat f[4];
    if (pname == GL_FOG_MODE)
        f[0] = (float)v[0];
    else
        x2fv(pname, v, f);
    api_Fogfv(gc, pname, f);
}
static void api_Fogx(GC *gc, GLenum pname, GLfixed v) { api_Fogxv(gc, pname, &v); }

/* -------------------------------------------------------------- textures */
static void api_GenTextures(GC *gc, GLsizei n, GLuint *out)
{
    int i;
    for (i = 0; i < n; i++) {
        out[i] = nt_free_name(&gc->share->tex);
        tex_lookup(gc, out[i], 1);
    }
}

static void api_DeleteTextures(GC *gc, GLsizei n, const GLuint *names)
{
    int i, u;
    for (i = 0; i < n; i++) {
        Tex *t = names[i] ? nt_get(&gc->share->tex, names[i]) : NULL;
        if (!t)
            continue;
        for (u = 0; u < MAX_TEX_UNITS; u++)
            if (gc->tu[u].bound == names[i])
                gc->tu[u].bound = 0;
        be_tex_deleted(t);
        gl_tex_drop_image(t);
        free(t);
        gc->share->tex.obj[names[i]] = NULL;
        if (names[i] < gc->share->tex.next)
            gc->share->tex.next = names[i];
    }
}

static void api_BindTexture(GC *gc, GLenum target, GLuint name)
{
    Tex *t;
    if (!IS_TEX_TARGET(target)) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (!(t = tex_lookup(gc, name, 1))) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return;
    }
    if (name)
        t->rect = target == GL_TEXTURE_RECTANGLE_IOS && !getenv("IOS3DS_GL_RECT_NORMALIZED");
    gc->tu[gc->active_tex].bound = name;
}

static GLboolean api_IsTexture(GC *gc, GLuint name) { return name && nt_get(&gc->share->tex, name) != NULL; }

static int src_bpp(GLenum format, GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
        switch (format) {
        case GL_RGBA: case GL_BGRA_EXT: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_ALPHA: case GL_LUMINANCE: return 1;
        }
        return 0;
    case GL_UNSIGNED_INT_8_8_8_8_REV:
        return format == GL_BGRA_EXT || format == GL_RGBA ? 4 : 0;
    case GL_UNSIGNED_SHORT_5_6_5:
        return format == GL_RGB ? 2 : 0;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
        return format == GL_RGBA ? 2 : 0;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        return format == GL_BGRA_EXT ? 2 : 0;
    }
    return 0;
}

#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
static inline unsigned ex5(unsigned v) { return (v << 3) | (v >> 2); }
static inline unsigned ex6(unsigned v) { return (v << 2) | (v >> 4); }
static inline unsigned ex4(unsigned v) { return (v << 4) | v; }

static void unpack_row(uint32_t *d, const uint8_t *s, int n, GLenum format, GLenum type)
{
    int i;
    unsigned v;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
        for (i = 0; i < n; i++, s += 2) {
            v = s[0] | (s[1] << 8);
            d[i] = ARGB(255, ex5(v >> 11), ex6((v >> 5) & 63), ex5(v & 31));
        }
        return;
    case GL_UNSIGNED_SHORT_4_4_4_4:
        for (i = 0; i < n; i++, s += 2) {
            v = s[0] | (s[1] << 8);
            d[i] = ARGB(ex4(v & 15), ex4(v >> 12), ex4((v >> 8) & 15), ex4((v >> 4) & 15));
        }
        return;
    case GL_UNSIGNED_SHORT_5_5_5_1:
        for (i = 0; i < n; i++, s += 2) {
            v = s[0] | (s[1] << 8);
            d[i] = ARGB((v & 1) ? 255 : 0, ex5(v >> 11), ex5((v >> 6) & 31), ex5((v >> 1) & 31));
        }
        return;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:     /* BGRA: A1 R5 G5 B5 */
        for (i = 0; i < n; i++, s += 2) {
            v = s[0] | (s[1] << 8);
            d[i] = ARGB((v >> 15) ? 255 : 0, ex5((v >> 10) & 31), ex5((v >> 5) & 31), ex5(v & 31));
        }
        return;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:     /* BGRA: A4 R4 G4 B4 */
        for (i = 0; i < n; i++, s += 2) {
            v = s[0] | (s[1] << 8);
            d[i] = ARGB(ex4(v >> 12), ex4((v >> 8) & 15), ex4((v >> 4) & 15), ex4(v & 15));
        }
        return;
    }
    switch (format) {
    case GL_BGRA_EXT:
        memcpy(d, s, (size_t)n * 4);
        return;
    case GL_RGBA:
        for (i = 0; i < n; i++, s += 4)
            d[i] = ARGB(s[3], s[0], s[1], s[2]);
        return;
    case GL_RGB:
        for (i = 0; i < n; i++, s += 3)
            d[i] = ARGB(255, s[0], s[1], s[2]);
        return;
    case GL_LUMINANCE:
        for (i = 0; i < n; i++)
            d[i] = ARGB(255, s[i], s[i], s[i]);
        return;
    case GL_LUMINANCE_ALPHA:
        for (i = 0; i < n; i++, s += 2)
            d[i] = ARGB(s[1], s[0], s[0], s[0]);
        return;
    case GL_ALPHA:
        for (i = 0; i < n; i++)
            d[i] = ARGB(s[i], 0, 0, 0);
        return;
    }
}

static GLenum base_format(GLenum f)
{
    switch (f) {
    case 4: case GL_RGBA: case GL_BGRA_EXT: case GL_RGBA8: case GL_RGBA4: case GL_RGB5_A1: return GL_RGBA;
    case 3: case GL_RGB: case GL_RGB8: case GL_RGB565: return GL_RGB;
    case 1: case GL_LUMINANCE: return GL_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: return GL_LUMINANCE_ALPHA;
    case GL_ALPHA: return GL_ALPHA;
    }
    return 0;
}

void gl_tex_dirty(GC *gc, Tex *t)
{
    t->be_seq++;
    t->stereo_opq = 0;              /* the pixels changed: re-check for stereo */
    t->stereo_cls = 0;              /* new content: classify it afresh */
    if (gc->be->tex_changed)
        gc->be->tex_changed(gc, t);
}

static int tex_alloc(GC *gc, Tex *t, int w, int h, GLenum base)
{
    uint32_t *px = NULL;
    int i;
    if (w && h) {
        px = malloc((size_t)w * h * 4);
        if (!px) {
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return 0;
        }
    }
    gl_tex_drop_image(t);
    t->px = px;
    t->own_px = 1;
    t->w = t->pitch = w;
    t->h = h;
    t->base = base;
    t->mip[0] = px;
    t->n_mip = px ? 1 : 0;
    for (i = 1; i < MAX_MIP_LEVELS; i++)
        t->mip[i] = NULL;
    return 1;
}

static void api_TexImage2D(GC *gc, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                           GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    Tex *t;
    GLenum base = base_format(format);
    int bpp = src_bpp(format, type), y, row;
    (void)ifmt;
    if (!IS_TEX_TARGET(target)) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (!base || !bpp) {
        gl_log("TexImage2D: unsupported format %#x type %#x", format, type);
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (w < 0 || h < 0 || w > MAX_TEX_SIZE || h > MAX_TEX_SIZE || border || level < 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    if (level > 0)
        return;                 /* only the base level is kept */
    t = gl_bound_tex(gc, gc->active_tex);
    if (!t || !tex_alloc(gc, t, w, h, base))
        return;
    row = (w * bpp + gc->unpack_align - 1) & ~(gc->unpack_align - 1);
    if (pixels)
        for (y = 0; y < h; y++)
            unpack_row(t->px + (size_t)y * w, (const uint8_t *)pixels + (size_t)y * row, w, format, type);
    else if (t->px)
        memset(t->px, 0, (size_t)w * h * 4);
    gl_tex_dirty(gc, t);
}

static void api_TexSubImage2D(GC *gc, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                              GLenum format, GLenum type, const GLvoid *pixels)
{
    Tex *t;
    int bpp = src_bpp(format, type), j, row;
    if (!IS_TEX_TARGET(target) || !bpp) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (level > 0)
        return;
    t = gl_bound_tex(gc, gc->active_tex);
    if (!t || !t->px) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > t->w || y + h > t->h) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    row = (w * bpp + gc->unpack_align - 1) & ~(gc->unpack_align - 1);
    if (pixels)
        for (j = 0; j < h; j++)
            unpack_row(t->px + (size_t)(y + j) * t->pitch + x, (const uint8_t *)pixels + (size_t)j * row, w, format, type);
    gl_tex_dirty(gc, t);
}

/* OES_compressed_paletted_texture (the one compressed format ES 1.1 requires)
   and IMG_texture_compression_pvrtc, the format iOS games actually ship. */
static void api_CompressedTexImage2D(GC *gc, GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                     GLint border, GLsizei size, const GLvoid *data)
{
    static const GLenum pal_type[5] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_5_6_5,
                                       GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_5_5_5_1};
    static const GLenum pal_fmt[5] = {GL_RGB, GL_RGBA, GL_RGB, GL_RGBA, GL_RGBA};
    uint32_t pal[256];
    const uint8_t *s = data;
    Tex *t;
    int k, bits, n, i;
    (void)border;
    if (ifmt >= GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG && ifmt <= GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG) {
        int fmt = (int)(ifmt - GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG);
        int bw = fmt < PVRTC_RGB_2BPP ? 4 : 8;
        GLsizei need = (GLsizei)((size_t)((w + bw - 1) / bw) * ((h + 3) / 4) * 8);
        GLenum base = (fmt == PVRTC_RGBA_4BPP || fmt == PVRTC_RGBA_2BPP) ? GL_RGBA : GL_RGB;
        if (target != GL_TEXTURE_2D || w < 8 || h < 8 || w > MAX_TEX_SIZE || h > MAX_TEX_SIZE ||
            w % bw || h % 4 || level > 0 || !data || size < need) {
            gl_set_error(gc, GL_INVALID_VALUE);
            return;
        }
        t = gl_bound_tex(gc, gc->active_tex);
        if (!t || !tex_alloc(gc, t, w, h, base))
            return;
        if (!pvrtc_decode(data, w, h, fmt, t->px)) {
            gl_set_error(gc, GL_INVALID_VALUE);
            return;
        }
        gl_tex_dirty(gc, t);
        return;
    }
    if (target != GL_TEXTURE_2D || ifmt < GL_PALETTE4_RGB8_OES || ifmt > GL_PALETTE8_RGB5_A1_OES) {
        gl_log("CompressedTexImage2D: unsupported format %#x", ifmt);
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (w < 0 || h < 0 || w > MAX_TEX_SIZE || h > MAX_TEX_SIZE || level > 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    k = (ifmt - GL_PALETTE4_RGB8_OES) % 5;
    bits = ifmt - GL_PALETTE4_RGB8_OES < 5 ? 4 : 8;
    n = 1 << bits;
    unpack_row(pal, s, n, pal_fmt[k], pal_type[k]);
    s += n * src_bpp(pal_fmt[k], pal_type[k]);
    t = gl_bound_tex(gc, gc->active_tex);
    if (!t || !tex_alloc(gc, t, w, h, pal_fmt[k]))
        return;
    for (i = 0; i < w * h; i++)
        t->px[i] = pal[bits == 8 ? s[i] : (i & 1) ? s[i >> 1] & 15 : s[i >> 1] >> 4];
    gl_tex_dirty(gc, t);
}

static void api_CompressedTexSubImage2D(GC *gc, GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                        GLsizei h, GLenum format, GLsizei size, const GLvoid *data)
{
    (void)target; (void)level; (void)x; (void)y; (void)w; (void)h; (void)format; (void)size; (void)data;
    gl_set_error(gc, GL_INVALID_OPERATION);
}

static void texparam(GC *gc, GLenum target, GLenum pname, GLint v)
{
    Tex *t = gl_bound_tex(gc, gc->active_tex);
    if (!IS_TEX_TARGET(target) || !t) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = v; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = v; break;
    case GL_TEXTURE_WRAP_S:     t->wrap_s = v; break;
    case GL_TEXTURE_WRAP_T:     t->wrap_t = v; break;
    case GL_GENERATE_MIPMAP:    t->gen_mipmap = v; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: break;
    case 0x813C: case 0x813D:   break;                           /* GL_TEXTURE_BASE/MAX_LEVEL_APPLE: one level */
    default:
        gl_log("TexParameter: unknown pname %#x (value %d)", pname, v);
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    gl_tex_dirty(gc, t);
}

static void api_TexParameteri(GC *gc, GLenum target, GLenum pname, GLint v) { texparam(gc, target, pname, v); }
static void api_TexParameterf(GC *gc, GLenum target, GLenum pname, GLfloat v) { texparam(gc, target, pname, (GLint)v); }
static void api_TexParameterx(GC *gc, GLenum target, GLenum pname, GLfixed v) { texparam(gc, target, pname, v); }
static void api_TexParameteriv(GC *gc, GLenum target, GLenum pname, const GLint *v)
{
    if (pname == GL_TEXTURE_CROP_RECT_OES) {
        Tex *t = gl_bound_tex(gc, gc->active_tex);
        if (t)
            memcpy(t->crop, v, sizeof t->crop);
    } else
        texparam(gc, target, pname, v[0]);
}
static void api_TexParameterfv(GC *gc, GLenum target, GLenum pname, const GLfloat *v)
{
    if (pname == GL_TEXTURE_CROP_RECT_OES) {
        GLint c[4] = {(GLint)v[0], (GLint)v[1], (GLint)v[2], (GLint)v[3]};
        api_TexParameteriv(gc, target, pname, c);
    } else
        texparam(gc, target, pname, (GLint)v[0]);
}
static void api_TexParameterxv(GC *gc, GLenum target, GLenum pname, const GLfixed *v)
{
    if (pname == GL_TEXTURE_CROP_RECT_OES) {
        GLint c[4] = {v[0] >> 16, v[1] >> 16, v[2] >> 16, v[3] >> 16};
        api_TexParameteriv(gc, target, pname, c);
    } else
        texparam(gc, target, pname, v[0]);
}

static void api_GetTexParameteriv(GC *gc, GLenum target, GLenum pname, GLint *out)
{
    Tex *t = gl_bound_tex(gc, gc->active_tex);
    (void)target;
    if (!t)
        return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *out = t->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *out = t->mag_filter; break;
    case GL_TEXTURE_WRAP_S:     *out = t->wrap_s; break;
    case GL_TEXTURE_WRAP_T:     *out = t->wrap_t; break;
    case GL_GENERATE_MIPMAP:    *out = t->gen_mipmap; break;
    case GL_TEXTURE_CROP_RECT_OES: memcpy(out, t->crop, sizeof t->crop); break;
    case 0x84FE:                *out = 1; break;                /* GL_TEXTURE_MAX_ANISOTROPY_EXT */
    default:
        gl_log("GetTexParameter: unknown pname %#x", pname);
        gl_set_error(gc, GL_INVALID_ENUM);
    }
}
static void api_GetTexParameterfv(GC *gc, GLenum target, GLenum pname, GLfloat *out)
{
    GLint v[4] = {0, 0, 0, 0};
    int i, n = nparams(pname);
    api_GetTexParameteriv(gc, target, pname, v);
    for (i = 0; i < n; i++)
        out[i] = (GLfloat)v[i];
}

/* OES_framebuffer_object's glGenerateMipmapOES: build a chain of box-filtered
   levels from the base image. Both back ends still sample level 0 for
   minification (docs/gpu.md); generating the chain is enough for games that
   call it at load time and for apps that inspect GL_TEXTURE_MAX_LEVEL. */
static void api_GenerateMipmap(GC *gc, GLenum target)
{
    Tex *t = gl_bound_tex(gc, gc->active_tex);
    int level, w, h, pw, ph, pitch, x, y, i;
    if (!IS_TEX_TARGET(target) || !t || !t->px) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    w = t->w;
    h = t->h;
    pw = w;
    ph = h;
    pitch = t->pitch;
    for (level = 1; (pw > 1 || ph > 1) && level < MAX_MIP_LEVELS; level++) {
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        uint32_t *out = malloc((size_t)nw * nh * 4);
        if (!out) {
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return;
        }
        for (y = 0; y < nh; y++)
            for (x = 0; x < nw; x++) {
                unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
                int j, k;
                for (j = 0; j < 2; j++)
                    for (k = 0; k < 2; k++) {
                        int sx = x * 2 + k, sy = y * 2 + j;
                        uint32_t p;
                        if (sx >= pw || sy >= ph)
                            continue;              /* odd sizes: clamp the edge */
                        p = t->mip[level - 1][(size_t)sy * pitch + sx];
                        a += p >> 24 & 255;
                        r += p >> 16 & 255;
                        g += p >> 8 & 255;
                        b += p & 255;
                        n++;
                    }
                out[(size_t)y * nw + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        free(t->mip[level]);
        t->mip[level] = out;
        pw = nw;
        ph = nh;
        pitch = nw;
    }
    for (i = level; i < t->n_mip; i++) {
        free(t->mip[i]);
        t->mip[i] = NULL;
    }
    t->n_mip = level;
    gl_tex_dirty(gc, t);
}

static void texenv_fv(GC *gc, GLenum target, GLenum pname, const GLfloat *v)
{
    TexEnv *e = &gc->tu[gc->active_tex].env;
    GLenum iv = (GLenum)v[0];
    if (target == GL_POINT_SPRITE_OES)
        return;                 /* GL_COORD_REPLACE_OES: sprites always replace */
    if (target == 0x8500 && pname == 0x8501)
        return;                 /* GL_TEXTURE_LOD_BIAS_EXT: there are no mipmaps to bias */
    if (target != GL_TEXTURE_ENV) {
        gl_log("TexEnv: unknown target %#x (pname %#x)", target, pname);
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_ENV_MODE:  e->mode = iv; break;
    case GL_TEXTURE_ENV_COLOR: memcpy(e->color, v, 16); break;
    case GL_COMBINE_RGB:       e->comb_rgb = iv; break;
    case GL_COMBINE_ALPHA:     e->comb_a = iv; break;
    case GL_RGB_SCALE:         e->rgb_scale = v[0]; break;
    case GL_ALPHA_SCALE:       e->a_scale = v[0]; break;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB:       e->src_rgb[pname - GL_SRC0_RGB] = iv; break;
    case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA: e->src_a[pname - GL_SRC0_ALPHA] = iv; break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: e->op_rgb[pname - GL_OPERAND0_RGB] = iv; break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: e->op_a[pname - GL_OPERAND0_ALPHA] = iv; break;
    default:
        gl_log("TexEnv: unknown pname %#x (value %g)", pname, v[0]);
        gl_set_error(gc, GL_INVALID_ENUM);
    }
}
static void api_TexEnvfv(GC *gc, GLenum t, GLenum p, const GLfloat *v) { texenv_fv(gc, t, p, v); }
static void api_TexEnvf(GC *gc, GLenum t, GLenum p, GLfloat v) { GLfloat f[4] = {v, 0, 0, 0}; texenv_fv(gc, t, p, f); }
static void api_TexEnvi(GC *gc, GLenum t, GLenum p, GLint v) { GLfloat f[4] = {(GLfloat)v, 0, 0, 0}; texenv_fv(gc, t, p, f); }
static void api_TexEnviv(GC *gc, GLenum t, GLenum p, const GLint *v)
{
    GLfloat f[4];
    int i, n = nparams(p);
    for (i = 0; i < n; i++)
        f[i] = p == GL_TEXTURE_ENV_COLOR ? v[i] * (1.0f / 2147483647.0f) : (GLfloat)v[i];
    texenv_fv(gc, t, p, f);
}
static void api_TexEnvxv(GC *gc, GLenum t, GLenum p, const GLfixed *v)
{
    GLfloat f[4];
    if (p == GL_TEXTURE_ENV_COLOR || p == GL_RGB_SCALE || p == GL_ALPHA_SCALE)
        x2fv(p, v, f);
    else
        f[0] = (GLfloat)v[0];
    texenv_fv(gc, t, p, f);
}
static void api_TexEnvx(GC *gc, GLenum t, GLenum p, GLfixed v) { api_TexEnvxv(gc, t, p, &v); }

/* --------------------------------------------------------------- buffers */
static Buf *buf_target(GC *gc, GLenum target)
{
    GLuint name = target == GL_ARRAY_BUFFER ? gc->array_buf : target == GL_ELEMENT_ARRAY_BUFFER ? gc->elem_buf : 0;
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return NULL;
    }
    if (!name) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return NULL;
    }
    return nt_get(&gc->share->buf, name);
}

static void api_GenBuffers(GC *gc, GLsizei n, GLuint *out)
{
    int i;
    for (i = 0; i < n; i++) {
        Buf *b = calloc(1, sizeof *b);
        out[i] = nt_free_name(&gc->share->buf);
        if (b) {
            b->name = out[i];
            b->usage = GL_STATIC_DRAW;
            nt_set(&gc->share->buf, out[i], b);
        }
    }
}

static void api_BindBuffer(GC *gc, GLenum target, GLuint name)
{
    if (name && !nt_get(&gc->share->buf, name)) {
        Buf *b = calloc(1, sizeof *b);
        if (!b || !nt_set(&gc->share->buf, name, b)) {
            free(b);
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return;
        }
        b->name = name;
        b->usage = GL_STATIC_DRAW;
    }
    if (target == GL_ARRAY_BUFFER)
        gc->array_buf = name;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        gc->elem_buf = name;
    else
        gl_set_error(gc, GL_INVALID_ENUM);
}

static void api_DeleteBuffers(GC *gc, GLsizei n, const GLuint *names)
{
    int i;
    for (i = 0; i < n; i++) {
        Buf *b = names[i] ? nt_get(&gc->share->buf, names[i]) : NULL;
        if (!b)
            continue;
        if (gc->array_buf == names[i])
            gc->array_buf = 0;
        if (gc->elem_buf == names[i])
            gc->elem_buf = 0;
        free(b->data);
        free(b);
        gc->share->buf.obj[names[i]] = NULL;
        if (names[i] < gc->share->buf.next)
            gc->share->buf.next = names[i];
    }
}

static void api_BufferData(GC *gc, GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    Buf *b = buf_target(gc, target);
    uint8_t *p;
    if (!b)
        return;
    if (size < 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    p = malloc(size ? size : 1);
    if (!p) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return;
    }
    if (data)
        memcpy(p, data, size);
    free(b->data);
    b->data = p;
    b->size = size;
    b->usage = usage;
}

static void api_BufferSubData(GC *gc, GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data)
{
    Buf *b = buf_target(gc, target);
    if (!b)
        return;
    if (off < 0 || size < 0 || (size_t)off + size > b->size) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    memcpy(b->data + off, data, size);
}

static void api_GetBufferParameteriv(GC *gc, GLenum target, GLenum pname, GLint *out)
{
    Buf *b = buf_target(gc, target);
    if (!b)
        return;
    switch (pname) {
    case GL_BUFFER_SIZE:  *out = b->size; break;
    case GL_BUFFER_USAGE: *out = b->usage; break;
    case GL_BUFFER_ACCESS_OES: *out = GL_WRITE_ONLY_OES; break;
    case GL_BUFFER_MAPPED_OES: *out = b->mapped; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}

static void *api_MapBuffer(GC *gc, GLenum target, GLenum access)
{
    Buf *b = buf_target(gc, target);
    (void)access;
    if (!b || !b->data)
        return NULL;
    b->mapped = 1;
    return b->data;
}
static GLboolean api_UnmapBuffer(GC *gc, GLenum target)
{
    Buf *b = buf_target(gc, target);
    if (b)
        b->mapped = 0;
    return 1;
}
static void api_GetBufferPointerv(GC *gc, GLenum target, GLenum pname, GLvoid **out)
{
    Buf *b = buf_target(gc, target);
    (void)pname;
    *out = b && b->mapped ? b->data : NULL;
}
static GLboolean api_IsBuffer(GC *gc, GLuint name) { return name && nt_get(&gc->share->buf, name) != NULL; }

/* ---------------------------------------------------- framebuffer objects */
static Fb *fb_cur(GC *gc)
{
    return gc->fb ? nt_get(&gc->share->fb, gc->fb) : NULL;
}

static void api_GenFramebuffers(GC *gc, GLsizei n, GLuint *out)
{
    int i;
    for (i = 0; i < n; i++) {
        Fb *f = calloc(1, sizeof *f);
        out[i] = nt_free_name(&gc->share->fb);
        if (f) {
            f->name = out[i];
            nt_set(&gc->share->fb, out[i], f);
        }
    }
}

static void api_BindFramebuffer(GC *gc, GLenum target, GLuint name)
{
    (void)target;
    if (name && !nt_get(&gc->share->fb, name)) {
        Fb *f = calloc(1, sizeof *f);
        if (!f || !nt_set(&gc->share->fb, name, f)) {
            free(f);
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return;
        }
        f->name = name;
    }
    gc->fb = name;
}

static void api_DeleteFramebuffers(GC *gc, GLsizei n, const GLuint *names)
{
    int i;
    for (i = 0; i < n; i++) {
        Fb *f = names[i] ? nt_get(&gc->share->fb, names[i]) : NULL;
        if (!f)
            continue;
        if (gc->fb == names[i])
            gc->fb = 0;
        free(f);
        gc->share->fb.obj[names[i]] = NULL;
        if (names[i] < gc->share->fb.next)
            gc->share->fb.next = names[i];
    }
}

static GLboolean api_IsFramebuffer(GC *gc, GLuint name) { return name && nt_get(&gc->share->fb, name) != NULL; }

static void api_GenRenderbuffers(GC *gc, GLsizei n, GLuint *out)
{
    int i;
    for (i = 0; i < n; i++) {
        Rb *r = calloc(1, sizeof *r);
        out[i] = nt_free_name(&gc->share->rb);
        if (r) {
            r->name = out[i];
            nt_set(&gc->share->rb, out[i], r);
        }
    }
}

static void api_BindRenderbuffer(GC *gc, GLenum target, GLuint name)
{
    (void)target;
    if (name && !nt_get(&gc->share->rb, name)) {
        Rb *r = calloc(1, sizeof *r);
        if (!r || !nt_set(&gc->share->rb, name, r)) {
            free(r);
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return;
        }
        r->name = name;
    }
    gc->rb = name;
}

static void api_DeleteRenderbuffers(GC *gc, GLsizei n, const GLuint *names)
{
    int i;
    for (i = 0; i < n; i++) {
        Rb *r = gl_rb_lookup(gc, names[i]);
        if (!r)
            continue;
        if (gc->rb == names[i])
            gc->rb = 0;
        gl_rb_drop_storage(r);
        free(r);
        gc->share->rb.obj[names[i]] = NULL;
        if (names[i] < gc->share->rb.next)
            gc->share->rb.next = names[i];
    }
}

static GLboolean api_IsRenderbuffer(GC *gc, GLuint name) { return gl_rb_lookup(gc, name) != NULL; }

static int is_depth_format(GLenum f)
{
    return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH24_STENCIL8 ||
           f == GL_DEPTH_COMPONENT || f == GL_STENCIL_INDEX8;
}

static void api_RenderbufferStorage(GC *gc, GLenum target, GLenum ifmt, GLsizei w, GLsizei h)
{
    Rb *r = gl_rb_lookup(gc, gc->rb);
    (void)target;
    if (!r) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    if (w < 0 || h < 0 || w > 2048 || h > 2048) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    gl_rb_drop_storage(r);
    r->ifmt = ifmt;
    if (is_depth_format(ifmt)) {
        r->depth = malloc((size_t)w * h * 2 + 2);
    } else {
        r->color = calloc((size_t)w * h + 1, 4);
        r->own_color = 1;
    }
    if (!r->depth && !r->color) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return;
    }
    r->w = r->pitch = w;
    r->h = h;
    r->flip = 0;
}

static void api_RenderbufferStorageMultisample(GC *gc, GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h)
{
    (void)samples;
    api_RenderbufferStorage(gc, target, ifmt, w, h);
}

static void api_GetRenderbufferParameteriv(GC *gc, GLenum target, GLenum pname, GLint *out)
{
    Rb *r = gl_rb_lookup(gc, gc->rb);
    int color;
    (void)target;
    if (!r) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    color = !is_depth_format(r->ifmt);
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:  *out = r->w; break;
    case GL_RENDERBUFFER_HEIGHT: *out = r->h; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *out = r->ifmt; break;
    case GL_RENDERBUFFER_RED_SIZE: case GL_RENDERBUFFER_GREEN_SIZE: case GL_RENDERBUFFER_BLUE_SIZE:
    case GL_RENDERBUFFER_ALPHA_SIZE: *out = color ? 8 : 0; break;
    case GL_RENDERBUFFER_DEPTH_SIZE: *out = color ? 0 : 16; break;
    case GL_RENDERBUFFER_STENCIL_SIZE: *out = 0; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}

static void fb_attach(GC *gc, GLenum attachment, GLenum type, GLuint name)
{
    Fb *f = fb_cur(gc);
    if (!f) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    if (!name)
        type = 0;
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0: f->color_type = type; f->color_name = name; break;
    case GL_DEPTH_ATTACHMENT:  f->depth_type = type; f->depth_name = name; break;
    case GL_STENCIL_ATTACHMENT: f->stencil_name = name; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}

static void api_FramebufferRenderbuffer(GC *gc, GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb)
{
    (void)target; (void)rbtarget;
    fb_attach(gc, attachment, GL_RENDERBUFFER, rb);
}

static void api_FramebufferTexture2D(GC *gc, GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level)
{
    (void)target; (void)textarget; (void)level;
    fb_attach(gc, attachment, GL_TEXTURE, tex);
}

/* Resolve the current framebuffer into what the back end draws on. */
int gl_get_rt(GC *gc, RT *rt)
{
    Fb *f = fb_cur(gc);
    Rb *c = NULL, *d = NULL;
    memset(rt, 0, sizeof *rt);
    if (!f) {
        c = gc->default_rb;
        d = gc->default_depth;
    } else if (f->color_type == GL_RENDERBUFFER) {
        c = gl_rb_lookup(gc, f->color_name);
    } else if (f->color_type == GL_TEXTURE) {
        Tex *t = nt_get(&gc->share->tex, f->color_name);
        if (!t || !t->px)
            return 0;
        rt->w = t->w; rt->h = t->h; rt->pitch = t->pitch; rt->color = t->px; rt->surface = t->surface != NULL;
        t->be_seq++;
    }
    if (c) {
        if (c->dr && !drawable_begin(gc, c))
            return 0;
        if (!c->color)
            return 0;
        rt->w = c->w; rt->h = c->h; rt->pitch = c->pitch; rt->color = c->color; rt->flip = c->flip;
        rt->surface = c->dr != NULL;
        if (c->ifmt == GL_RGB565 || c->ifmt == GL_RGB8)
            rt->opaque = 0xff000000u;
    }
    if (!rt->color)
        return 0;
    if (f && f->depth_type == GL_RENDERBUFFER)
        d = gl_rb_lookup(gc, f->depth_name);
    if (d && d->depth && d->w >= rt->w && d->h >= rt->h) {
        rt->depth = d->depth;
        rt->dpitch = d->pitch;
    }
    return 1;
}

static GLenum api_CheckFramebufferStatus(GC *gc, GLenum target)
{
    Fb *f = fb_cur(gc);
    RT rt;
    (void)target;
    if (f && !f->color_type && !f->depth_type)
        return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (!gl_get_rt(gc, &rt))
        return f ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT : GL_FRAMEBUFFER_UNSUPPORTED;
    if (f && f->depth_type && !rt.depth)
        return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
    return GL_FRAMEBUFFER_COMPLETE;
}

static void api_GetFramebufferAttachmentParameteriv(GC *gc, GLenum target, GLenum attachment, GLenum pname, GLint *out)
{
    Fb *f = fb_cur(gc);
    int color = attachment == GL_COLOR_ATTACHMENT0;
    (void)target;
    if (!f) {
        gl_set_error(gc, GL_INVALID_OPERATION);
        return;
    }
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE)
        *out = color ? f->color_type : f->depth_type;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME)
        *out = color ? f->color_name : f->depth_name;
    else
        *out = 0;
}

/* ------------------------------------------------ clear, read, copy, draw */
static void api_Clear(GC *gc, GLbitfield mask)
{
    RT rt;
    if (!gl_get_rt(gc, &rt)) {
        gl_set_error(gc, GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }
    gc->be->clear(gc, &rt, mask);
}

static void api_ReadPixels(GC *gc, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *out)
{
    RT rt;
    int i, j, row;
    if (!gl_get_rt(gc, &rt)) {
        gl_set_error(gc, GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }
    if ((format != GL_RGBA && format != GL_BGRA_EXT) || type != GL_UNSIGNED_BYTE) {
        gl_set_error(gc, GL_INVALID_ENUM);
        return;
    }
    if (gc->be->finish)
        gc->be->finish(gc, &rt);
    row = (w * 4 + gc->pack_align - 1) & ~(gc->pack_align - 1);
    for (j = 0; j < h; j++) {
        int sy = y + j;
        uint8_t *d = (uint8_t *)out + (size_t)j * row;
        const uint32_t *s;
        if (sy < 0 || sy >= rt.h)
            continue;
        s = rt.color + (size_t)(rt.flip ? rt.h - 1 - sy : sy) * rt.pitch;
        for (i = 0; i < w; i++, d += 4) {
            uint32_t p = (x + i >= 0 && x + i < rt.w) ? s[x + i] : 0;
            if (format == GL_BGRA_EXT)
                memcpy(d, &p, 4);
            else {
                d[0] = p >> 16; d[1] = p >> 8; d[2] = p; d[3] = p >> 24;
            }
        }
    }
}

static void copy_from_rt(GC *gc, Tex *t, int dx, int dy, int x, int y, int w, int h)
{
    RT rt;
    int i, j;
    if (!gl_get_rt(gc, &rt)) {
        gl_set_error(gc, GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }
    if (gc->be->finish)
        gc->be->finish(gc, &rt);
    for (j = 0; j < h; j++) {
        int sy = y + j;
        uint32_t *d = t->px + (size_t)(dy + j) * t->pitch + dx;
        const uint32_t *s;
        if (sy < 0 || sy >= rt.h)
            continue;
        s = rt.color + (size_t)(rt.flip ? rt.h - 1 - sy : sy) * rt.pitch;
        for (i = 0; i < w; i++)
            if (x + i >= 0 && x + i < rt.w)
                d[i] = t->base == GL_RGBA ? s[x + i] : s[x + i] | 0xff000000u;
    }
    gl_tex_dirty(gc, t);
}

static void api_CopyTexImage2D(GC *gc, GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{
    Tex *t = gl_bound_tex(gc, gc->active_tex);
    GLenum base = base_format(ifmt);
    (void)target; (void)border;
    if (level > 0 || !t)
        return;
    if (!base || w < 0 || h < 0 || w > MAX_TEX_SIZE || h > MAX_TEX_SIZE) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    if (tex_alloc(gc, t, w, h, base)) {
        memset(t->px, 0, (size_t)w * h * 4);
        copy_from_rt(gc, t, 0, 0, x, y, w, h);
    }
}

static void api_CopyTexSubImage2D(GC *gc, GLenum target, GLint level, GLint dx, GLint dy, GLint x, GLint y, GLsizei w, GLsizei h)
{
    Tex *t = gl_bound_tex(gc, gc->active_tex);
    (void)target;
    if (level > 0)
        return;
    if (!t || !t->px || dx < 0 || dy < 0 || w < 0 || h < 0 || dx + w > t->w || dy + h > t->h) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    copy_from_rt(gc, t, dx, dy, x, y, w, h);
}

static void api_DrawArrays(GC *gc, GLenum mode, GLint first, GLsizei count)
{
    if (count < 0 || first < 0)
        gl_set_error(gc, GL_INVALID_VALUE);
    else if (mode > GL_TRIANGLE_FAN)
        gl_set_error(gc, GL_INVALID_ENUM);
    else
        gl_draw(gc, mode, first, count, 0, NULL);
}

static void api_DrawElements(GC *gc, GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    if (count < 0)
        gl_set_error(gc, GL_INVALID_VALUE);
    else if (mode > GL_TRIANGLE_FAN || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT))
        gl_set_error(gc, GL_INVALID_ENUM);
    else {
        if (gc->elem_buf) {
            Buf *b = nt_get(&gc->share->buf, gc->elem_buf);
            if (!b || !b->data)
                return;
            indices = b->data + (size_t)indices;
        }
        gl_draw(gc, mode, 0, count, type, indices);
    }
}

static void api_DrawTexf(GC *gc, GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h) { gl_draw_tex(gc, x, y, z, w, h); }
static void api_DrawTexi(GC *gc, GLint x, GLint y, GLint z, GLint w, GLint h) { gl_draw_tex(gc, x, y, z, w, h); }
static void api_DrawTexs(GC *gc, GLshort x, GLshort y, GLshort z, GLshort w, GLshort h) { gl_draw_tex(gc, x, y, z, w, h); }
static void api_DrawTexx(GC *gc, GLfixed x, GLfixed y, GLfixed z, GLfixed w, GLfixed h) { gl_draw_tex(gc, x2f(x), x2f(y), x2f(z), x2f(w), x2f(h)); }
static void api_DrawTexfv(GC *gc, const GLfloat *c) { gl_draw_tex(gc, c[0], c[1], c[2], c[3], c[4]); }
static void api_DrawTexiv(GC *gc, const GLint *c) { gl_draw_tex(gc, c[0], c[1], c[2], c[3], c[4]); }
static void api_DrawTexsv(GC *gc, const GLshort *c) { gl_draw_tex(gc, c[0], c[1], c[2], c[3], c[4]); }
static void api_DrawTexxv(GC *gc, const GLfixed *c) { gl_draw_tex(gc, x2f(c[0]), x2f(c[1]), x2f(c[2]), x2f(c[3]), x2f(c[4])); }

static void api_Flush(GC *gc)
{
    if (gc->be->flush_all)
        gc->be->flush_all(gc);
}
static void api_Finish(GC *gc)
{
    RT rt;
    if (gc->be->finish && gl_get_rt(gc, &rt))
        gc->be->finish(gc, &rt);
    api_Flush(gc);
}

/* APPLE_fence / object waits: everything is synchronous here. */
static void api_FinishObject(GC *gc, GLenum object, GLint name) { (void)object; (void)name; api_Finish(gc); }
static GLboolean api_TestObject(GC *gc, GLenum object, GLuint name) { (void)gc; (void)object; (void)name; return 1; }
static void api_GenFences(GC *gc, GLsizei n, GLuint *out)
{
    static GLuint next = 1;
    int i;
    (void)gc;
    for (i = 0; i < n; i++)
        out[i] = next++;
}
static void api_DeleteFences(GC *gc, GLsizei n, const GLuint *f) { (void)gc; (void)n; (void)f; }
static void api_SetFence(GC *gc, GLuint f) { (void)gc; (void)f; }
static GLboolean api_TestFence(GC *gc, GLuint f) { (void)gc; (void)f; return 1; }
static void api_FinishFence(GC *gc, GLuint f) { (void)gc; (void)f; }
static GLboolean api_IsFence(GC *gc, GLuint f) { (void)gc; return f != 0; }
static void api_FramebufferParameteri(GC *gc, GLenum target, GLenum pname, GLint v) { (void)gc; (void)target; (void)pname; (void)v; }
static void api_DiscardFramebuffer(GC *gc, GLenum target, GLsizei n, const GLenum *att) { (void)gc; (void)target; (void)n; (void)att; }
static void api_ResolveMultisampleFramebuffer(GC *gc) { (void)gc; }

/* ------------------------------------------------------------------ gets */
static GLenum api_GetError(GC *gc)
{
    GLenum e = gc->error;
    gc->error = GL_NO_ERROR;
    return e;
}

static const char gl_extensions[] =
    "GL_OES_framebuffer_object GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_draw_texture "
    "GL_OES_blend_subtract GL_OES_blend_func_separate GL_OES_blend_equation_separate "
    "GL_OES_mapbuffer GL_OES_point_sprite GL_OES_point_size_array GL_OES_read_format "
    "GL_OES_compressed_paletted_texture GL_IMG_texture_compression_pvrtc "
    "GL_OES_matrix_palette GL_OES_texture_mirrored_repeat "
    "GL_EXT_texture_format_BGRA8888 GL_APPLE_texture_format_BGRA8888 GL_IMG_read_format "
    "GL_IMG_texture_format_BGRA8888 GL_APPLE_texture_2D_limited_npot GL_APPLE_framebuffer_multisample "
    "GL_EXT_discard_framebuffer GL_APPLE_fence ";

static const GLubyte *api_GetString(GC *gc, GLenum name)
{
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"ios3ds";
    case GL_RENDERER:   return (const GLubyte *)gc->be->name;
    case GL_VERSION:    return (const GLubyte *)"OpenGL ES-CM 1.1 ios3ds";
    case GL_EXTENSIONS: return (const GLubyte *)gl_extensions;
    case 0x8A1E:        return (const GLubyte *)"";     /* QuartzCore asks right after GL_EXTENSIONS (an
                                                           Apple-private second extension list); none */
    }
    gl_log("GetString: unknown name %#x", name);
    gl_set_error(gc, GL_INVALID_ENUM);
    return NULL;
}

enum { T_INT, T_FLOAT, T_COLOR, T_BOOL };

/* Returns the number of values written to v (as floats) and their kind. */
static int get_values(GC *gc, GLenum pname, float *v, int *kind)
{
    RT rt;
    Array *a;
    int *cap, i;
    TexUnit *tu = &gc->tu[gc->active_tex];
    *kind = T_INT;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE: v[0] = MAX_TEX_SIZE; return 1;
    case 0x84F8:              v[0] = MAX_TEX_SIZE; return 1;    /* GL_MAX_RECTANGLE_TEXTURE_SIZE (QuartzCore) */
    case 0x84FD:              v[0] = 4; return 1;               /* GL_MAX_TEXTURE_LOD_BIAS_EXT */
    case GL_MAX_RENDERBUFFER_SIZE: v[0] = 2048; return 1;
    case GL_MAX_TEXTURE_UNITS: v[0] = MAX_TEX_UNITS; return 1;
    case GL_MAX_VERTEX_UNITS_OES: case GL_MAX_PALETTE_MATRICES_OES: v[0] = MAX_PALETTE_MATRICES; return 1;
    case GL_MAX_WEIGHTS_OES: v[0] = 4; return 1;
    case GL_MAX_LIGHTS: v[0] = MAX_LIGHTS; return 1;
    case GL_MAX_CLIP_PLANES: v[0] = MAX_CLIP_PLANES; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = MV_STACK; return 1;
    case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = PROJ_STACK; return 1;
    case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = TEX_STACK; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 2048; return 2;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = 1; return 1;
    case GL_SUBPIXEL_BITS: v[0] = 4; return 1;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v[0] = 8; return 1;
    case GL_DEPTH_BITS: v[0] = gl_get_rt(gc, &rt) && rt.depth ? 16 : 0; return 1;
    case GL_STENCIL_BITS: v[0] = 0; return 1;
    case GL_SAMPLE_BUFFERS: v[0] = 0; return 1;
    case GL_SAMPLES: v[0] = 0; return 1;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: v[0] = 14; return 1;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        for (i = 0; i < 10; i++)
            v[i] = GL_PALETTE4_RGB8_OES + i;
        for (i = 0; i < 4; i++)
            v[10 + i] = GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG + i;
        return 14;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE_OES: v[0] = GL_UNSIGNED_BYTE; return 1;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES: v[0] = GL_BGRA_EXT; return 1;
    case GL_TEXTURE_BINDING_2D: v[0] = tu->bound; return 1;
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + gc->active_tex; return 1;
    case GL_CLIENT_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + gc->client_tex; return 1;
    case GL_ARRAY_BUFFER_BINDING: v[0] = gc->array_buf; return 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = gc->elem_buf; return 1;
    case GL_VERTEX_ARRAY_BUFFER_BINDING: v[0] = gc->a_vertex.buf; return 1;
    case GL_NORMAL_ARRAY_BUFFER_BINDING: v[0] = gc->a_normal.buf; return 1;
    case GL_COLOR_ARRAY_BUFFER_BINDING: v[0] = gc->a_color.buf; return 1;
    case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: v[0] = gc->a_texcoord[gc->client_tex].buf; return 1;
    case GL_FRAMEBUFFER_BINDING: v[0] = gc->fb; return 1;
    case GL_RENDERBUFFER_BINDING: v[0] = gc->rb; return 1;
    case GL_MATRIX_MODE: v[0] = gc->matrix_mode; return 1;
    case GL_VIEWPORT: for (i = 0; i < 4; i++) v[i] = gc->vp[i]; return 4;
    case GL_SCISSOR_BOX: for (i = 0; i < 4; i++) v[i] = gc->scissor[i]; return 4;
    case GL_MODELVIEW_STACK_DEPTH: v[0] = gc->mv_depth + 1; return 1;
    case GL_PROJECTION_STACK_DEPTH: v[0] = gc->proj_depth + 1; return 1;
    case GL_TEXTURE_STACK_DEPTH: v[0] = tu->depth + 1; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = gc->unpack_align; return 1;
    case GL_PACK_ALIGNMENT: v[0] = gc->pack_align; return 1;
    case GL_DEPTH_FUNC: v[0] = gc->depth_func; return 1;
    case GL_ALPHA_TEST_FUNC: v[0] = gc->alpha_func; return 1;
    case GL_BLEND_SRC: case GL_BLEND_SRC_RGB: v[0] = gc->blend_src_rgb; return 1;
    case GL_BLEND_DST: case GL_BLEND_DST_RGB: v[0] = gc->blend_dst_rgb; return 1;
    case GL_BLEND_SRC_ALPHA: v[0] = gc->blend_src_a; return 1;
    case GL_BLEND_DST_ALPHA: v[0] = gc->blend_dst_a; return 1;
    case GL_BLEND_EQUATION: v[0] = gc->blend_eq_rgb; return 1;
    case GL_BLEND_EQUATION_ALPHA: v[0] = gc->blend_eq_a; return 1;
    case GL_CULL_FACE_MODE: v[0] = gc->cull_mode; return 1;
    case GL_FRONT_FACE: v[0] = gc->front_face; return 1;
    case GL_SHADE_MODEL: v[0] = gc->shade_model; return 1;
    case GL_FOG_MODE: v[0] = gc->fog_mode; return 1;
    case GL_PERSPECTIVE_CORRECTION_HINT: case GL_POINT_SMOOTH_HINT: case GL_LINE_SMOOTH_HINT:
    case GL_FOG_HINT: case GL_GENERATE_MIPMAP_HINT: v[0] = GL_DONT_CARE; return 1;
    case GL_STENCIL_CLEAR_VALUE: case GL_STENCIL_REF: v[0] = 0; return 1;
    case GL_STENCIL_FUNC: v[0] = GL_ALWAYS; return 1;
    case GL_STENCIL_FAIL: case GL_STENCIL_PASS_DEPTH_FAIL: case GL_STENCIL_PASS_DEPTH_PASS: v[0] = GL_KEEP; return 1;
    case GL_STENCIL_VALUE_MASK: case GL_STENCIL_WRITEMASK: v[0] = 0; return 1;
    case GL_VERTEX_ARRAY_SIZE: v[0] = gc->a_vertex.size; return 1;
    case GL_VERTEX_ARRAY_TYPE: v[0] = gc->a_vertex.type; return 1;
    case GL_VERTEX_ARRAY_STRIDE: v[0] = gc->a_vertex.stride; return 1;
    case GL_NORMAL_ARRAY_TYPE: v[0] = gc->a_normal.type; return 1;
    case GL_NORMAL_ARRAY_STRIDE: v[0] = gc->a_normal.stride; return 1;
    case GL_COLOR_ARRAY_SIZE: v[0] = gc->a_color.size; return 1;
    case GL_COLOR_ARRAY_TYPE: v[0] = gc->a_color.type; return 1;
    case GL_COLOR_ARRAY_STRIDE: v[0] = gc->a_color.stride; return 1;
    case GL_TEXTURE_COORD_ARRAY_SIZE: v[0] = gc->a_texcoord[gc->client_tex].size; return 1;
    case GL_TEXTURE_COORD_ARRAY_TYPE: v[0] = gc->a_texcoord[gc->client_tex].type; return 1;
    case GL_TEXTURE_COORD_ARRAY_STRIDE: v[0] = gc->a_texcoord[gc->client_tex].stride; return 1;
    case GL_DEPTH_WRITEMASK: *kind = T_BOOL; v[0] = gc->depth_mask; return 1;
    case GL_COLOR_WRITEMASK: *kind = T_BOOL; for (i = 0; i < 4; i++) v[i] = gc->color_mask[i]; return 4;
    case GL_LIGHT_MODEL_TWO_SIDE: *kind = T_BOOL; v[0] = gc->two_side; return 1;
    case GL_CURRENT_COLOR: *kind = T_COLOR; memcpy(v, gc->color, 16); return 4;
    case GL_COLOR_CLEAR_VALUE: *kind = T_COLOR; memcpy(v, gc->clear_color, 16); return 4;
    case GL_FOG_COLOR: *kind = T_COLOR; memcpy(v, gc->fog_color, 16); return 4;
    case GL_LIGHT_MODEL_AMBIENT: *kind = T_COLOR; memcpy(v, gc->light_model_ambient, 16); return 4;
    case GL_ALPHA_TEST_REF: *kind = T_COLOR; v[0] = gc->alpha_ref; return 1;
    case GL_DEPTH_CLEAR_VALUE: *kind = T_COLOR; v[0] = gc->clear_depth; return 1;
    case GL_DEPTH_RANGE: *kind = T_COLOR; v[0] = gc->depth_near; v[1] = gc->depth_far; return 2;
    case GL_CURRENT_NORMAL: *kind = T_FLOAT; memcpy(v, gc->normal, 12); return 3;
    case GL_CURRENT_TEXTURE_COORDS: *kind = T_FLOAT; memcpy(v, gc->texcoord[gc->active_tex], 16); return 4;
    case GL_POINT_SIZE: *kind = T_FLOAT; v[0] = gc->point_size; return 1;
    case GL_LINE_WIDTH: *kind = T_FLOAT; v[0] = gc->line_width; return 1;
    case GL_POLYGON_OFFSET_FACTOR: *kind = T_FLOAT; v[0] = gc->poly_offset_factor; return 1;
    case GL_POLYGON_OFFSET_UNITS: *kind = T_FLOAT; v[0] = gc->poly_offset_units; return 1;
    case GL_FOG_DENSITY: *kind = T_FLOAT; v[0] = gc->fog_density; return 1;
    case GL_FOG_START: *kind = T_FLOAT; v[0] = gc->fog_start; return 1;
    case GL_FOG_END: *kind = T_FLOAT; v[0] = gc->fog_end; return 1;
    case GL_ALIASED_POINT_SIZE_RANGE: case GL_SMOOTH_POINT_SIZE_RANGE: *kind = T_FLOAT; v[0] = 1; v[1] = 64; return 2;
    case GL_ALIASED_LINE_WIDTH_RANGE: case GL_SMOOTH_LINE_WIDTH_RANGE: *kind = T_FLOAT; v[0] = 1; v[1] = 16; return 2;
    case GL_MODELVIEW_MATRIX: *kind = T_FLOAT; memcpy(v, gc->mv[gc->mv_depth].m, 64); return 16;
    case GL_PROJECTION_MATRIX: *kind = T_FLOAT; memcpy(v, gc->proj[gc->proj_depth].m, 64); return 16;
    case GL_TEXTURE_MATRIX: *kind = T_FLOAT; memcpy(v, tu->stack[tu->depth].m, 64); return 16;
    }
    cap = cap_ptr(gc, pname);
    if (cap) {
        *kind = T_BOOL;
        v[0] = *cap;
        return 1;
    }
    a = client_array(gc, pname);
    if (a) {
        *kind = T_BOOL;
        v[0] = a->enabled;
        return 1;
    }
    gl_log("Get: unknown pname %#x", pname);
    gl_set_error(gc, GL_INVALID_ENUM);
    return 0;
}

static void api_GetIntegerv(GC *gc, GLenum pname, GLint *out)
{
    float v[16];
    int kind, i, n = get_values(gc, pname, v, &kind);
    for (i = 0; i < n; i++)
        out[i] = kind == T_COLOR ? (GLint)(v[i] * 2147483647.0f) : kind == T_FLOAT ? (GLint)lroundf(v[i]) : (GLint)v[i];
}
static void api_GetFloatv(GC *gc, GLenum pname, GLfloat *out)
{
    float v[16];
    int kind, n = get_values(gc, pname, v, &kind);
    memcpy(out, v, n * sizeof(float));
}
static void api_GetBooleanv(GC *gc, GLenum pname, GLboolean *out)
{
    float v[16];
    int kind, i, n = get_values(gc, pname, v, &kind);
    for (i = 0; i < n; i++)
        out[i] = v[i] != 0;
}
static void api_GetFixedv(GC *gc, GLenum pname, GLfixed *out)
{
    float v[16];
    int kind, i, n = get_values(gc, pname, v, &kind);
    for (i = 0; i < n; i++)
        out[i] = kind == T_INT || kind == T_BOOL ? (GLfixed)v[i] : (GLfixed)(v[i] * 65536.0f);
}

static void api_GetPointerv(GC *gc, GLenum pname, GLvoid **out)
{
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: *out = (GLvoid *)gc->a_vertex.ptr; break;
    case GL_NORMAL_ARRAY_POINTER: *out = (GLvoid *)gc->a_normal.ptr; break;
    case GL_COLOR_ARRAY_POINTER:  *out = (GLvoid *)gc->a_color.ptr; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: *out = (GLvoid *)gc->a_texcoord[gc->client_tex].ptr; break;
    default: gl_set_error(gc, GL_INVALID_ENUM);
    }
}

/* -------------------------------------------------------------- context */
GC *gl_gc_new(Share *sh)
{
    GC *gc = calloc(1, sizeof *gc);
    int i;
    if (!gc)
        return NULL;
    gc->share = sh;
    sh->refs++;
    gc->be = gl_pick_backend();
    gc->matrix_mode = GL_MODELVIEW;
    gc->mv[0] = gc->proj[0] = mat_identity;
    for (i = 0; i < MAX_PALETTE_MATRICES; i++)
        gc->palette[i] = mat_identity;
    gc->mvp_dirty = 1;
    gc->depth_far = 1;
    gc->color[0] = gc->color[1] = gc->color[2] = gc->color[3] = 1;
    gc->normal[2] = 1;
    for (i = 0; i < MAX_TEX_UNITS; i++) {
        TexEnv *e = &gc->tu[i].env;
        int k;
        gc->texcoord[i][3] = 1;
        gc->tu[i].stack[0] = mat_identity;
        gc->tu[i].identity = 1;
        e->mode = GL_MODULATE;
        e->comb_rgb = e->comb_a = GL_MODULATE;
        e->rgb_scale = e->a_scale = 1;
        for (k = 0; k < 3; k++) {
            static const GLenum src[3] = {GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT};
            e->src_rgb[k] = e->src_a[k] = src[k];
            e->op_rgb[k] = k == 2 ? GL_SRC_ALPHA : GL_SRC_COLOR;
            e->op_a[k] = GL_SRC_ALPHA;
        }
        gc->a_texcoord[i].size = 4;
        gc->a_texcoord[i].type = GL_FLOAT;
    }
    gc->a_vertex.size = gc->a_color.size = 4;
    gc->a_vertex.type = gc->a_color.type = gc->a_normal.type = GL_FLOAT;
    gc->a_normal.size = 3;
    for (i = 0; i < MAX_LIGHTS; i++) {
        Light *l = &gc->light[i];
        l->ambient[3] = 1;
        if (i == 0)
            l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = l->specular[0] = l->specular[1] = l->specular[2] = 1;
        l->diffuse[3] = l->specular[3] = 1;
        l->pos[2] = 1;
        l->spot_dir[2] = -1;
        l->spot_cutoff = 180;
        l->att[0] = 1;
    }
    gc->mat.ambient[0] = gc->mat.ambient[1] = gc->mat.ambient[2] = 0.2f;
    gc->mat.diffuse[0] = gc->mat.diffuse[1] = gc->mat.diffuse[2] = 0.8f;
    gc->mat.ambient[3] = gc->mat.diffuse[3] = gc->mat.specular[3] = gc->mat.emission[3] = 1;
    gc->light_model_ambient[0] = gc->light_model_ambient[1] = gc->light_model_ambient[2] = 0.2f;
    gc->light_model_ambient[3] = 1;
    gc->shade_model = GL_SMOOTH;
    gc->fog_mode = GL_EXP;
    gc->fog_density = 1;
    gc->fog_end = 1;
    gc->cull_mode = GL_BACK;
    gc->front_face = GL_CCW;
    gc->alpha_func = GL_ALWAYS;
    gc->blend_src_rgb = gc->blend_src_a = GL_ONE;
    gc->blend_dst_rgb = gc->blend_dst_a = GL_ZERO;
    gc->blend_eq_rgb = gc->blend_eq_a = GL_FUNC_ADD;
    gc->depth_func = GL_LESS;
    gc->depth_mask = 1;
    gc->color_mask[0] = gc->color_mask[1] = gc->color_mask[2] = gc->color_mask[3] = 1;
    gc->clear_depth = 1;
    gc->point_size = gc->line_width = 1;
    gc->pack_align = gc->unpack_align = 4;
    gc->dither = gc->multisample = 1;
    return gc;
}

void gl_gc_free(GC *gc)
{
    gl_share_release(gc->share);
    free(gc->vcache);
    free(gc->emit);
    free(gc);
}

/* ------------------------------------------------------------- dispatch */
#ifndef GL_HOST
extern void gl_unimpl_thunks(void);     /* thunks.S: GLI_NSLOTS entries of 12 bytes */
#endif
static const char *slot_name(int slot);

int gl_unimpl(int slot)
{
    static unsigned char seen[GLI_NSLOTS];
    if (slot >= 0 && slot < GLI_NSLOTS && !seen[slot]) {
        seen[slot] = 1;
        gl_log("unimplemented GL call: slot %d (%s)", slot, slot_name(slot));
    }
    return 0;
}

#ifdef GL_HOST
static int unimpl_host(void) { return gl_unimpl(-1); }
#endif

/* ES 2.0-only entry points: EAGL refuses an ES2 context before our engine is
   reached (docs/gpu.md §1), but a game can still call one through an ES1
   context. Fail with GL_INVALID_OPERATION, not the thunk's silent 0, so a
   failed ES2 probe is unambiguous and an ES1 fallback keeps running. */
static GLuint api_es2_u(GC *gc) { gl_set_error(gc, GL_INVALID_OPERATION); return 0; }
static void api_es2_v(GC *gc) { gl_set_error(gc, GL_INVALID_OPERATION); }

#define H(name, fn) {SLOT_##name, #name, (void *)(fn)}
static const struct { unsigned short slot; const char *name; void *fn; } handlers[] = {
    H(glCreateShader, api_es2_u), H(glCreateProgram, api_es2_u),
    H(glShaderSource, api_es2_v), H(glCompileShader, api_es2_v),
    H(glLinkProgram, api_es2_v), H(glUseProgram, api_es2_v),
    H(glEnableVertexAttribArray, api_es2_v), H(glDisableVertexAttribArray, api_es2_v),
    H(glVertexAttribPointer, api_es2_v), H(glUniform1f, api_es2_v),
    H(glGetShaderiv, api_es2_v), H(glGetProgramiv, api_es2_v),
    H(glActiveTexture, api_ActiveTexture), H(glAlphaFunc, api_AlphaFunc), H(glAlphaFuncx, api_AlphaFuncx),
    H(glBindBuffer, api_BindBuffer), H(glBindFramebufferOES, api_BindFramebuffer),
    H(glBindRenderbufferOES, api_BindRenderbuffer), H(glBindTexture, api_BindTexture),
    H(glBlendColor, api_BlendColor), H(glBlendEquationOES, api_BlendEquation),
    H(glBlendEquationSeparateOES, api_BlendEquationSeparate), H(glBlendFunc, api_BlendFunc),
    H(glBlendFuncSeparateOES, api_BlendFuncSeparate), H(glBufferData, api_BufferData),
    H(glBufferSubData, api_BufferSubData), H(glCheckFramebufferStatusOES, api_CheckFramebufferStatus),
    H(glClear, api_Clear), H(glClearColor, api_ClearColor), H(glClearColorx, api_ClearColorx),
    H(glClearDepthf, api_ClearDepthf), H(glClearDepthx, api_ClearDepthx), H(glClearStencil, api_ClearStencil),
    H(glClientActiveTexture, api_ClientActiveTexture), H(glClipPlanef, api_ClipPlanef),
    H(glClipPlanex, api_ClipPlanex), H(glColor4f, api_Color4f), H(glColor4ub, api_Color4ub),
    H(glColor4x, api_Color4x), H(glColorMask, api_ColorMask), H(glColorPointer, api_ColorPointer),
    H(glCompressedTexImage2D, api_CompressedTexImage2D), H(glCompressedTexSubImage2D, api_CompressedTexSubImage2D),
    H(glCopyTexImage2D, api_CopyTexImage2D), H(glCopyTexSubImage2D, api_CopyTexSubImage2D),
    H(glCullFace, api_CullFace), H(glDeleteBuffers, api_DeleteBuffers), H(glDeleteFencesAPPLE, api_DeleteFences),
    H(glDeleteFramebuffersOES, api_DeleteFramebuffers), H(glDeleteRenderbuffersOES, api_DeleteRenderbuffers),
    H(glDeleteTextures, api_DeleteTextures), H(glDepthFunc, api_DepthFunc), H(glDepthMask, api_DepthMask),
    H(glDepthRangef, api_DepthRangef), H(glDepthRangex, api_DepthRangex), H(glDisable, api_Disable),
    H(glDisableClientState, api_DisableClientState), H(glDiscardFramebufferEXT, api_DiscardFramebuffer),
    H(glDrawArrays, api_DrawArrays), H(glDrawElements, api_DrawElements),
    H(glDrawTexfOES, api_DrawTexf), H(glDrawTexfvOES, api_DrawTexfv), H(glDrawTexiOES, api_DrawTexi),
    H(glDrawTexivOES, api_DrawTexiv), H(glDrawTexsOES, api_DrawTexs), H(glDrawTexsvOES, api_DrawTexsv),
    H(glDrawTexxOES, api_DrawTexx), H(glDrawTexxvOES, api_DrawTexxv),
    H(glEnable, api_Enable), H(glEnableClientState, api_EnableClientState), H(glFinish, api_Finish),
    H(glFinishFenceAPPLE, api_FinishFence), H(glFinishObjectAPPLE, api_FinishObject), H(glFlush, api_Flush),
    H(glFogf, api_Fogf), H(glFogfv, api_Fogfv), H(glFogx, api_Fogx), H(glFogxv, api_Fogxv),
    H(glFramebufferParameteriAPPLE, api_FramebufferParameteri),
    H(glFramebufferRenderbufferOES, api_FramebufferRenderbuffer),
    H(glFramebufferTexture2DOES, api_FramebufferTexture2D), H(glFrontFace, api_FrontFace),
    H(glFrustumf, api_Frustumf), H(glFrustumx, api_Frustumx), H(glGenBuffers, api_GenBuffers),
    H(glGenFencesAPPLE, api_GenFences), H(glGenFramebuffersOES, api_GenFramebuffers),
    H(glGenRenderbuffersOES, api_GenRenderbuffers), H(glGenTextures, api_GenTextures),
    H(glGenerateMipmapOES, api_GenerateMipmap), H(glGetBooleanv, api_GetBooleanv),
    H(glGetBufferParameteriv, api_GetBufferParameteriv), H(glGetBufferPointervOES, api_GetBufferPointerv),
    H(glGetError, api_GetError), H(glGetFixedv, api_GetFixedv), H(glGetFloatv, api_GetFloatv),
    H(glGetFramebufferAttachmentParameterivOES, api_GetFramebufferAttachmentParameteriv),
    H(glGetIntegerv, api_GetIntegerv), H(glGetPointerv, api_GetPointerv),
    H(glGetRenderbufferParameterivOES, api_GetRenderbufferParameteriv), H(glGetString, api_GetString),
    H(glGetTexParameterfv, api_GetTexParameterfv), H(glGetTexParameteriv, api_GetTexParameteriv),
    H(glHint, api_Hint), H(glIsBuffer, api_IsBuffer), H(glIsEnabled, api_IsEnabled),
    H(glIsFenceAPPLE, api_IsFence), H(glIsFramebufferOES, api_IsFramebuffer),
    H(glIsRenderbufferOES, api_IsRenderbuffer), H(glIsTexture, api_IsTexture),
    H(glLightModelf, api_LightModelf), H(glLightModelfv, api_LightModelfv), H(glLightModelx, api_LightModelx),
    H(glLightModelxv, api_LightModelxv), H(glLightf, api_Lightf), H(glLightfv, api_Lightfv),
    H(glLightx, api_Lightx), H(glLightxv, api_Lightxv), H(glLineWidth, api_LineWidth),
    H(glCurrentPaletteMatrixOES, api_CurrentPaletteMatrixOES),
    H(glLoadPaletteFromModelViewMatrixOES, api_LoadPaletteFromModelViewMatrixOES),
    H(glMatrixIndexPointerOES, api_MatrixIndexPointerOES), H(glWeightPointerOES, api_WeightPointerOES),
    H(glLineWidthx, api_LineWidthx), H(glLoadIdentity, api_LoadIdentity), H(glLoadMatrixf, api_LoadMatrixf),
    H(glLoadMatrixx, api_LoadMatrixx), H(glLogicOp, api_LogicOp), H(glMapBufferOES, api_MapBuffer),
    H(glMaterialf, api_Materialf), H(glMaterialfv, api_Materialfv), H(glMaterialx, api_Materialx),
    H(glMaterialxv, api_Materialxv), H(glMatrixMode, api_MatrixMode), H(glMultMatrixf, api_MultMatrixf),
    H(glMultMatrixx, api_MultMatrixx), H(glMultiTexCoord4f, api_MultiTexCoord4f),
    H(glMultiTexCoord4x, api_MultiTexCoord4x), H(glNormal3f, api_Normal3f), H(glNormal3x, api_Normal3x),
    H(glNormalPointer, api_NormalPointer), H(glOrthof, api_Orthof), H(glOrthox, api_Orthox),
    H(glPixelStorei, api_PixelStorei), H(glPointParameterf, api_PointParameterf),
    H(glPointParameterfv, api_PointParameterfv), H(glPointParameterx, api_PointParameterx),
    H(glPointParameterxv, api_PointParameterxv), H(glPointSize, api_PointSize),
    H(glPointSizePointerOES, api_PointSizePointerOES), H(glPointSizex, api_PointSizex),
    H(glPolygonOffset, api_PolygonOffset), H(glPolygonOffsetx, api_PolygonOffsetx),
    H(glPopMatrix, api_PopMatrix), H(glPushMatrix, api_PushMatrix), H(glReadPixels, api_ReadPixels),
    H(glRenderbufferStorageMultisampleAPPLE, api_RenderbufferStorageMultisample),
    H(glRenderbufferStorageOES, api_RenderbufferStorage),
    H(glResolveMultisampleFramebufferAPPLE, api_ResolveMultisampleFramebuffer),
    H(glRotatef, api_Rotatef), H(glRotatex, api_Rotatex), H(glSampleCoverage, api_SampleCoverage),
    H(glSampleCoveragex, api_SampleCoveragex), H(glScalef, api_Scalef), H(glScalex, api_Scalex),
    H(glScissor, api_Scissor), H(glSetFenceAPPLE, api_SetFence), H(glShadeModel, api_ShadeModel),
    H(glStencilFunc, api_StencilFunc), H(glStencilMask, api_StencilMask), H(glStencilOp, api_StencilOp),
    H(glTestFenceAPPLE, api_TestFence), H(glTestObjectAPPLE, api_TestObject),
    H(glTexCoordPointer, api_TexCoordPointer), H(glTexEnvf, api_TexEnvf), H(glTexEnvfv, api_TexEnvfv),
    H(glTexEnvi, api_TexEnvi), H(glTexEnviv, api_TexEnviv), H(glTexEnvx, api_TexEnvx),
    H(glTexEnvxv, api_TexEnvxv), H(glTexImage2D, api_TexImage2D), H(glTexParameterf, api_TexParameterf),
    H(glTexParameterfv, api_TexParameterfv), H(glTexParameteri, api_TexParameteri),
    H(glTexParameteriv, api_TexParameteriv), H(glTexParameterx, api_TexParameterx),
    H(glTexParameterxv, api_TexParameterxv), H(glTexSubImage2D, api_TexSubImage2D),
    H(glTranslatef, api_Translatef), H(glTranslatex, api_Translatex), H(glUnmapBufferOES, api_UnmapBuffer),
    H(glVertexPointer, api_VertexPointer), H(glViewport, api_Viewport),
};

static const char *slot_name(int slot)
{
    (void)slot;
    return "see gl_slots.h";
}

/* Fill one __GLIFunctionDispatchRec: every slot gets a logging stub, then
 * the implemented entry points go on top. */
#ifdef GL_IOS3
/* iOS 3.x (7E18) OpenGLES has an 822-slot __GLIFunctionDispatchRec: the
 * 4.x-only slots 761-776 (instanced/base-vertex draws, ARB vertex arrays,
 * multisample textures) and 838-840 (discard/APPLE framebuffer) do not
 * exist, so 4.x slots 777-837 sit 16 lower. Writing the 841-slot 4.x layout
 * overflowed OpenGLES's heap block (re-log 125). Slots 441-447 differ in
 * name only (NV combiners vs uniform blocks); the engine implements neither. */
#define GLI_TABLE_SLOTS 822
static int table_slot(unsigned s)
{
    if (s < 761)
        return (int)s;
    if (s >= 777 && s < 838)
        return (int)s - 16;
    return -1;
}
#else
#define GLI_TABLE_SLOTS GLI_NSLOTS
static int table_slot(unsigned s) { return (int)s; }
#endif

static void install(void **table, int native)
{
    unsigned i;
    int t;
    for (i = 0; i < GLI_NSLOTS; i++) {
        if ((t = native ? (int)i : table_slot(i)) < 0 || t >= (native ? GLI_NSLOTS : GLI_TABLE_SLOTS))
            continue;
#ifdef GL_HOST
        table[t] = (void *)unimpl_host;
#else
        table[t] = (char *)gl_unimpl_thunks + 12 * i;
#endif
    }
    for (i = 0; i < sizeof handlers / sizeof handlers[0]; i++) {
        t = native ? (int)handlers[i].slot : table_slot(handlers[i].slot);
        if (t >= 0 && t < (native ? GLI_NSLOTS : GLI_TABLE_SLOTS))
            table[t] = handlers[i].fn;
    }
}

/* OpenGLES's own dispatch tables (the running OS's layout) */
void gl_install_dispatch(GC *gc, void **table)
{
    (void)gc;
    install(table, 0);
}

/* the engine's internal tables, indexed by SLOT_* (ca.c's scene check) */
void gl_install_dispatch_native(GC *gc, void **table)
{
    (void)gc;
    install(table, 1);
}
