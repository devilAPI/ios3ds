/* gltest: exercises the ios3ds GL engine through the public OpenGL ES 1.1
 * API and checks the pixels it gets back.
 *
 *   gltest [out.ppm]                     host build: offscreen render + checks
 *   gltest [delay_s [seconds [out.ppm]]] iOS: EAGLContext offscreen render +
 *                                        checks, then after delay_s draws the
 *                                        animated scene straight into the main
 *                                        display's IOSurface for `seconds`
 *
 * The scene: smooth-shaded triangle, checkerboard texture (GL_LINEAR,
 * MODULATE), a 50% alpha-blended quad, a scissored clear, glDrawTexiOES, and
 * a lit, depth-tested cube under a perspective projection.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glapi.h"

#ifndef GL_TEXTURE_CROP_RECT_OES
#define GL_TEXTURE_CROP_RECT_OES 0x8B9D
#endif

static int W = 320, H = 480;
static GLuint tex;
static int fails;

static void make_texture(void)
{
    static uint32_t px[64 * 64];
    int x, y;
    GLint crop[4] = {0, 0, 64, 64};
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)            /* RGBA bytes: white / orange 8x8 cells */
            px[y * 64 + x] = ((x >> 3) ^ (y >> 3)) & 1 ? 0xff2080ffu : 0xffffffffu;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);     /* for glDrawTexiOES */
}

/* cube() variants, for narrowing down hardware failures (run14: the PICA200
   pass drew no cube while every 2D check passed) */
#define CV_NODEPTH 1
#define CV_NOCULL  2
#define CV_NOLIGHT 4
#define CV_ORTHO   8
#define CV_USHORT  16
#define CV_ARRAYS  32
static int cube_flags;

static void cube(float t)
{
    static const GLfloat v[] = {
        -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,     /* +z */
         1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,     /* -z */
         1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,     /* +x */
        -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,     /* -x */
        -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,     /* +y */
        -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,     /* -y */
    };
    static const GLfloat fn[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const GLfloat fc[6][4] = {{1,.3f,.3f,1},{.3f,1,.3f,1},{.3f,.3f,1,1},{1,1,.3f,1},{.3f,1,1,1},{1,.3f,1,1}};
    static GLfloat n[72], c[96];
    static const GLubyte idx[36] = {0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11, 12,13,14,12,14,15,
                                    16,17,18,16,18,19, 20,21,22,20,22,23};
    static const GLfloat lpos[4] = {1, 2, 3, 0}, amb[4] = {.25f, .25f, .25f, 1}, dif[4] = {1, 1, 1, 1};
    int f, k;
    for (f = 0; f < 6; f++)
        for (k = 0; k < 4; k++) {
            memcpy(n + (f * 4 + k) * 3, fn[f], 12);
            memcpy(c + (f * 4 + k) * 4, fc[f], 16);
        }
    glViewport(W / 8, H / 32, W * 3 / 4, H * 3 / 8);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (cube_flags & CV_ORTHO)
        glOrthof(-2.5f, 2.5f, -2.5f, 2.5f, 2, 10);
    else
        glFrustumf(-1, 1, -1, 1, 2, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -5);
    glRotatef(30 + t * 40, 1, 0, 0);
    glRotatef(40 + t * 60, 0, 1, 0);
    if (!(cube_flags & CV_NODEPTH)) glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (!(cube_flags & CV_NOCULL)) glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    if (!(cube_flags & CV_NOLIGHT)) glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glNormalPointer(GL_FLOAT, 0, n);
    glColorPointer(4, GL_FLOAT, 0, c);
    if (cube_flags & CV_ARRAYS) {               /* non-indexed: expand the triangles */
        static GLfloat ev[36 * 3], en[36 * 3], ec[36 * 4];
        int i;
        for (i = 0; i < 36; i++) {
            memcpy(ev + i * 3, v + idx[i] * 3, 12);
            memcpy(en + i * 3, n + idx[i] * 3, 12);
            memcpy(ec + i * 4, c + idx[i] * 4, 16);
        }
        glVertexPointer(3, GL_FLOAT, 0, ev);
        glNormalPointer(GL_FLOAT, 0, en);
        glColorPointer(4, GL_FLOAT, 0, ec);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    } else if (cube_flags & CV_USHORT) {
        static unsigned short idx16[36];
        int i;
        for (i = 0; i < 36; i++) idx16[i] = idx[i];
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, idx16);
    } else {
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, idx);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}

/* Layout in GL coordinates (origin bottom left), for a W x H target. */
#define SX(x) ((float)(x) * W / 320)
#define SY(y) ((float)(y) * H / 480)

static void draw_scene(float t)
{
    GLfloat tri[6] = {SX(20), SY(300), SX(150), SY(300), SX(85), SY(440)};
    static const GLfloat tricol[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    GLfloat quad[8] = {SX(170), SY(300), SX(300), SY(300), SX(170), SY(430), SX(300), SY(430)};
    static const GLfloat uv[8] = {0, 0, 2, 0, 0, 2, 2, 2};
    GLfloat blend[8] = {SX(230), SY(250), SX(310), SY(250), SX(230), SY(350), SX(310), SY(350)};

    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.1f, 0.12f, 0.25f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glShadeModel(GL_SMOOTH);

    /* smooth-shaded triangle, spinning about its centroid */
    glPushMatrix();
    glTranslatef(SX(85), SY(346.67f), 0);
    glRotatef(t * 90, 0, 0, 1);
    glTranslatef(-SX(85), -SY(346.67f), 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, tri);
    glColorPointer(4, GL_FLOAT, 0, tricol);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_COLOR_ARRAY);
    glPopMatrix();

    /* textured quad (2x2 repeats of the checkerboard) */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quad);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    /* glDrawTexiOES: the texture, unscaled, bottom left */
    glDrawTexiOES((int)SX(8), (int)SY(200), 0, 64, 64);
    glDisable(GL_TEXTURE_2D);

    /* 50% red, blended */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f);
    glVertexPointer(2, GL_FLOAT, 0, blend);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);

    /* scissored clear: a yellow bar */
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)SX(90), (int)SY(250), (int)SX(120), (int)SY(30));
    glClearColor(1, 0.85f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    cube(t);
    glDisableClientState(GL_VERTEX_ARRAY);
}


static void e_check(const char *what)
{
    GLenum e = glGetError();
    if (e) {
        printf("gltest: %s: glGetError = 0x%x FAIL\n", what, e);
        fails++;
    }
}

/* ---- scene 2: the less common paths, only compared between back ends ----
 * A viewport that hangs over the target, an NPOT texture, two texture units,
 * alpha test, GL_COMBINE, colour masks, a masked + scissored clear, depth
 * mask, and render-to-texture (target switches). */
static void quad(float x0, float y0, float x1, float y1, float z)
{
    GLfloat v[12] = {x0, y0, z, x1, y0, z, x0, y1, z, x1, y1, z};
    glVertexPointer(3, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_scene2(GLuint main_fb)
{
    static uint32_t npot[24 * 48], cut[16 * 16];
    static const GLfloat uv[8] = {0, 0, 1, 0, 0, 1, 1, 1}, uv3[8] = {0, 0, 3, 0, 0, 3, 3, 3};
    static const GLfloat envc[4] = {0.2f, 0.9f, 0.3f, 1};
    static const GLfloat tri[9] = {4, 4, 0, 60, 8, 0, 30, 58, 0};
    static const GLfloat tricol[12] = {1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    GLuint t[3], fb;
    int x, y;
    for (y = 0; y < 24; y++)
        for (x = 0; x < 48; x++)            /* RGBA bytes: gradient with a diagonal */
            npot[y * 48 + x] = 0xff000000u | (uint32_t)(x * 5) | (uint32_t)(y * 10) << 8 | (x == 2 * y ? 0xff0000u : 0);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)            /* opaque disc on a transparent ground */
            cut[y * 16 + x] = (x - 8) * (x - 8) + (y - 8) * (y - 8) < 40 ? 0xff40c0ffu : 0x00ffffffu;
    glGenTextures(3, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 48, 24, 0, GL_RGBA, GL_UNSIGNED_BYTE, npot);
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, cut);

    /* render to a 64x64 texture first */
    glBindTexture(GL_TEXTURE_2D, t[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersOES(1, &fb);
    glBindFramebufferOES(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[2], 0);
    glViewport(0, 0, 64, 64);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.3f, 0.1f, 0.4f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri);
    glColorPointer(4, GL_FLOAT, 0, tricol);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_COLOR_ARRAY);

    /* back to the main target, with a viewport that hangs over all four edges */
    glBindFramebufferOES(GL_FRAMEBUFFER, main_fb);
    glViewport(-40, -60, W + 80, H + 120);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(-40, W + 40, -60, H + 60, -1, 1);
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor4f(0.9f, 0.2f, 0.2f, 1);
    quad(-30, 400, 120, 520, 0);            /* cut off by the left and top edges */

    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, t[0]);     /* NPOT, REPLACE */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(140, 380, 300, 460, 0);
    glBindTexture(GL_TEXTURE_2D, t[2]);     /* the rendered texture */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    quad(20, 250, 148, 378, 0);

    glBindTexture(GL_TEXTURE_2D, t[1]);     /* alpha test: discs, 3x3 repeats */
    glTexCoordPointer(2, GL_FLOAT, 0, uv3);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    quad(170, 250, 300, 370, 0);
    glDisable(GL_ALPHA_TEST);

    /* GL_COMBINE: interpolate texture and env colour by the vertex alpha */
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_PRIMARY_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
    glColor4f(1, 1, 1, 0.35f);
    quad(20, 130, 140, 240, 0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);

    /* two units: checkerboard modulated, the NPOT texture added on top */
    glActiveTexture(GL_TEXTURE0 + 1);
    glClientActiveTexture(GL_TEXTURE0 + 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    quad(170, 130, 300, 240, 0);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    /* depth: near quad first, then a far one that must stay behind; a third
       drawn with the depth mask off must not block the fourth */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glColor4f(0.1f, 0.8f, 0.9f, 1);
    quad(30, 20, 130, 100, 0.5f);
    glColor4f(0.9f, 0.8f, 0.1f, 1);
    quad(80, 40, 200, 120, -0.5f);
    glDepthMask(0);
    glColor4f(0.9f, 0.1f, 0.9f, 1);
    quad(150, 10, 230, 60, 0.9f);
    glDepthMask(1);
    glColor4f(0.2f, 0.2f, 0.2f, 1);
    quad(190, 30, 300, 110, 0.7f);
    glDisable(GL_DEPTH_TEST);

    /* colour mask on a draw, then a masked, scissored clear */
    glColorMask(1, 0, 0, 1);
    glColor4f(1, 1, 1, 1);
    quad(240, 400, 330, 470, 0);            /* also cut off on the right */
    glColorMask(0, 1, 0, 1);
    glEnable(GL_SCISSOR_TEST);
    glScissor(100, 180, 150, 30);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDisableClientState(GL_VERTEX_ARRAY);
    glViewport(0, 0, W, H);
    e_check("scene 2");
}

static int near(const GLubyte *p, int r, int g, int b, int tol)
{
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

static void check(const char *what, float x, float y, int r, int g, int b, int tol, int invert)
{
    GLubyte p[4];
    int ok;
    glReadPixels((int)x, (int)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    ok = near(p, r, g, b, tol) != invert;
    printf("gltest: %-28s (%3d,%3d) = %3d %3d %3d %s\n", what, (int)x, (int)y, p[0], p[1], p[2], ok ? "ok" : "FAIL");
    fails += !ok;
}

/* Draw only the cube with each variant and report whether its centre is
   drawn (background 26,31,64). Logged only for the GPU pass when it failed. */
static void cube_variants(void)
{
    static const struct { int f; const char *name; } v[] = {
        {0, "as in scene 1"}, {CV_NODEPTH, "no depth test"}, {CV_NOCULL, "no culling"},
        {CV_NOLIGHT, "no lighting"}, {CV_ORTHO, "orthographic"}, {CV_USHORT, "16-bit indices"},
        {CV_ARRAYS, "glDrawArrays"}, {CV_NODEPTH | CV_NOCULL | CV_NOLIGHT, "no depth/cull/lighting"},
        {CV_NODEPTH | CV_NOCULL | CV_NOLIGHT | CV_ORTHO | CV_ARRAYS, "all of these"},
    };
    unsigned i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        GLubyte p[4];
        glViewport(0, 0, W, H);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(26 / 255.f, 31 / 255.f, 64 / 255.f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        cube_flags = v[i].f;
        cube(0);
        glFinish();
        glReadPixels((int)SX(160), (int)SY(105), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        printf("gltest: cube variant %-24s: centre %3d %3d %3d -> %s\n", v[i].name, p[0], p[1], p[2],
               near(p, 26, 31, 64, 6) ? "NOT drawn" : "drawn");
    }
    cube_flags = 0;
}

static void run_checks(void)
{
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    GLenum e;
    printf("gltest: GL_VERSION \"%s\", GL_RENDERER \"%s\"\n", ver ? ver : "?", ren ? ren : "?");
    check("clear colour", SX(160), SY(470), 26, 31, 64, 2, 0);
    check("triangle centroid (grey)", SX(85), SY(347), 85, 85, 85, 12, 0);
    check("triangle red corner", SX(24), SY(302), 255, 0, 0, 40, 0);
    check("texture: white cell", SX(170) + 4, SY(300) + 4, 255, 255, 255, 8, 0);
    check("texture: orange cell", SX(170) + 12 * W / 320.f, SY(300) + 4, 255, 128, 32, 12, 0);
    check("drawtex: white cell", SX(8) + 3, SY(200) + 3, 255, 255, 255, 8, 0);
    check("blend over background", SX(305), SY(260), 140, 15, 32, 4, 0);
    check("scissored clear", SX(150), SY(265), 255, 217, 0, 2, 0);
    check("outside scissor", SX(150), SY(285), 26, 31, 64, 2, 0);
    check("cube centre is drawn", SX(160), SY(105), 26, 31, 64, 6, 1);
    e = glGetError();
    printf("gltest: glGetError = 0x%x %s\n", e, e ? "FAIL" : "ok");
    fails += e != 0;
}

static void write_ppm(const char *path)
{
    unsigned char *px = malloc((size_t)W * H * 4);
    FILE *f;
    int y, x;
    if (!px || !(f = fopen(path, "wb"))) {
        free(px);
        return;
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = H - 1; y >= 0; y--)
        for (x = 0; x < W; x++)
            fwrite(px + ((size_t)y * W + x) * 4, 1, 3, f);
    fclose(f);
    free(px);
    printf("gltest: wrote %s\n", path);
}

/* The whole frame as RGBA bytes, for comparing two back ends. */
static unsigned char *grab(void)
{
    unsigned char *px = malloc((size_t)W * H * 4);
    if (px)
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px;
}

/* Pixel-compare the GPU back end's frame with the software rasterizer's.
   Edges may differ by a pixel (fill rules) and colours by a few steps
   (the GPU works in 8-bit fixed point), so: count pixels that differ by more
   than 24 in a channel; up to 1 % of the frame is tolerated. */
static void compare(const char *what, const unsigned char *a, const unsigned char *b)
{
    long n = (long)W * H, bad = 0, any = 0, i;
    int max = 0, c;
    for (i = 0; i < n; i++) {
        int d = 0;
        for (c = 0; c < 4; c++) {
            int e = abs(a[i * 4 + c] - b[i * 4 + c]);
            if (e > d)
                d = e;
        }
        if (d > max)
            max = d;
        any += d > 0;
        bad += d > 24;
    }
    printf("gltest: %s, pica vs software: %ld of %ld pixels differ at all, %ld by more than 24 (max %d) %s\n",
           what, any, n, bad, max, bad * 100 <= n ? "ok" : "FAIL");
    fails += bad * 100 > n;
}

/* An offscreen framebuffer: RGBA8 colour + 16-bit depth renderbuffers. */
static GLuint off_fb;

#ifdef GL_HOST
/* The stereo depth stamp (docs/stereo3d.md) lives in the target's alpha
   byte and must survive the write-back into the surface: with
   IOS3DS_GL_PRESENT_ALL=1 that is the GPU's direct present (the path the
   stereo3d=1 display target now uses), otherwise the CPU read-back. Four
   bands get an alpha-only "stamp" (colour mask A) over an opaque colour;
   RGB must be untouched and alpha must read back exactly. */
static void alpha_stamp_check(const char *backend)
{
    static const unsigned char stamp[4] = { 0x33, 0x66, 0x99, 0xCC };
    unsigned char *px;
    int band, bad = 0, x, y;
    glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0.2f, 0.4f, 0.6f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(0, 0, 0, 1);
    for (band = 0; band < 4; band++) {
        glScissor(band * W / 4, 0, W / 4, H);
        glClearColor(0, 0, 0, stamp[band] / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glColorMask(1, 1, 1, 1);
    glDisable(GL_SCISSOR_TEST);
    px = grab();
    if (!px)
        return;
    for (y = 0; y < H; y += 7)
        for (x = 0; x < W; x += 5) {
            const unsigned char *p = px + ((size_t)y * W + x) * 4;
            int want = stamp[x / (W / 4) < 4 ? x / (W / 4) : 3];
            if (abs(p[0] - 51) > 2 || abs(p[1] - 102) > 2 || abs(p[2] - 153) > 2 || p[3] != want)
                bad++;
        }
    free(px);
    printf("gltest: %s stereo alpha stamp through the write-back: %d bad samples %s\n", backend, bad,
           bad ? "FAIL" : "ok");
    fails += bad != 0;
}
#endif

static int offscreen(void)
{
    GLuint fb, rb[2];
    GLenum st;
    glGenFramebuffersOES(1, &fb);
    off_fb = fb;
    glBindFramebufferOES(GL_FRAMEBUFFER, fb);
    glGenRenderbuffersOES(2, rb);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    st = glCheckFramebufferStatusOES(GL_FRAMEBUFFER);
    printf("gltest: offscreen %dx%d framebuffer status 0x%x\n", W, H, st);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

/* Compressed textures: the extension must be advertised and 2bpp/4bpp
   PVRTC images must decode at upload on both back ends. The blocks are
   encoded here (no app or image files are used). */
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void pvrtc_check(const char *backend)
{
    static const GLfloat tn[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    uint8_t data[64];
    GLuint t[2];
    GLubyte c[4];
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLint n = 0, fmt[16];
    int bad = 0, i, has = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, fmt);
    for (i = 0; i < n && i < 16; i++)
        if (fmt[i] >= 0x8C00 && fmt[i] <= 0x8C03)
            has++;
    if (!ext || !strstr(ext, "GL_IMG_texture_compression_pvrtc") || n != 14 || has != 4)
        bad++;
    glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, tn);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glGenTextures(2, t);
    for (i = 0; i < 4; i++)                 /* RGB 4bpp, endpoints both opaque red */
        put64(data + i * 8, (uint64_t)0 | ((uint64_t)(31u << 10) << 33) |
              ((uint64_t)1 << 48) | ((uint64_t)(31u << 9) << 49) | ((uint64_t)1 << 63));
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, 8, 8, 0, sizeof data, data);
    quad(10, 10, 70, 70, 0);
    glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    if (abs(c[0] - 255) > 4 || c[1] > 4 || c[2] > 4)
        bad++;
    for (i = 0; i < 2; i++)                 /* RGB 2bpp, endpoints both blue */
        put64(data + i * 8, ((uint64_t)31u << 33) | ((uint64_t)31u << 49));
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, 8, 8, 0, 16, data);
    quad(100, 10, 160, 70, 0);
    glReadPixels(130, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    if (abs(c[2] - 255) > 4 || c[0] > 4 || c[1] > 4)
        bad++;
    /* a truncated PVRTC image must fail cleanly, not read past the buffer */
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, 8, 8, 0, 8, data);
    if (glGetError() != GL_INVALID_VALUE)
        bad++;
    glDeleteTextures(2, t);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    printf("gltest: %s: PVRTC 2bpp/4bpp decode and extension list: %s\n", backend, bad ? "FAIL" : "ok");
    fails += bad;
}

/* OES_matrix_palette: the index/weight arrays must move the vertices, and
   glGenerateMipmapOES must build a chain instead of logging an error. */
static void matrix_palette_check(const char *backend)
{
    static const GLubyte idx[4] = {0, 0, 0, 0};
    static const GLfloat wt[4] = {1, 1, 1, 1};
    GLubyte at140[4] = {0, 0, 0, 0}, at40[4] = {0, 0, 0, 0};
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLenum miperr;
    int bad = 0;
    glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(100, 0, 0);
    glCurrentPaletteMatrixOES(0);
    glLoadPaletteFromModelViewMatrixOES();
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glColor4f(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_MATRIX_INDEX_ARRAY_OES);
    glEnableClientState(GL_WEIGHT_ARRAY_OES);
    glMatrixIndexPointerOES(1, GL_UNSIGNED_BYTE, 0, idx);
    glWeightPointerOES(1, GL_FLOAT, 0, wt);
    quad(10, 10, 70, 70, 0);
    glDisableClientState(GL_MATRIX_INDEX_ARRAY_OES);
    glDisableClientState(GL_WEIGHT_ARRAY_OES);
    glReadPixels(140, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, at140);
    if (at140[0] < 250 || at140[1] < 250 || at140[2] < 250)
        bad++;                                  /* the translated quad covers (140,40) */
    glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, at40);
    if (at40[0] > 5 || at40[1] > 5 || at40[2] > 5)        /* ... and not (40,40) */
        bad++;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenerateMipmapOES(GL_TEXTURE_2D);
    miperr = glGetError();
    if (miperr != GL_NO_ERROR)
        bad++;
    if (!ext || !strstr(ext, "GL_OES_matrix_palette"))
        bad++;
    printf("gltest: %s: matrix palette (at 140:%d,%d,%d; at 40:%d,%d,%d) + mipmap (err 0x%x, ext %s): %s\n",
           backend, at140[0], at140[1], at140[2], at40[0], at40[1], at40[2], miperr,
           ext && strstr(ext, "GL_OES_matrix_palette") ? "yes" : "no", bad ? "FAIL" : "ok");
    fails += bad;
}

/* An ES2-only call through an ES1 context must fail with an error instead of
   a silent 0: games probe for ES2 and fall back to ES1 when it is refused. */
static void es2_check(const char *backend)
{
    GLuint sh = glCreateShader(0x8B30);          /* GL_FRAGMENT_SHADER */
    GLenum e = glGetError();
    printf("gltest: %s: ES 2.0 shader request: shader %u, error 0x%x %s\n", backend, sh, e,
           sh == 0 && e == GL_INVALID_OPERATION ? "ok" : "FAIL");
    fails += sh != 0 || e != GL_INVALID_OPERATION;
    glGetError();                               /* leave a clean error state */
}

#ifdef GL_HOST
void host_gl_init(void);
extern int setenv(const char *, const char *, int);

/* QuartzCore binds IOSurface images with target 0x84F5 (rectangle: texel
   coordinates). The same texture drawn once as GL_TEXTURE_2D with 0..1 and
   once as 0x84F5 with 0..64 must give the same pixels. */
static void rect_texture_check(const char *backend)
{
    static const GLfloat tn[8] = { 0, 0, 1, 0, 0, 1, 1, 1 }, tr[8] = { 0, 0, 64, 0, 0, 64, 64, 64 };
    GLubyte a[4 * 64 * 64], b[4 * 64 * 64];
    int i, bad = 0;
    glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glColor4f(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexCoordPointer(2, GL_FLOAT, 0, tn);
    quad(0, 0, 64, 64, 0);
    glBindTexture(0x84F5, tex);
    glTexCoordPointer(2, GL_FLOAT, 0, tr);
    quad(100, 0, 164, 64, 0);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, a);
    glReadPixels(100, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, b);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    for (i = 0; i < 4 * 64 * 64; i++)
        bad += a[i] != b[i];
    printf("gltest: %s: rectangle texture (0x84F5, texel coordinates) vs 2D: %d bytes differ %s\n", backend, bad,
           bad || glGetError() ? "FAIL" : "ok");
    if (bad)
        fails++;
}

/* More texture memory than the GPU arena holds (hardware run 15: CA's
   layers filled it and later textures sampled black): every texture must
   still draw in its own colour, the back end evicts and re-uploads, and the
   GPU stays on. */
typedef struct {                        /* engine.h PicaStats */
    unsigned lists, draws, readbacks, readback_kb, uploads, upload_kb, presents, bounces;
    unsigned arena_kb, tex_kb, ntex, evictions, arena_free_kb, gpu_on;
} PicaStats;
extern void pica_backend_stats2(PicaStats *s);

static void arena_churn_check(const char *backend)
{
    enum { N = 24, D = 512 };
    static const GLfloat tn[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    GLuint t[N];
    uint32_t *px = malloc((size_t)D * D * 4);
    int i, k, bad = 0;
    GLubyte c[4];
    PicaStats st;
    if (!px)
        return;
    glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexCoordPointer(2, GL_FLOAT, 0, tn);
    glGenTextures(N, t);
    for (k = 0; k < 2 * N; k++) {       /* each texture twice: the second round re-uploads evicted ones */
        i = k % N;
        if (k < N) {
            uint32_t col = 0xFF000000u | (uint32_t)(i * 10) << 16 | (uint32_t)(255 - i * 10) << 8 | 0x40;
            int j;
            for (j = 0; j < D * D; j++)
                px[j] = col;
            glBindTexture(GL_TEXTURE_2D, t[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, D, D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        glBindTexture(GL_TEXTURE_2D, t[i]);
        quad(10, 10, 30, 30, 0);
        glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        if (abs(c[2] - i * 10) > 2 || abs(c[1] - (255 - i * 10)) > 2 || abs(c[0] - 0x40) > 2)
            bad++;
    }
    glDeleteTextures(N, t);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    free(px);
    memset(&st, 0, sizeof st);
    pica_backend_stats2(&st);
    if (!strcmp(backend, "pica")) {
        printf("gltest: %s: %d MiB of textures through the arena: %d of %d draws wrong, %u evictions, GPU %s %s\n",
               backend, N * D * D * 4 >> 20, bad, 2 * N, st.evictions, st.gpu_on ? "on" : "OFF",
               bad || !st.evictions || !st.gpu_on ? "FAIL" : "ok");
        fails += bad || !st.evictions || !st.gpu_on;
    } else {
        printf("gltest: %s: %d of %d texture-churn draws wrong %s\n", backend, bad, 2 * N, bad ? "FAIL" : "ok");
        fails += bad != 0;
    }
}


/* IOS3DS_GLTEST_STEREO=1: a SpringBoard-like home frame through the stereo
   stamp (stereo.c) on the PICA200 back end. The test-only hook
   IOS3DS_STEREO_ANY_RT lets the offscreen target stand in for CA's display
   surface. Checks the class map the GPU mirror reads: wallpaper SA_BG,
   icons SA_ICONS, dock SA_FRONT, colours untouched by the stamp, and a
   following app frame (a second opaque full-screen draw) wiped flat. */
static int stereo_frame_check(void)
{
    static const struct { int x, y; unsigned char a; const char *what; } probe[] = {
        { 5, 200, 0x33, "wallpaper" }, { 60, 150, 0x66, "icon" }, { 200, 300, 0x66, "icon" },
        { 160, 440, 0x99, "dock" }, { 160, 8, 0x99, "status bar" },
    };
    unsigned char px[4];
    unsigned i;
    int bad = 0, k;
    unsigned nb0 = 0, nv0 = 0;
    setenv("IOS3DS_GL", "pica", 1);
    setenv("IOS3DS_CA", "1", 1);
    setenv("IOS3DS_STEREO", "1", 1);
    setenv("IOS3DS_STEREO_ANY_RT", "1", 1);
    host_gl_init();
    if (!offscreen())
        return 1;
    glFlush();                          /* end the engine's start-up checks' frame */
    {
        extern void stereo_batch_stats(unsigned *batches, unsigned *verts);
        stereo_batch_stats(&nb0, &nv0);
    }
    for (k = 0; k < 2; k++) {
        glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
        glViewport(0, 0, W, H);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(0, W, 0, H, -1, 1);    /* y = GL row: the classifier's row 0 is the iOS top */
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnableClientState(GL_VERTEX_ARRAY);
        glColor4f(0.2f, 0.3f, 0.4f, 1);
        quad(0, 0, W, H, 0);                        /* wallpaper */
        glColor4f(0.5f, 0.5f, 0.5f, 1);
        for (i = 0; i < 8; i++)                     /* icon grid */
            quad(40 + (i % 4) * 70, 120 + (i / 4) * 160, 90 + (i % 4) * 70, 190 + (i / 4) * 160, 0);
        glColor4f(0.7f, 0.7f, 0.7f, 1);
        quad(10, 400, W - 10, 470, 0);              /* dock */
        quad(0, 0, W, 20, 0);                       /* status bar */
        if (k) {
            glColor4f(1, 1, 1, 1);
            quad(0, 0, W, H, 0);                    /* an app: covers everything */
        }
        glFlush();
        for (i = 0; i < sizeof probe / sizeof probe[0]; i++) {
            unsigned char want = k ? 0 : probe[i].a;
            glReadPixels(probe[i].x, probe[i].y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (px[3] != want) {
                printf("gltest: stereo frame %d: %s at %d,%d alpha 0x%02x, want 0x%02x FAIL\n", k, probe[i].what,
                       probe[i].x, probe[i].y, px[3], want);
                bad++;
            }
        }
        glReadPixels(60, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (abs(px[0] - (k ? 255 : 128)) > 2)
            printf("gltest: stereo frame %d: icon colour %d FAIL\n", k, px[0]), bad++;
    }
    {
        extern void stereo_batch_stats(unsigned *batches, unsigned *verts);
        unsigned nb = 0, nv = 0;
        stereo_batch_stats(&nb, &nv);
        nb -= nb0;
        nv -= nv0;
        printf("gltest: stereo stamp: %u batched alpha draws, %u vertices (home frame + wiped app frame) %s\n", nb,
               nv, nb == 1 && nv == 6 * 11 ? "ok" : "FAIL");
        bad += !(nb == 1 && nv == 6 * 11);
    }
    /* Transitions: a small damage update (no full-screen draw, < 16 draws)
       keeps the class map of the scene it updates. After the app frame it
       must not stamp (the app stays flat); after a home frame it must stamp
       its own pixels and leave the rest of the map alone (before, it was
       wiped flat: a flat flash and a half-flat map during animations). */
    for (k = 0; k < 3; k++) {
        static const struct { int x, y; unsigned char a[3]; const char *what; } p2[] = {
            { 60, 150, { 0xff, 0x66, 0x66 }, "updated icon" },   /* 0xff: CA's own alpha, no class */
            { 5, 200, { 0x00, 0x33, 0x33 }, "wallpaper" },
            { 160, 440, { 0x00, 0x99, 0x99 }, "dock" },
        };
        glBindFramebufferOES(GL_FRAMEBUFFER, off_fb);
        glViewport(0, 0, W, H);
        glColor4f(0.5f, 0.5f, 0.5f, 1);
        if (k == 1) {                                /* the home screen again */
            glColor4f(0.2f, 0.3f, 0.4f, 1);
            quad(0, 0, W, H, 0);
            glColor4f(0.5f, 0.5f, 0.5f, 1);
            for (i = 0; i < 8; i++)
                quad(40 + (i % 4) * 70, 120 + (i / 4) * 160, 90 + (i % 4) * 70, 190 + (i / 4) * 160, 0);
            glColor4f(0.7f, 0.7f, 0.7f, 1);
            quad(10, 400, W - 10, 470, 0);
        } else {                                     /* two-draw damage update */
            quad(40, 120, 90, 190, 0);
            quad(110, 120, 160, 190, 0);
        }
        glFlush();
        for (i = 0; i < sizeof p2 / sizeof p2[0]; i++) {
            glReadPixels(p2[i].x, p2[i].y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (px[3] != p2[i].a[k]) {
                printf("gltest: stereo partial %d: %s alpha 0x%02x, want 0x%02x FAIL\n", k, p2[i].what, px[3],
                       p2[i].a[k]);
                bad++;
            }
        }
    }
    printf("gltest: stereo %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}

static void one_pass(const char *backend, const char *ppm, unsigned char *out[2])
{
    char path[1024];
    setenv("IOS3DS_GL", backend, 1);
    host_gl_init();
    if (!offscreen())
        exit(1);
    make_texture();
    draw_scene(0);
    run_checks();
    if (getenv("IOS3DS_GLTEST_VARIANTS"))
        cube_variants();
    if (ppm)
        write_ppm(ppm);
    out[0] = grab();
    draw_scene2(off_fb);
    if (ppm) {
        snprintf(path, sizeof path, "%s.scene2.ppm", ppm);
        write_ppm(path);
    }
    out[1] = grab();
    rect_texture_check(backend);
    arena_churn_check(backend);
    alpha_stamp_check(backend);
    pvrtc_check(backend);
    matrix_palette_check(backend);
    es2_check(backend);
}

int main(int argc, char **argv)
{
    char path[1024];
    unsigned char *sw[2], *hw[2];
    if (getenv("IOS3DS_GLTEST_STEREO"))
        return stereo_frame_check();
    one_pass("sw", argc > 1 ? argv[1] : NULL, sw);
    if (argc > 1)
        snprintf(path, sizeof path, "%s.pica.ppm", argv[1]);
    one_pass("pica", argc > 1 ? path : NULL, hw);
    if (!strstr((const char *)glGetString(GL_RENDERER), "PICA")) {
        printf("gltest: the PICA200 back end did not come up FAIL\n");
        fails++;
    } else if (sw[0] && hw[0] && sw[1] && hw[1]) {
        compare("scene 1", sw[0], hw[0]);
        compare("scene 2", sw[1], hw[1]);
    }
    {   /* the CoreAnimation gate (ca.c): config, back end, CA-like scene GPU vs CPU */
        extern int pica_ca_gate(void);
        int g = pica_ca_gate();
        printf("gltest: CA compositor gate: %s %s\n", g ? "GPU" : "software", g ? "ok" : "FAIL");
        fails += !g;
    }
    printf("gltest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

#else /* iOS */
extern int gettimeofday(struct timeval *, void *);     /* the stub SDK has no <sys/time.h> */
extern int setenv(const char *, const char *, int);
extern char *getenv(const char *);

typedef void *id;
typedef void *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
#define MSG(type, obj, sel) ((type)objc_msgSend)(obj, sel_registerName(sel)

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

/* armv6 Darwin timeval is {long, int}; filled with a marker so a failing
   call (hardware run10/11: "frame took 0 ms") is visible as such */
struct tv32 { long tv_sec; int tv_usec; };
static struct tv32 last_tv;
static int last_tv_rc, last_tv_errno;
extern int *__error(void);
extern unsigned long long mach_absolute_time(void);
struct mtb { unsigned numer, denom; };
extern int mach_timebase_info(struct mtb *);

static double now(void)
{
    struct tv32 tv = { 0x5a5a5a5a, 0x5a5a5a5a };
    *__error() = 0;
    last_tv_rc = gettimeofday((struct timeval *)(void *)&tv, NULL);
    last_tv_errno = *__error();
    last_tv = tv;
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double mach_ms(unsigned long long ticks)
{
    struct mtb tb = { 0, 0 };
    mach_timebase_info(&tb);
    return tb.denom ? (double)ticks * tb.numer / tb.denom / 1e6 : -1;
}

/* The main display's framebuffer IOSurface (what QuartzCore composites into). */
static void *screen_surface(void)
{
    void *h = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", 1);
    int (*get_main)(void **) = h ? (int (*)(void **))dlsym(h, "IOMobileFramebufferGetMainDisplay") : NULL;
    int (*get_surf)(void *, int, void **) =
        h ? (int (*)(void *, int, void **))dlsym(h, "IOMobileFramebufferGetLayerDefaultSurface") : NULL;
    void *fb = NULL, *s = NULL;
    int r1 = -1, r2 = -1;
    if (get_main)
        r1 = get_main(&fb);
    if (fb && get_surf)
        r2 = get_surf(fb, 0, &s);
    printf("gltest: main display %p (0x%x), layer 0 surface %p (0x%x)\n", fb, r1, s, r2);
    return s;
}

int main(int argc, char **argv)
{
    int delay = argc > 1 ? atoi(argv[1]) : 0;
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    id cls, ctx;
    void *surf;
    GLuint fb, rb[2];
    GLint w = 0, h = 0;
    double t0, t;
    unsigned frames = 0;

    setvbuf(stdout, NULL, 1 /* _IOLBF */, 0);
    cls = objc_getClass("EAGLContext");
    ctx = MSG(id (*)(id, SEL), cls, "alloc"));
    ctx = MSG(id (*)(id, SEL, int), ctx, "initWithAPI:"), 1);
    printf("gltest: EAGLContext %p\n", ctx);
    if (!ctx)
        return 1;
    MSG(signed char (*)(id, SEL, id), cls, "setCurrentContext:"), ctx);

    if (!offscreen())
        return 1;
    make_texture();
    t0 = now();
    struct tv32 tv0 = last_tv;
    int rc0 = last_tv_rc, e0 = last_tv_errno;
    unsigned long long m0 = mach_absolute_time();
    draw_scene(0);
    glFinish();
    unsigned long long m1 = mach_absolute_time();
    printf("gltest: offscreen frame took %.0f ms\n", (now() - t0) * 1000);
    printf("gltest: timing: gettimeofday rc %d/%d errno %d/%d tv %ld.%06d -> %ld.%06d; "
           "mach_absolute_time %llu ticks = %.0f ms\n", rc0, last_tv_rc, e0, last_tv_errno,
           tv0.tv_sec, tv0.tv_usec, last_tv.tv_sec, last_tv.tv_usec, m1 - m0, mach_ms(m1 - m0));
    run_checks();
    es2_check("sw");
    {   /* a game's usual ES2 probe: EAGL must return nil, not a context our
           ES1-only engine cannot serve; ES1 is still current afterwards */
        id alloc2 = MSG(id (*)(id, SEL), cls, "alloc"));
        id es2 = MSG(id (*)(id, SEL, int), alloc2, "initWithAPI:"), 2);
        printf("gltest: EAGL ES2 context request: %s (ES1 fallback continues)\n", es2 ? "ACCEPTED" : "refused");
        fails += es2 != NULL;
        if (es2)
            MSG(void (*)(id, SEL), es2, "release"));
    }
    if (argc > 3)
        write_ppm(argv[3]);
    printf("gltest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);

    /* The same scene on the PICA200 back end (a second context created with
       IOS3DS_GL=pica), compared pixel by pixel with the frame above. If the
       GPU is not usable the engine says why on the console and falls back. */
    {
        unsigned char *sw = grab(), *sw2, *hw;
        id ctx2;
        int before = fails;
        pvrtc_check("sw");
        matrix_palette_check("sw");
        draw_scene2(off_fb);
        sw2 = grab();
        setenv("IOS3DS_GL", "pica", 1);
        ctx2 = MSG(id (*)(id, SEL), cls, "alloc"));
        ctx2 = MSG(id (*)(id, SEL, int), ctx2, "initWithAPI:"), 1);
        if (ctx2) {
            MSG(signed char (*)(id, SEL, id), cls, "setCurrentContext:"), ctx2);
            if (!strstr((const char *)glGetString(GL_RENDERER), "PICA")) {
                printf("gltest: PICA200 back end not available (see [gl] pica lines above): GPU pass skipped\n");
                MSG(signed char (*)(id, SEL, id), cls, "setCurrentContext:"), ctx);
            } else if (offscreen()) {
                make_texture();
                t0 = now();
                draw_scene(0);
                glFinish();
                printf("gltest: PICA200 offscreen frame took %.0f ms\n", (now() - t0) * 1000);
                run_checks();
                hw = grab();
                if (sw && hw)
                    compare("scene 1", sw, hw);
                free(hw);
                pvrtc_check("pica");
                matrix_palette_check("pica");
                draw_scene2(off_fb);
                hw = grab();
                if (sw2 && hw)
                    compare("scene 2", sw2, hw);
                free(hw);
                printf("gltest: PICA200 %s (%d failed)\n", fails > before ? "FAIL" : "PASS", fails - before);
                if (fails > before || getenv("IOS3DS_GLTEST_VARIANTS"))
                    cube_variants();
                ctx = ctx2;             /* the on-screen demo below runs on the GPU */
            }
        }
        free(sw);
        free(sw2);
    }

    if (secs <= 0)
        return fails != 0;
    sleep(delay);
    surf = screen_surface();
    if (!surf)
        return 1;
    glGenFramebuffersOES(1, &fb);
    glBindFramebufferOES(GL_FRAMEBUFFER, fb);
    glGenRenderbuffersOES(2, rb);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[0]);
    if (!MSG(signed char (*)(id, SEL, GLenum, void *, signed char), ctx, "attachImage:toCoreSurface:invertedRender:"),
             GL_RENDERBUFFER, surf, 1)) {
        printf("gltest: attachImage:toCoreSurface: failed (0x%x)\n", glGetError());
        return 1;
    }
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    W = w;
    H = h;
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    printf("gltest: drawing into the display surface %dx%d, status 0x%x\n", W, H,
           glCheckFramebufferStatusOES(GL_FRAMEBUFFER));
    t0 = now();
    while ((t = now() - t0) < secs) {
        draw_scene((float)t);
        glFinish();
        if (!(frames++ % 20))
            printf("gltest: frame %u at %.1f s (%.1f fps)\n", frames, t, frames / (t > 0 ? t : 1));
        usleep(50000);
    }
    return 0;
}
#endif
