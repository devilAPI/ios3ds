/*
 * ios3ds-pica host tests. Build and run with `make test` in tools/ios3ds-pica.
 *
 * Expected words are derived by hand from the register descriptions in
 * docs/pica200.md (not by calling the code under test), so a mistake in an
 * encoder shows up as a mismatch here. The second half drives pica_hw.c
 * against a fake register block.
 *
 * With an argument, also writes <dir>/sample.cmd and <dir>/sample.tex for
 * tests/test_pica.py, which re-checks them with independent Python code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pica.h"
#include "pica_drv.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { checks++; unsigned long long a_ = (a), b_ = (b); if (a_ != b_) { failures++; \
    printf("FAIL %s:%d: %s = %#llx, expected %#llx\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* ---- a tiny command list reader for the checks below ---- */

typedef struct { uint32_t reg, mask, val; } wr_t;

/* Flatten a list into register writes. Returns the count. */
static unsigned decode(const uint32_t *w, unsigned nwords, wr_t *out, unsigned max)
{
    unsigned n = 0, i = 0;

    while (i + 1 < nwords) {
        uint32_t hdr = w[i + 1], reg = hdr & 0xFFFF, extra = (hdr >> 20) & 0xFF;
        if (n < max) out[n] = (wr_t){ reg, (hdr >> 16) & 0xF, w[i] };
        n++;
        for (unsigned k = 0; k < extra; k++) {
            if (hdr >> 31) reg++;
            if (n < max) out[n] = (wr_t){ reg, (hdr >> 16) & 0xF, w[i + 2 + k] };
            n++;
        }
        i += 2 + extra + (extra & 1);
    }
    return n;
}

/* Last value written to reg, or 0xDEADBEEF. */
static uint32_t last(const wr_t *w, unsigned n, uint32_t reg)
{
    uint32_t v = 0xDEADBEEF;
    for (unsigned i = 0; i < n; i++)
        if (w[i].reg == reg) v = w[i].val;
    return v;
}

static uint32_t buf[4096];
static wr_t wr[4096];

static unsigned run(pica_cmd_t *c)
{
    CHECK(!c->overflow);
    return decode(c->buf, c->len, wr, 4096);
}

/* ---- encoder ---- */

static void test_cmd(void)
{
    pica_cmd_t c;
    uint32_t v[300];

    for (unsigned i = 0; i < 300; i++) v[i] = 0x1000 + i;

    pica_cmd_init(&c, buf, 4096);
    pica_cmd_write(&c, 0x041, 0xAABBCCDD);
    CHECK_EQ(c.len, 2);
    CHECK_EQ(buf[0], 0xAABBCCDD);
    CHECK_EQ(buf[1], 0x000F0041);

    pica_cmd_write_masked(&c, 0x229, 0x2, 0x100);
    CHECK_EQ(buf[3], 0x00020229);

    /* 3 consecutive registers: 2 extra parameters, no padding needed */
    pica_cmd_write_seq(&c, 0x11C, v, 3);
    CHECK_EQ(c.len, 8);
    CHECK_EQ(buf[4], 0x1000);
    CHECK_EQ(buf[5], 0x802F011C);
    CHECK_EQ(buf[6], 0x1001);
    CHECK_EQ(buf[7], 0x1002);

    /* 2 parameters to one register: 1 extra, then a pad word */
    pica_cmd_write_burst(&c, 0x2CC, v, 2);
    CHECK_EQ(c.len, 12);
    CHECK_EQ(buf[9], 0x001F02CC);
    CHECK_EQ(buf[10], 0x1001);
    CHECK_EQ(buf[11], 0);

    /* more than 256 parameters split into groups; sequential groups advance the register */
    pica_cmd_init(&c, buf, 4096);
    pica_cmd_write_seq(&c, 0x100, v, 300);
    CHECK_EQ(buf[1], 0x8FFF0100);                   /* 255 extra */
    CHECK_EQ(c.len, 258 + 46);
    CHECK_EQ(buf[258], 0x1000 + 256);
    CHECK_EQ(buf[259], 0x80000000u | 0x000F0000 | 43u << 20 | 0x200);
    CHECK_EQ(run(&c), 300);
    CHECK_EQ(wr[299].reg, 0x100 + 299);
    CHECK_EQ(wr[299].val, 0x1000 + 299);

    pica_cmd_init(&c, buf, 4096);
    pica_cmd_write_burst(&c, 0x2C1, v, 300);
    CHECK_EQ(buf[259], 0x000F0000 | 43u << 20 | 0x2C1);

    /* finish: end marker, total a multiple of 16 bytes */
    pica_cmd_init(&c, buf, 4096);
    pica_cmd_write(&c, 0x041, 1);
    CHECK_EQ(pica_cmd_finish(&c), 16);
    CHECK_EQ(buf[2], 0x12345678);
    CHECK_EQ(buf[3], 0x000F0010);
    pica_cmd_init(&c, buf, 4096);
    CHECK_EQ(pica_cmd_finish(&c), 16);              /* one marker is 8 bytes: a second one pads */
    CHECK_EQ(buf[3], 0x000F0010);

    /* overflow is sticky and never writes past the end */
    buf[4] = 0x55555555;
    pica_cmd_init(&c, buf, 4);
    pica_cmd_write(&c, 1, 1);
    pica_cmd_write_seq(&c, 2, v, 3);
    CHECK(c.overflow);
    CHECK_EQ(c.len, 2);
    pica_cmd_write(&c, 1, 1);
    CHECK_EQ(c.len, 2);
    CHECK_EQ(buf[4], 0x55555555);
    CHECK_EQ(pica_cmd_finish(&c), 0);
}

static void test_floats(void)
{
    CHECK_EQ(pica_f24(0.0f), 0);
    CHECK_EQ(pica_f24(1.0f), 0x3F0000);
    CHECK_EQ(pica_f24(-1.0f), 0xBF0000);
    CHECK_EQ(pica_f24(0.5f), 0x3E0000);
    CHECK_EQ(pica_f24(160.0f), 0x464000);           /* 1.25 * 2^7 */
    CHECK_EQ(pica_f24(1e-30f), 0);                  /* underflow */
    CHECK_EQ(pica_f24(1e30f), 0x7F0000);            /* overflow -> infinity */
    CHECK_EQ(pica_f31(1.0f), 0x1F800000);
    CHECK_EQ(pica_f31(2.0f / 320.0f), 0x1BCCCCCD);  /* 1.6 * 2^-8, mantissa as in the IEEE float */
    CHECK_EQ(pica_f16(1.0f), 0x3C00);
    CHECK_EQ(pica_f16(-2.0f), 0xC000);
}

/* ---- state ---- */

static void test_state(void)
{
    pica_cmd_t c;
    unsigned n;

    /* 320x480 RGBA8 + D24S8 */
    pica_fb_t fb = { .color_pa = 0x18100000 + 0x2000, .depth_pa = 0x18200000,
                     .width = 320, .height = 480, .color_fmt = PICA_FB_RGBA8, .depth_fmt = PICA_DEPTH24S8 };
    pica_cmd_init(&c, buf, 4096);
    pica_emit_framebuffer(&c, &fb);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_DEPTHBUFFER_LOC), 0x18200000 >> 3);
    CHECK_EQ(last(wr, n, P3D_COLORBUFFER_LOC), 0x18102000 >> 3);
    CHECK_EQ(last(wr, n, P3D_FRAMEBUFFER_DIM), 0x01000000 | 479 << 12 | 320);
    CHECK_EQ(last(wr, n, P3D_RENDERBUF_DIM), 0x01000000 | 479 << 12 | 320);
    CHECK_EQ(last(wr, n, P3D_COLORBUFFER_FORMAT), 2);           /* 4 bytes -> size code 2, format 0 */
    CHECK_EQ(last(wr, n, P3D_DEPTHBUFFER_FORMAT), 3);
    CHECK_EQ(last(wr, n, P3D_COLORBUFFER_READ), 0xF);
    CHECK_EQ(last(wr, n, P3D_COLORBUFFER_WRITE), 0xF);
    CHECK_EQ(last(wr, n, P3D_DEPTHBUFFER_READ), 3);
    CHECK_EQ(last(wr, n, P3D_DEPTHBUFFER_WRITE), 3);
    CHECK_EQ(wr[0].reg, P3D_FRAMEBUFFER_INVALIDATE);

    fb.color_fmt = PICA_CB_RGB565; fb.depth_pa = 0;
    pica_cmd_init(&c, buf, 4096);
    pica_emit_framebuffer(&c, &fb);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_COLORBUFFER_FORMAT), 3 << 16);     /* 2 bytes -> size code 0 */
    CHECK_EQ(last(wr, n, P3D_DEPTHBUFFER_WRITE), 0);
    CHECK_EQ(pica_fb_color_bpp(PICA_FB_RGB8), 3);
    CHECK_EQ(pica_fb_depth_bpp(PICA_DEPTH24S8), 4);
    CHECK_EQ(pica_fb_depth_bpp(PICA_DEPTH16), 2);

    pica_cmd_init(&c, buf, 4096);
    pica_emit_viewport(&c, 0, 0, 320, 480);
    pica_emit_scissor(&c, 1, 10, 20, 100, 50);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_VIEWPORT_WIDTH), 0x464000);        /* f24(160) */
    CHECK_EQ(last(wr, n, P3D_VIEWPORT_INVW), 0x1BCCCCCD << 1);
    CHECK_EQ(last(wr, n, P3D_VIEWPORT_HEIGHT), 0x46E000);       /* f24(240) = 1.875 * 2^7 */
    CHECK_EQ(last(wr, n, P3D_VIEWPORT_XY), 0);
    CHECK_EQ(last(wr, n, P3D_SCISSORTEST_MODE), 3);
    CHECK_EQ(last(wr, n, P3D_SCISSORTEST_POS), 20 << 16 | 10);
    CHECK_EQ(last(wr, n, P3D_SCISSORTEST_DIM), 69 << 16 | 109);
    pica_cmd_init(&c, buf, 4096);
    pica_emit_scissor(&c, 0, 1, 2, 3, 4);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_SCISSORTEST_MODE), 0);

    /* GL defaults */
    pica_raster_t r;
    pica_raster_defaults(&r);
    pica_cmd_init(&c, buf, 4096);
    pica_emit_raster(&c, &r);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_ENABLE), 1);
    CHECK_EQ(last(wr, n, P3D_FACECULLING_CONFIG), 0);
    /* reversed depth (citro3d's convention, hardware run 15): stored = K (1 - d) */
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_SCALE), 0xBEFF80);        /* -K (far - near) = -(1 - 2^-10) */
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_OFFSET), 0);              /* K (1 - far) */
    CHECK_EQ(last(wr, n, P3D_FRAGOP_ALPHA_TEST), 1 << 4);       /* off, ALWAYS */
    CHECK_EQ(last(wr, n, P3D_STENCIL_TEST), 1u << 4 | 0xFFu << 8 | 0xFFu << 24);
    CHECK_EQ(last(wr, n, P3D_STENCIL_OP), 0);
    /* depth test off: LESS kept (as GREATER: reversed), colour writes on, depth write forced off */
    CHECK_EQ(last(wr, n, P3D_DEPTH_COLOR_MASK), 6 << 4 | 0xF << 8);
    CHECK_EQ(last(wr, n, P3D_BLEND_FUNC), 1 << 16 | 1 << 24);   /* ONE, ZERO */
    CHECK_EQ(last(wr, n, P3D_COLOR_OPERATION), 0x00E40100);

    /* the usual UI state: premultiplied-alpha blending, depth test LEQUAL */
    r.blend = 1;
    r.blend_src_rgb = r.blend_src_a = PICA_BF_ONE;
    r.blend_dst_rgb = r.blend_dst_a = PICA_BF_INV_SRC_ALPHA;
    r.depth_test = 1; r.depth_func = PICA_LEQUAL;
    r.alpha_test = 1; r.alpha_func = PICA_GREATER; r.alpha_ref = 0x80;
    r.cull = PICA_CULL_BACK_CCW;
    r.depth_near = 0.25f; r.depth_far = 0.75f;
    pica_cmd_init(&c, buf, 4096);
    pica_emit_raster(&c, &r);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_BLEND_FUNC), 1 << 16 | 7 << 20 | 1 << 24 | 7u << 28);
    CHECK_EQ(last(wr, n, P3D_DEPTH_COLOR_MASK), 1 | 7 << 4 | 0x1F << 8);      /* LEQUAL -> GEQUAL */
    CHECK_EQ(last(wr, n, P3D_FRAGOP_ALPHA_TEST), 1 | 6 << 4 | 0x80 << 8);
    CHECK_EQ(last(wr, n, P3D_FACECULLING_CONFIG), 2);
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_SCALE), 0xBDFF80);        /* -K 0.5 */
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_OFFSET), 0x3CFF80);       /* K 0.25 */
    CHECK_EQ(pica_depth_stored(PICA_Z_REVERSED, 1.0f), 0);
    CHECK_EQ(pica_depth_stored(PICA_Z_REVERSED, 0.0f), 0xFFC000);  /* K (2^24 - 1), float rounding */
    /* run 15's convention: stored = d, functions as given */
    r.depth_conv = PICA_Z_LEGACY;
    pica_cmd_init(&c, buf, 4096);
    pica_emit_raster(&c, &r);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_DEPTH_COLOR_MASK), 1 | 5 << 4 | 0x1F << 8);
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_SCALE), 0x3E0000);        /* 0.5 */
    CHECK_EQ(last(wr, n, P3D_DEPTHMAP_OFFSET), 0x3E8000);       /* 0.75 */
    r.depth_conv = PICA_Z_REVERSED;
    r.logic_op_enable = 1; r.logic_op = 11;
    pica_cmd_init(&c, buf, 4096);
    pica_emit_raster(&c, &r);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_COLOR_OPERATION), 0x00E40000);
    CHECK_EQ(last(wr, n, P3D_LOGIC_OP), 11);

    /* texture on unit 1 */
    pica_tex_t t = { .pa = 0x18180000, .width = 256, .height = 64, .fmt = PICA_TEX_RGBA4,
                     .mag = PICA_LINEAR, .min = PICA_NEAREST, .wrap_s = PICA_REPEAT,
                     .wrap_t = PICA_CLAMP_TO_EDGE, .border = 0x11223344 };
    pica_cmd_init(&c, buf, 4096);
    pica_emit_texture(&c, 1, &t);
    pica_emit_texunits(&c, 3);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT1_BORDER_COLOR), 0x11223344);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT1_BORDER_COLOR + 1), 256 << 16 | 64);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT1_BORDER_COLOR + 2), 1 << 1 | 2 << 12);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT1_BORDER_COLOR + 4), 0x18180000 >> 3);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT1_TYPE), 4);
    CHECK_EQ(last(wr, n, P3D_TEXUNIT_CONFIG), 1 << 16);         /* cache clear comes last */

    /* position float3 + colour ubyte4 + texcoord float2, one 24-byte vertex */
    pica_vtx_layout_t l = { .count = 3, .nbuffers = 1, .base_pa = 0x18100000 };
    l.attr[0].type = PICA_FLOAT; l.attr[0].size = 3;
    l.attr[1].type = PICA_UBYTE; l.attr[1].size = 4;
    l.attr[2].type = PICA_FLOAT; l.attr[2].size = 2;
    l.buffer[0].offset = 0x4000; l.buffer[0].stride = 24;
    pica_cmd_init(&c, buf, 4096);
    CHECK_EQ(pica_emit_vtx_layout(&c, &l), 0);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFERS_FORMAT_LOW), 0x7DB);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFERS_FORMAT_HIGH), 2u << 28);
    CHECK_EQ(last(wr, n, P3D_VSH_INPUTBUFFER_CONFIG), 0xA0000002);
    CHECK_EQ(last(wr, n, P3D_VSH_NUM_ATTR), 2);
    CHECK_EQ(last(wr, n, P3D_VSH_ATTR_PERM_LOW), 0x210);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFERS_LOC), 0x18100000 >> 3);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET), 0x4000);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 1), 0x210);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 2), 24 << 16 | 3u << 28);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 3), 0);     /* buffer 1 unused */

    /* constant colour + separate arrays; the position array has a 4-byte gap */
    l.attr[1].fixed = 1;
    l.attr[2].buffer = 1;
    l.nbuffers = 2;
    l.buffer[0].stride = 16;
    l.buffer[1].offset = 0x8000; l.buffer[1].stride = 8;
    pica_cmd_init(&c, buf, 4096);
    CHECK_EQ(pica_emit_vtx_layout(&c, &l), 0);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFERS_FORMAT_HIGH), 2u << 28 | 1 << 17);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 1), 0xC0);  /* attr 0, then 4 bytes of padding */
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 2), 16 << 16 | 2u << 28);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 3), 0x8000);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 4), 0x2);
    CHECK_EQ(last(wr, n, P3D_ATTRIBBUFFER0_OFFSET + 5), 8 << 16 | 1u << 28);
    l.buffer[0].stride = 14;                                    /* gap not a multiple of 4 */
    pica_cmd_init(&c, buf, 4096);
    CHECK_EQ(pica_emit_vtx_layout(&c, &l), -1);
    l.buffer[0].stride = 8;                                     /* smaller than the data */
    CHECK_EQ(pica_emit_vtx_layout(&c, &l), -1);
    CHECK_EQ(c.len, 0);

    /* fixed attribute: (1, 0.5, 0, 1) -> f24 0x3F0000, 0x3E0000, 0, 0x3F0000 */
    const float col[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
    pica_cmd_init(&c, buf, 4096);
    pica_emit_fixed_attr(&c, 1, col);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_FIXEDATTRIB_INDEX), 1);
    CHECK_EQ(last(wr, n, P3D_FIXEDATTRIB_DATA0), 0x3F000000);       /* w << 8 | z >> 16 */
    CHECK_EQ(last(wr, n, P3D_FIXEDATTRIB_DATA0 + 1), 0x00003E00);   /* z << 16 | y >> 8 */
    CHECK_EQ(last(wr, n, P3D_FIXEDATTRIB_DATA0 + 2), 0x003F0000);   /* y << 24 | x */

    /* draws: the trigger must be bracketed by leaving and re-entering configuration mode */
    pica_cmd_init(&c, buf, 4096);
    pica_emit_draw_arrays(&c, PICA_TRIANGLE_STRIP, 4, 6);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_NUMVERTICES), 6);
    CHECK_EQ(last(wr, n, P3D_VERTEX_OFFSET), 4);
    unsigned leave = 0, trig = 0, enter = 0;
    for (unsigned i = 0; i < n; i++) {
        if (wr[i].reg == P3D_START_DRAW_FUNC0 && wr[i].val == 0) leave = i;
        if (wr[i].reg == P3D_DRAWARRAYS) trig = i;
        if (wr[i].reg == P3D_START_DRAW_FUNC0 && wr[i].val == 1) enter = i;
        if (wr[i].reg == P3D_PRIMITIVE_CONFIG) { CHECK_EQ(wr[i].mask, 2); CHECK_EQ(wr[i].val, 0x100); }
    }
    CHECK(leave && leave < trig && trig < enter);

    pica_cmd_init(&c, buf, 4096);
    pica_emit_draw_elements(&c, PICA_TRIANGLES, 36, 0x9000, 1);
    n = run(&c);
    CHECK_EQ(last(wr, n, P3D_INDEXBUFFER_CONFIG), 0x80009000);
    CHECK_EQ(last(wr, n, P3D_NUMVERTICES), 36);
    CHECK_EQ(wr[0].val, 0x300);                                 /* lists use the geometry-primitive path */

    /* uniforms: c4 = (1, 2, 3, 4) is sent w first as IEEE floats */
    const float u[1][4] = { { 1.0f, 2.0f, 3.0f, 4.0f } };
    pica_cmd_init(&c, buf, 4096);
    pica_emit_uniforms(&c, 4, u, 1);
    n = run(&c);
    CHECK_EQ(n, 5);
    CHECK_EQ(wr[0].reg, P3D_VSH_FLOATUNIFORM_CONFIG);
    CHECK_EQ(wr[0].val, 0x80000004);
    CHECK_EQ(wr[1].val, 0x40800000);
    CHECK_EQ(wr[2].val, 0x40400000);
    CHECK_EQ(wr[3].val, 0x40000000);
    CHECK_EQ(wr[4].val, 0x3F800000);
    CHECK_EQ(wr[4].reg, P3D_VSH_FLOATUNIFORM_DATA);
}

/* ---- textures ---- */

static void test_tex(void)
{
    /* tile order: x in the even bits, y in the odd bits, tiles row-major */
    CHECK_EQ(pica_tex_index(0, 0, 64), 0);
    CHECK_EQ(pica_tex_index(1, 0, 64), 1);
    CHECK_EQ(pica_tex_index(0, 1, 64), 2);
    CHECK_EQ(pica_tex_index(1, 1, 64), 3);
    CHECK_EQ(pica_tex_index(2, 0, 64), 4);
    CHECK_EQ(pica_tex_index(0, 2, 64), 8);
    CHECK_EQ(pica_tex_index(7, 7, 64), 63);
    CHECK_EQ(pica_tex_index(8, 0, 64), 64);
    CHECK_EQ(pica_tex_index(0, 8, 64), 8 * 64);
    CHECK_EQ(pica_tex_index(63, 31, 64), 64 * 32 - 1);
    static uint8_t seen[64 * 32];
    memset(seen, 0, sizeof seen);
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 64; x++)
            seen[pica_tex_index(x, y, 64)]++;
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof seen; i++) bad += seen[i] != 1;
    CHECK_EQ(bad, 0);

    CHECK_EQ(pica_pot(1), 8);
    CHECK_EQ(pica_pot(320), 512);
    CHECK_EQ(pica_pot(512), 512);
    CHECK_EQ(pica_tex_bytes(PICA_TEX_RGBA8, 512, 512), 1 << 20);
    CHECK_EQ(pica_tex_bytes(PICA_TEX_L4, 8, 8), 32);
    CHECK_EQ(pica_tex_bpp(99), 0);

    /* one RGBA pixel at GL (0,0) of an 8x8 texture lands in stored row 7:
       texel index 42 (y = 7 -> bits 1,3,5), bytes A,B,G,R */
    static uint8_t tex[8 * 8 * 4];
    const uint8_t px[4] = { 0x11, 0x22, 0x33, 0x44 };          /* R G B A */
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA8, 8, 8, 0, 0, px, PICA_SRC_RGBA8888, 1, 1, 4), 0);
    CHECK_EQ(tex[42 * 4 + 0], 0x44);
    CHECK_EQ(tex[42 * 4 + 1], 0x33);
    CHECK_EQ(tex[42 * 4 + 2], 0x22);
    CHECK_EQ(tex[42 * 4 + 3], 0x11);
    unsigned nz = 0;
    for (unsigned i = 0; i < sizeof tex; i++) nz += tex[i] != 0;
    CHECK_EQ(nz, 4);

    /* the same colour arriving as BGRA gives the same texel; (1, 7) is stored at row 0, x = 1 */
    const uint8_t bgra[4] = { 0x33, 0x22, 0x11, 0x44 };
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA8, 8, 8, 1, 7, bgra, PICA_SRC_BGRA8888, 1, 1, 4), 0);
    CHECK_EQ(tex[1 * 4 + 0], 0x44);
    CHECK_EQ(tex[1 * 4 + 3], 0x11);

    /* 16-bit formats: R in the top bits, little-endian */
    const uint16_t p565 = 0xF800 | 0x07E0 >> 1 << 1;           /* red + green */
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGB565, 8, 8, 0, 7, &p565, PICA_SRC_RGB565, 1, 1, 2), 0);
    CHECK_EQ(tex[0] | tex[1] << 8, 0xFFE0);
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA4, 8, 8, 0, 7, px, PICA_SRC_RGBA8888, 1, 1, 4), 0);
    CHECK_EQ(tex[0] | tex[1] << 8, 0x1234);
    const uint8_t opaque_red[4] = { 0xFF, 0, 0, 0xFF };
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA5551, 8, 8, 0, 7, opaque_red, PICA_SRC_RGBA8888, 1, 1, 4), 0);
    CHECK_EQ(tex[0] | tex[1] << 8, 0xF801);
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGB8, 8, 8, 0, 7, px, PICA_SRC_RGBA8888, 1, 1, 4), 0);
    CHECK_EQ(tex[0], 0x33); CHECK_EQ(tex[1], 0x22); CHECK_EQ(tex[2], 0x11);

    /* luminance / alpha */
    const uint8_t la[2] = { 0x90, 0x40 };                      /* L, A */
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_LA8, 8, 8, 0, 7, la, PICA_SRC_LA88, 1, 1, 2), 0);
    CHECK_EQ(tex[0], 0x40); CHECK_EQ(tex[1], 0x90);            /* stored A, L */
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_LA4, 8, 8, 0, 7, la, PICA_SRC_LA88, 1, 1, 2), 0);
    CHECK_EQ(tex[0], 0x94);
    memset(tex, 0, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_A8, 8, 8, 0, 7, la + 1, PICA_SRC_A8, 1, 1, 1), 0);
    CHECK_EQ(tex[0], 0x40);
    /* 4-bit: texels 0 and 1 share a byte, texel 0 in the low nibble */
    const uint8_t l2[2] = { 0xA0, 0x50 };
    memset(tex, 0xFF, sizeof tex);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_L4, 8, 8, 0, 7, l2, PICA_SRC_L8, 2, 1, 2), 0);
    CHECK_EQ(tex[0], 0x5A);
    CHECK_EQ(tex[1], 0xFF);

    /* rejected: NPOT texture, rectangle outside, compressed target */
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA8, 12, 8, 0, 0, px, PICA_SRC_RGBA8888, 1, 1, 4), -1);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_RGBA8, 8, 8, 8, 0, px, PICA_SRC_RGBA8888, 1, 1, 4), -1);
    CHECK_EQ(pica_tex_upload(tex, PICA_TEX_ETC1, 8, 8, 0, 0, px, PICA_SRC_RGBA8888, 1, 1, 4), -1);

    /* upload a 20x12 BGRA image (row padding in the source) into 32x16 and read
       it back through the colour-buffer untiler: must round-trip exactly */
    enum { W = 20, H = 12, STRIDE = W * 4 + 8 };
    static uint8_t img[H * STRIDE], big[32 * 16 * 4], back[H * W * 4];
    for (unsigned i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 7 + 3);
    memset(big, 0, sizeof big);
    CHECK_EQ(pica_tex_upload(big, PICA_TEX_RGBA8, 32, 16, 0, 0, img, PICA_SRC_BGRA8888, W, H, STRIDE), 0);
    /* GL rows are bottom-up, the read-back is top-down: compare flipped */
    pica_untile_rgba8_to_bgra(back, W * 4, big, 32, 16, 0, 0, W, H);
    unsigned diff = 0;
    for (unsigned y = 0; y < H; y++)
        diff += memcmp(back + y * W * 4, img + (H - 1 - y) * STRIDE, W * 4) != 0;
    CHECK_EQ(diff, 0);

    /* edge padding copies the last column / row one texel outwards */
    pica_tex_pad_edges(big, PICA_TEX_RGBA8, 32, 16, W, H);
    CHECK(memcmp(big + pica_tex_index(W, 15 - 3, 32) * 4, big + pica_tex_index(W - 1, 15 - 3, 32) * 4, 4) == 0);
    CHECK(memcmp(big + pica_tex_index(5, 15 - H, 32) * 4, big + pica_tex_index(5, 16 - H, 32) * 4, 4) == 0);
    CHECK(memcmp(big + pica_tex_index(W, 15 - H, 32) * 4, big + pica_tex_index(W - 1, 16 - H, 32) * 4, 4) == 0);

    CHECK_EQ(pica_tex_fmt_for(PICA_SRC_BGRA8888), PICA_TEX_RGBA8);
    CHECK_EQ(pica_tex_fmt_for(PICA_SRC_A8), PICA_TEX_A8);
}

/* ---- fixed function ---- */

/* Evaluate TEV stages for one fragment (8-bit channels as 0..1 floats). */
static void tev_eval(const pica_tev_t *st, int n, const float prim[4], const float tex0[4],
                     const float tex1[4], float out[4])
{
    float prev[4] = { 0, 0, 0, 0 };

    for (int s = 0; s < n; s++) {
        const pica_tev_t *t = &st[s];
        float konst[4], arg[3][4], res[4];

        for (int i = 0; i < 4; i++) konst[i] = (float)((t->color >> (8 * i)) & 0xFF) / 255.0f;
        for (int a = 0; a < 3; a++) {
            const float *rgb_src, *a_src;
            const float *pick[2];
            unsigned ids[2] = { t->src_rgb[a], t->src_a[a] };
            for (int k = 0; k < 2; k++)
                pick[k] = ids[k] == PICA_TEVSRC_PRIMARY ? prim : ids[k] == PICA_TEVSRC_TEXTURE0 ? tex0 :
                          ids[k] == PICA_TEVSRC_TEXTURE1 ? tex1 : ids[k] == PICA_TEVSRC_CONSTANT ? konst : prev;
            rgb_src = pick[0]; a_src = pick[1];
            for (int ch = 0; ch < 3; ch++) {
                unsigned op = t->op_rgb[a];
                float v = op < 2 ? rgb_src[ch] : op < 4 ? rgb_src[3] : rgb_src[(op >> 2) - 1];
                arg[a][ch] = (op & 1) ? 1.0f - v : v;
            }
            unsigned op = t->op_a[a];
            float v = op < 2 ? a_src[3] : a_src[(op >> 1) - 1];
            arg[a][3] = (op & 1) ? 1.0f - v : v;
        }
        for (int ch = 0; ch < 4; ch++) {
            unsigned f = ch < 3 ? t->func_rgb : t->func_a;
            float a = arg[0][ch], b = arg[1][ch], c = arg[2][ch], v;
            switch (f) {
            case PICA_TEV_REPLACE: v = a; break;
            case PICA_TEV_MODULATE: v = a * b; break;
            case PICA_TEV_ADD: v = a + b; break;
            case PICA_TEV_INTERPOLATE: v = a * c + b * (1.0f - c); break;
            case PICA_TEV_MULT_ADD: v = a * b + c; break;
            default: v = -1000.0f; break;
            }
            res[ch] = v > 1.0f ? 1.0f : v;
        }
        memcpy(prev, res, sizeof res);
    }
    memcpy(out, prev, sizeof prev);
}

static int near(float a, float b) { float d = a - b; return d < 1e-5f && d > -1e-5f; }
#define CHECK_VEC(v, r, g, b, a) CHECK(near((v)[0], r) && near((v)[1], g) && near((v)[2], b) && near((v)[3], a))

static void test_ff(void)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float m[16];

    /* identity: z' = (z - w) / 2, so GL near (z = -w) -> -w and far (z = w) -> 0 */
    pica_ff_clip_matrix(m, ident, ident);
    CHECK(near(m[10], 0.5f) && near(m[14], -0.5f) && near(m[0], 1.0f) && near(m[15], 1.0f));
    CHECK(near(m[2], 0.0f) && near(m[11], 0.0f));

    /* glOrthof(0, 320, 0, 480, -1, 1) times a translation: x and y behave as in GL */
    float ortho[16] = { 2.0f / 320, 0, 0, 0, 0, 2.0f / 480, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
    float trans[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 160, 240, 0, 1 };
    pica_ff_clip_matrix(m, ortho, trans);
    CHECK(near(m[12], 0.0f) && near(m[13], 0.0f));              /* (0,0,0) -> centre */
    CHECK(near(m[14], -0.5f));                                  /* mid-depth */

    /* NPOT content 320x480 in a 512x512 texture */
    pica_ff_tex_matrix(m, NULL, 320, 480, 512, 512);
    CHECK(near(m[0], 0.625f) && near(m[5], 0.9375f) && near(m[10], 1.0f));

    /* uniforms: c0..c3 = rows of the clip matrix, c8 = colour scale */
    pica_cmd_t c;
    float clip[16];
    for (int i = 0; i < 16; i++) clip[i] = (float)(i + 1);     /* column-major: row 0 = 1, 5, 9, 13 */
    pica_cmd_init(&c, buf, 4096);
    pica_ff_emit_matrices(&c, clip, NULL, NULL, 1.0f / 255.0f);
    unsigned n = run(&c);
    CHECK_EQ(n, 1 + 9 * 4);
    CHECK_EQ(wr[0].val, 0x80000000);
    float f;
    memcpy(&f, &wr[1].val, 4); CHECK(near(f, 13.0f));           /* c0.w */
    memcpy(&f, &wr[4].val, 4); CHECK(near(f, 1.0f));            /* c0.x */
    memcpy(&f, &wr[1 + 4 * 4 + 3].val, 4); CHECK(near(f, 1.0f));/* c4.x: identity texture matrix */
    memcpy(&f, &wr[1 + 5 * 4 + 2].val, 4); CHECK(near(f, 1.0f));/* c5.y */
    memcpy(&f, &wr[1 + 8 * 4].val, 4); CHECK(near(f, 1.0f / 255.0f));

    /* program upload carries the assembled words and the output map */
    pica_cmd_init(&c, buf, 4096);
    pica_ff_emit_program(&c);
    n = run(&c);
    unsigned ncode = 0, ndesc = 0;
    for (unsigned i = 0; i < n; i++) {
        if (wr[i].reg == P3D_VSH_CODETRANSFER_DATA) { CHECK_EQ(wr[i].val, pica_ff_shader.code[ncode]); ncode++; }
        if (wr[i].reg == P3D_VSH_OPDESCS_DATA) { CHECK_EQ(wr[i].val, pica_ff_shader.opdesc[ndesc]); ndesc++; }
    }
    CHECK_EQ(ncode, pica_ff_shader.ncode);
    CHECK_EQ(ndesc, pica_ff_shader.nopdesc);
    CHECK_EQ(last(wr, n, P3D_VSH_ENTRYPOINT), 0x7FFF0000);
    CHECK_EQ(last(wr, n, P3D_SH_OUTMAP_TOTAL), 4);
    CHECK_EQ(last(wr, n, P3D_SH_OUTMAP_O0), 0x03020100);
    CHECK_EQ(last(wr, n, P3D_SH_OUTMAP_O0 + 2), 0x1F1F0D0C);
    CHECK_EQ(last(wr, n, P3D_SH_OUTMAP_O0 + 4), 0x1F1F1F1F);
    CHECK_EQ(last(wr, n, P3D_VSH_OUTMAP_MASK), 0xF);
    CHECK_EQ(last(wr, n, P3D_VSH_OUTMAP_TOTAL1), 3);
    CHECK_EQ(last(wr, n, P3D_SH_OUTATTR_CLOCK), 0x303);
    CHECK_EQ(last(wr, n, P3D_GSH_INPUTBUFFER_CONFIG), 0xA0000000);

    /* glTexEnv, checked by evaluating the stages on one fragment */
    const float prim[4] = { 0.5f, 0.25f, 1.0f, 0.5f };
    const float rgba[4] = { 0.8f, 0.4f, 0.2f, 0.5f };           /* an RGBA sample */
    const float rgb1[4] = { 0.8f, 0.4f, 0.2f, 1.0f };           /* RGB texture: alpha reads 1 */
    const float a_only[4] = { 0.0f, 0.0f, 0.0f, 0.5f };         /* ALPHA texture: colour reads 0 */
    pica_tev_t st[PICA_TEV_STAGES];
    float out[4];
    pica_gl_texenv_t e = { .mode = 0x2100 /* GL_MODULATE */, .base_format = 0x1908 /* GL_RGBA */ };
    const pica_gl_texenv_t *one[2] = { &e, NULL };

    CHECK_EQ(pica_ff_texenv(st, one), 1);
    tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 0.4f, 0.1f, 0.2f, 0.25f);
    e.mode = 0x1E01;                                            /* GL_REPLACE */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 0.8f, 0.4f, 0.2f, 0.5f);
    e.base_format = 0x1907;                                     /* GL_RGB: alpha stays the fragment's */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgb1, NULL, out);
    CHECK_VEC(out, 0.8f, 0.4f, 0.2f, 0.5f);
    e.mode = 0x2100;
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgb1, NULL, out);
    CHECK_VEC(out, 0.4f, 0.1f, 0.2f, 0.5f);
    e.base_format = 0x1906;                                     /* GL_ALPHA (glyphs): colour stays, alpha multiplies */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, a_only, NULL, out);
    CHECK_VEC(out, 0.5f, 0.25f, 1.0f, 0.25f);
    e.mode = 0x1E01;
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, a_only, NULL, out);
    CHECK_VEC(out, 0.5f, 0.25f, 1.0f, 0.5f);
    e.base_format = 0x1908; e.mode = 0x2101;                    /* GL_DECAL: Cp(1-As) + Cs As, alpha Ap */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 0.65f, 0.325f, 0.6f, 0.5f);
    e.mode = 0x0BE2; e.env_color = 0xFF | 0x00 << 8 | 0xFF << 16;  /* GL_BLEND with Cc = magenta */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 0.5f * 0.2f + 0.8f, 0.25f * 0.6f, 1.0f, 0.25f);
    e.mode = 0x0104;                                            /* GL_ADD (clamped) */
    pica_ff_texenv(st, one); tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 1.0f, 0.65f, 1.0f, 0.25f);
    e.mode = 0x8570;                                            /* GL_COMBINE: not handled here */
    CHECK_EQ(pica_ff_texenv(st, one), -1);

    /* two units: unit 1 modulates the result of unit 0 */
    pica_gl_texenv_t e0 = { .mode = 0x1E01, .base_format = 0x1908 }, e1 = { .mode = 0x2100, .base_format = 0x1907 };
    const pica_gl_texenv_t *two[2] = { &e0, &e1 };
    const float t1[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    CHECK_EQ(pica_ff_texenv(st, two), 2);
    tev_eval(st, PICA_TEV_STAGES, prim, rgba, t1, out);
    CHECK_VEC(out, 0.4f, 0.2f, 0.1f, 0.5f);
    /* only unit 1 enabled: it becomes the first stage and takes the vertex colour */
    const pica_gl_texenv_t *second[2] = { NULL, &e1 };
    CHECK_EQ(pica_ff_texenv(st, second), 1);
    tev_eval(st, PICA_TEV_STAGES, prim, rgba, t1, out);
    CHECK_VEC(out, 0.25f, 0.125f, 0.5f, 0.5f);
    /* no texture: the vertex colour */
    const pica_gl_texenv_t *none[2] = { NULL, NULL };
    CHECK_EQ(pica_ff_texenv(st, none), 1);
    tev_eval(st, PICA_TEV_STAGES, prim, rgba, t1, out);
    CHECK_VEC(out, 0.5f, 0.25f, 1.0f, 0.5f);

    /* present swizzle: the buffer's (R, G, B, A) = sample's (A, R, G, B) */
    pica_ff_tev_bgra_swizzle(st);
    tev_eval(st, PICA_TEV_STAGES, prim, rgba, NULL, out);
    CHECK_VEC(out, 0.5f, 0.8f, 0.4f, 0.2f);

    /* TEV register packing: MODULATE(texture0, primary) on stage 4 */
    e.mode = 0x2100; e.base_format = 0x1908; e.env_color = 0x80402010;
    pica_ff_texenv(st, one);
    pica_cmd_init(&c, buf, 4096);
    pica_emit_tev(&c, 4, &st[0]);
    n = run(&c);
    CHECK_EQ(wr[0].reg, 0x0F0);
    CHECK_EQ(wr[0].val, 0x03 | 0x03 << 16);                     /* sources: tex0, primary(0), 0 */
    CHECK_EQ(wr[1].val, 0);
    CHECK_EQ(wr[2].val, 1 | 1 << 16);
    CHECK_EQ(wr[3].val, 0x80402010);
    CHECK_EQ(wr[4].val, 0);

    /* enum translation */
    CHECK_EQ(pica_gl_func(0x0201), PICA_LESS);
    CHECK_EQ(pica_gl_func(0x0203), PICA_LEQUAL);
    CHECK_EQ(pica_gl_func(0x0205), PICA_NOTEQUAL);
    CHECK_EQ(pica_gl_func(0x0207), PICA_ALWAYS);
    CHECK_EQ(pica_gl_func(0x0208), -1);
    CHECK_EQ(pica_gl_blend_factor(1), PICA_BF_ONE);
    CHECK_EQ(pica_gl_blend_factor(0x0302), PICA_BF_SRC_ALPHA);
    CHECK_EQ(pica_gl_blend_factor(0x0303), PICA_BF_INV_SRC_ALPHA);
    CHECK_EQ(pica_gl_blend_factor(0x0306), PICA_BF_DST_COLOR);
    CHECK_EQ(pica_gl_blend_factor(0x0308), PICA_BF_SRC_ALPHA_SAT);
    CHECK_EQ(pica_gl_blend_factor(0x0309), -1);
    CHECK_EQ(pica_gl_blend_eq(0x800B), PICA_BLEND_RSUB);
    CHECK_EQ(pica_gl_stencil_op(0x150A), PICA_STENCIL_INVERT);
    CHECK_EQ(pica_gl_stencil_op(0x1E02), PICA_STENCIL_INCR);
    CHECK_EQ(pica_gl_logic_op(0x1503), 3);                      /* COPY */
    CHECK_EQ(pica_gl_logic_op(0x1506), 11);                     /* XOR */
    CHECK_EQ(pica_gl_logic_op(0x150F), 4);                      /* SET */
    CHECK_EQ(pica_gl_wrap(0x812F), PICA_CLAMP_TO_EDGE);
    CHECK_EQ(pica_gl_wrap(0x2901), PICA_REPEAT);
    CHECK_EQ(pica_gl_prim(5), PICA_TRIANGLE_STRIP);
    CHECK_EQ(pica_gl_prim(1), -1);                              /* GL_LINES */
    CHECK_EQ(pica_gl_attr_type(0x1406), PICA_FLOAT);
    CHECK_EQ(pica_gl_attr_type(0x140C), -1);                    /* GL_FIXED */
}

/* ---- driver core + software model, end to end ---- */
#include "pica_dev.h"
#include "../sim/picasim.h"

/* Clear a 64x64 target, draw one red triangle covering the lower-left half
   (GL coordinates), transfer it to a linear buffer and look at the pixels:
   checks the fill, the command list, the shader, rasterization, the buffer
   orientation and the display transfer in one go. */
static void test_sim(void)
{
    const pica_dev_t *dev = pica_dev_host();
    const PicaSimStats *st = pica_dev_host_stats();
    pica_info_t info;
    uint32_t color, depth, lin, vbuf, bytes;
    static uint32_t list[2048], out[64 * 64];
    static const float vtx[3][12] = {       /* clip position, colour, texcoord0, texcoord1 */
        { -1, -1, 0, 1,  1, 0, 0, 1,  0, 0, 0, 0 },
        {  1, -1, 0, 1,  1, 0, 0, 1,  0, 0, 0, 0 },
        { -1,  1, 0, 1,  1, 0, 0, 1,  0, 0, 0, 0 },
    };
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float clip[16];
    pica_cmd_t c;
    pica_fb_t fb;
    pica_raster_t rs;
    pica_vtx_layout_t l;

    CHECK_EQ(dev->info(&info), PICA_OK);
    CHECK_EQ(info.state, PICA_OK);
    CHECK_EQ(info.version, PICA_PROTO_VERSION);
    CHECK_EQ(dev->dirty(), PICA_OK);        /* surface read-back -> mirror damage flag */
    CHECK_EQ(dev->alloc(64 * 64 * 4, 0, &color), PICA_OK);
    CHECK_EQ(dev->alloc(64 * 64 * 4, 0, &depth), PICA_OK);
    CHECK_EQ(dev->alloc(64 * 64 * 4, 0, &lin), PICA_OK);
    CHECK_EQ(dev->alloc(4096, 0, &vbuf), PICA_OK);
    CHECK(color != depth && depth != lin && !(color & 0x7F));
    CHECK_EQ(dev->alloc(0x7FFFFFFF, 0, &bytes), PICA_ERR_RANGE);
    CHECK_EQ(dev->write(info.arena_pa, vtx, 16), PICA_ERR_RANGE);       /* the driver's own block */
    CHECK_EQ(dev->write(info.arena_pa + info.arena_size - 8, vtx, 16), PICA_ERR_RANGE);

    /* The PSC engine cannot write FCRAM (hardware run 13 and picasim), so
       the driver now does PICA_OP_FILL on the CPU. A constant is the same
       tiled or linear, so the result is what a working fill would give. */
    {
        static uint32_t pat[64 * 64];
        for (unsigned i = 0; i < 64 * 64; i++)
            pat[i] = 0x12345678u;
        CHECK_EQ(dev->write(lin, pat, sizeof pat), PICA_OK);
        CHECK_EQ(dev->fill(lin, 64 * 64 * 4, 0x0000FFFF, PSC_FILL_32, 0, 0, 0, 0), PICA_OK);
        CHECK_EQ(dev->read(lin, out, sizeof out), PICA_OK);
        CHECK_EQ(out[0], 0x0000FFFFu);
        CHECK_EQ(out[64 * 64 - 1], 0x0000FFFFu);
        /* 16- and 24-bit widths fill repeated halfwords / byte triples */
        CHECK_EQ(dev->fill(lin, 64, 0xABCDABCDu, PSC_FILL_16, 0, 0, 0, 0), PICA_OK);
        CHECK_EQ(dev->read(lin, out, 64), PICA_OK);
        CHECK_EQ(out[0], 0xABCDABCDu);
        CHECK_EQ(out[15], 0xABCDABCDu);
        CHECK_EQ(dev->fill(lin, 64, 0xAABBCCDDu, PSC_FILL_24, 0, 0, 0, 0), PICA_OK);
        CHECK_EQ(dev->read(lin, out, 64), PICA_OK);
        CHECK_EQ(out[0], 0xDDBBCCDDu);       /* only the low 24 bits repeat */
        CHECK_EQ(out[15], 0xDDBBCCDDu);
        CHECK_EQ(dev->fill(lin, 64, 0, PSC_FILL_32, lin + 64, 64, 0x11223344, PSC_FILL_32), PICA_OK);
        CHECK_EQ(dev->read(lin, out, 128), PICA_OK);
        CHECK_EQ(out[0], 0u);
        CHECK_EQ(out[16], 0x11223344u);
        CHECK_EQ(dev->fill(info.arena_pa + 0x1000, 16, 0, PSC_FILL_32, 0, 0, 0, 0), PICA_ERR_RANGE);
        for (unsigned i = 0; i < 64 * 64; i++)
            pat[i] = 0x0000FFFFu;
        CHECK_EQ(dev->write(color, pat, sizeof pat), PICA_OK);
        for (unsigned i = 0; i < 64 * 64; i++)
            pat[i] = pica_depth_stored(PICA_Z_REVERSED, 1.0f);    /* far */
        CHECK_EQ(dev->write(depth, pat, sizeof pat), PICA_OK);
    }

    pica_cmd_init(&c, list, 2048);
    pica_emit_base_state(&c);
    pica_ff_emit_program(&c);
    pica_ff_clip_matrix(clip, ident, ident);
    pica_ff_emit_matrices(&c, clip, NULL, NULL, 1.0f);
    memset(&fb, 0, sizeof fb);
    fb.color_pa = color; fb.depth_pa = depth; fb.width = 64; fb.height = 64;
    fb.color_fmt = PICA_FB_RGBA8; fb.depth_fmt = PICA_DEPTH24S8;
    pica_emit_framebuffer(&c, &fb);
    pica_emit_viewport(&c, 0, 0, 64, 64);
    pica_emit_scissor(&c, 0, 0, 0, 0, 0);
    pica_raster_defaults(&rs);
    rs.depth_test = 1;
    pica_emit_raster(&c, &rs);
    memset(&l, 0, sizeof l);
    l.count = 4;
    l.attr[0].type = PICA_FLOAT; l.attr[0].size = 4;
    l.attr[1].type = PICA_FLOAT; l.attr[1].size = 4;
    l.attr[2].type = PICA_FLOAT; l.attr[2].size = 2;
    l.attr[3].type = PICA_FLOAT; l.attr[3].size = 2;
    l.nbuffers = 1;
    l.buffer[0].stride = 48;
    l.base_pa = vbuf;
    CHECK_EQ(pica_emit_vtx_layout(&c, &l), 0);
    pica_emit_draw_arrays(&c, PICA_TRIANGLES, 0, 3);
    pica_emit_fb_flush(&c);
    bytes = pica_cmd_finish(&c);
    CHECK(bytes && !(bytes & 15));
    CHECK_EQ(dev->submit(list, bytes, vbuf, vtx, sizeof vtx), PICA_OK);
    CHECK_EQ(st->draws, 1);
    CHECK_EQ(st->triangles, 1);
    CHECK_EQ(st->errors, 0);
    CHECK(st->pixels > 1900 && st->pixels < 2150);              /* half of 64x64 */

    /* tiled -> linear; without the flip flag the first linear row is the top GL row */
    CHECK_EQ(dev->transfer(color, 64u << 16 | 64, lin, 64u << 16 | 64,
                           PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8)), PICA_OK);
    CHECK_EQ(dev->read(lin, out, sizeof out), PICA_OK);
    CHECK_EQ(out[63 * 64 + 2], 0xFF0000FFu);                    /* GL (2, 0): red, word = R<<24|G<<16|B<<8|A */
    CHECK_EQ(out[(63 - 20) * 64 + 20], 0xFF0000FFu);            /* GL (20, 20) */
    CHECK_EQ(out[(63 - 60) * 64 + 60], 0x0000FFFFu);            /* GL (60, 60): untouched clear colour */
    CHECK_EQ(out[2 * 64 + 60], 0x0000FFFFu);
    CHECK_EQ(out[2 * 64 + 1], 0xFF0000FFu);                     /* GL (1, 61): just inside the diagonal */
    /* the same with the flip flag: first linear row = GL row 0 */
    CHECK_EQ(dev->transfer(color, 64u << 16 | 64, lin, 64u << 16 | 64,
                           PPF_FLIP_V | PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8)), PICA_OK);
    CHECK_EQ(dev->read(lin, out, sizeof out), PICA_OK);
    CHECK_EQ(out[20 * 64 + 20], 0xFF0000FFu);
    CHECK_EQ(out[60 * 64 + 60], 0x0000FFFFu);
    /* depth was written where the triangle is: z_clip 0 -> far plane value */
    CHECK_EQ(dev->free(color), PICA_OK);
    CHECK_EQ(dev->free(color), PICA_ERR_RANGE);
    CHECK_EQ(dev->free(depth), PICA_OK);
    CHECK_EQ(dev->free(lin), PICA_OK);
    CHECK_EQ(dev->free(vbuf), PICA_OK);
}

/* The arena allocator under CA's pattern (hardware runs 19-21, issues 1/5):
   a 1 MiB render target and its 600 KiB copy come and go while small layer
   textures are uploaded and evicted. Big blocks come from their own top
   pool, so with a 12/16 MiB arena 150 targets need no texture evictions;
   with one shared pool and random eviction it was ~42 MiB (re-log 75). */
static int t_alive(uint32_t o) { (void)o; return 1; }
static void t_log(const char *f, ...) { (void)f; }
static uint32_t alloc_churn(uint32_t big_min, uint32_t *evicted_kb)
{
    static pica_plat_t plat;
    static pica_drv_t d;
    static uint32_t live[400], livesz[400];
    uint32_t n = 0, held = 0, i, k, pa, rt, swz, fails = 0, rnd = 12345, ev = 0;
    plat.alive = t_alive;
    plat.log = t_log;
    memset(&d, 0, sizeof d);
    d.p = &plat;
    d.pa = 0x24000000;
    d.size = 12u << 20;
    d.state = PICA_OK;
    d.block[0].size = PICA_DRV_CMD_SIZE;
    d.nblocks = 1;
    (void)big_min;
    CHECK_EQ(pica_drv_alloc(&d, 0, 1u << 20, 0x1000, &pa), PICA_OK);       /* the mirror's texture */
    for (i = 0; i < 3000; i++) {
        uint32_t sz;
        rnd = rnd * 1103515245u + 12345u;
        sz = (((rnd >> 8) % 96) + 1) * 1024;                                /* 1-96 KiB textures */
        while (n && (held + sz > (2u << 20) || n >= 210)) {                 /* LRU-ish eviction */
            k = (rnd >> 4) % n;
            CHECK_EQ(pica_drv_free(&d, 1, live[k]), PICA_OK);
            held -= livesz[k];
            live[k] = live[n - 1];
            livesz[k] = livesz[--n];
        }
        if (pica_drv_alloc(&d, 1, sz, 0, &pa) == PICA_OK) {
            live[n] = pa;
            livesz[n++] = (sz + PICA_DRV_ALIGN - 1) & ~(PICA_DRV_ALIGN - 1);
            held += livesz[n - 1];
        }
        if (i % 20)
            continue;
        for (;;) {                          /* a target comes and goes */
            uint32_t top = 0;
            if (pica_drv_alloc(&d, 1, 1u << 20, 0, &rt) == PICA_OK) {
                if (pica_drv_alloc(&d, 1, 600u << 10, 0, &swz) == PICA_OK)
                    break;
                CHECK_EQ(pica_drv_free(&d, 1, rt), PICA_OK);
            }
            if (!n) {
                fails++;
                rt = swz = 0;
                break;
            }
            for (k = 1; k < n; k++)         /* no room: evict the highest small block */
                if (live[k] > live[top])
                    top = k;
            CHECK_EQ(pica_drv_free(&d, 1, live[top]), PICA_OK);
            held -= livesz[top];
            ev += livesz[top];
            live[top] = live[n - 1];
            livesz[top] = livesz[--n];
        }
        if (rt) {
            CHECK_EQ(pica_drv_free(&d, 1, swz), PICA_OK);
            CHECK_EQ(pica_drv_free(&d, 1, rt), PICA_OK);
        }
    }
    *evicted_kb = ev >> 10;
    return fails;
}

static void test_alloc(void)
{
    uint32_t kb, pa, fails;
    static pica_plat_t plat;
    static pica_drv_t d;
    plat.alive = t_alive;
    plat.log = t_log;
    d.p = &plat;
    d.pa = 0x24000000;
    d.size = 12u << 20;
    d.state = PICA_OK;
    d.block[0].size = PICA_DRV_CMD_SIZE;
    d.nblocks = 1;
    CHECK_EQ(pica_drv_alloc(&d, 0, 1u << 20, 0x1000, &pa), PICA_OK);
    CHECK_EQ(pa, d.pa + PICA_DRV_HUGE_FLOOR(d.size) - (1u << 20));          /* big: top of the medium pool */
    CHECK_EQ(pica_drv_alloc(&d, 0, 4096, 0, &pa), PICA_OK);
    CHECK_EQ(pa, d.pa + PICA_DRV_CMD_SIZE);                                 /* small: from the bottom */
    fails = alloc_churn(PICA_DRV_BIG, &kb);
    CHECK_EQ(fails, 0);
    CHECK_EQ(d.full_logs, 0);               /* no arena-full line under this churn */
    CHECK(kb < 20 * 1024);          /* one pool + random eviction: ~42 MiB */
    printf("alloc: 150 targets placed in a 3/4 full arena, %u KiB of textures evicted for them\n", kb);
}

/* Hardware log 2026-11-25 (New-model console, 12 MiB arena): hundreds of
   failed allocations with 0.6-1.8 MiB free in 85-129 blocks, then a 4 MiB
   texture refused with 10.8 MiB free. Small layer textures were allowed into
   the top of the arena, so a huge block never found a contiguous gap. Here
   128 KiB blocks fill the arena, every other one is freed, and a 4 MiB block
   plus another 128 KiB one must still fit: the huge pool stays untouched. */
static void test_partition(void)
{
    static pica_plat_t plat;
    static pica_drv_t d;
    uint32_t pa[160], big, small, n = 0, i;
    plat.alive = t_alive;
    plat.log = t_log;
    memset(&d, 0, sizeof d);
    d.p = &plat;
    d.pa = 0x24000000;
    d.size = 12u << 20;
    d.state = PICA_OK;
    d.block[0].size = PICA_DRV_CMD_SIZE;
    d.nblocks = 1;
    while (n < 160 && pica_drv_alloc(&d, 1, 128u << 10, 0, &pa[n]) == PICA_OK)
        n++;
    CHECK(n > 20);
    for (i = 1; i < n; i += 2)
        CHECK_EQ(pica_drv_free(&d, 1, pa[i]), PICA_OK);
    CHECK_EQ(pica_drv_alloc(&d, 1, 4u << 20, 0, &big), PICA_OK);
    CHECK(big >= d.pa + PICA_DRV_HUGE_FLOOR(d.size));
    CHECK_EQ(pica_drv_alloc(&d, 1, 128u << 10, 0, &small), PICA_OK);
    CHECK(small < big);
    printf("arena: %u small blocks churned, 4 MiB + 128 KiB still fit\n", n);
}

/* The camera's frame buffers live in the arena under an owner that no client
   pid can have (kernel/shim/pica.c shim_pica_cam_buffers). A GL client's
   free_all must not hand that block back to the texture allocator. */
static void test_cam_reserve(void)
{
    static pica_plat_t plat;
    static pica_drv_t d;
    const uint32_t frames = 3u * 320u * 240u * 2u, owner = 0xFFFFFFFEu;
    uint32_t cam_pa, gl_pa, again_pa;
    plat.alive = t_alive;
    plat.log = t_log;
    d.p = &plat;
    d.pa = 0x24000000;
    d.size = 12u << 20;
    d.state = PICA_OK;
    d.block[0].size = PICA_DRV_CMD_SIZE;        /* as pica_drv_init() leaves it */
    d.nblocks = 1;
    CHECK_EQ(pica_drv_alloc(&d, owner, frames, 32, &cam_pa), PICA_OK);
    CHECK_EQ(cam_pa & 31u, 0u);                                 /* the CAM FIFO needs 32-byte alignment */
    CHECK_EQ(pica_drv_alloc(&d, 42, 4096, 0, &gl_pa), PICA_OK);
    CHECK(gl_pa + 4096 <= cam_pa || gl_pa >= cam_pa + frames);   /* no overlap with the camera frames */
    CHECK_EQ(pica_drv_free_all(&d, 42), 1u);                     /* one GL block given back */
    CHECK_EQ(pica_drv_alloc(&d, 43, frames, 32, &again_pa), PICA_OK);
    CHECK(again_pa != cam_pa);                                  /* the camera block survived free_all */
    printf("arena: camera reserves %u KiB under a private owner, GL churn leaves it alone\n", frames >> 10);
}

/* PICA_OP_SCATTER's page copy: an arena block into destination pages that
   are not physically contiguous (CA's second 320x480 surface after an app
   returns to the home screen), each through its own mapping, with the
   caller's lines dropped first and the CPU's copy written back. */
static uint8_t t_sys[3 * PICA_PAGE_SIZE + 64];
static uint32_t t_cache_calls, t_clean_calls;
static void *t_map_pa(uint32_t pa, uint32_t len)
{
    (void)len;
    if (pa == 0x30000000u) return t_sys;
    if (pa == 0x30004000u) return t_sys + PICA_PAGE_SIZE;
    if (pa == 0x30011000u) return t_sys + 2 * PICA_PAGE_SIZE;   /* the next page is 0x30012000 */
    return NULL;
}
static void t_cache_user(uint32_t va, uint32_t len) { (void)va; (void)len; t_cache_calls++; }
static void t_to_gpu(void *p, uint32_t len) { (void)p; (void)len; t_clean_calls++; }
static void t_from_gpu(void *p, uint32_t len) { (void)p; (void)len; }

static void test_scatter(void)
{
    static pica_drv_t d;
    static pica_plat_t plat;
    static uint8_t arena[PICA_DRV_CMD_SIZE + 3 * PICA_PAGE_SIZE + 256];
    static const uint32_t pages[3] = { 0x30000000u, 0x30004000u, 0x30011000u };
    uint32_t pa, i, n = 3 * PICA_PAGE_SIZE / 4;
    memset(&plat, 0, sizeof plat);
    plat.map_pa = t_map_pa;
    plat.cache_user = t_cache_user;
    plat.to_gpu = t_to_gpu;
    plat.from_gpu = t_from_gpu;
    memset(&d, 0, sizeof d);
    d.p = &plat;
    d.va = arena;
    d.pa = 0x24000000u;
    d.size = sizeof arena;
    d.state = PICA_OK;
    d.block[0].size = PICA_DRV_CMD_SIZE;
    d.nblocks = 1;
    CHECK_EQ(pica_drv_alloc(&d, 0, 3 * PICA_PAGE_SIZE, 0, &pa), PICA_OK);
    for (i = 0; i < n; i++)
        ((uint32_t *)(arena + (pa - d.pa)))[i] = 0x11110000u + i;
    memset(t_sys, 0xA5, sizeof t_sys);
    t_cache_calls = t_clean_calls = 0;
    for (i = 0; i < 3; i++)
        CHECK_EQ(pica_drv_copy_page(&d, pa + i * PICA_PAGE_SIZE, 0x10000000u + i * PICA_PAGE_SIZE,
                                    pages[i], PICA_PAGE_SIZE), PICA_OK);
    for (i = 0; i < n; i++)
        CHECK_EQ(((uint32_t *)t_sys)[i], 0x11110000u + i);
    CHECK_EQ(t_cache_calls, 3);                 /* one page each: clean+invalidate, then write back */
    CHECK_EQ(t_clean_calls, 3);
    /* only the requested bytes: 64 leaves the rest of the page alone */
    memset(t_sys, 0xA5, sizeof t_sys);
    CHECK_EQ(pica_drv_copy_page(&d, pa, 0x10000000u, pages[0], PICA_PAGE_SIZE), PICA_OK);
    CHECK_EQ(pica_drv_copy_page(&d, pa + PICA_PAGE_SIZE, 0x10001000u, pages[1], 64), PICA_OK);
    CHECK_EQ(((uint32_t *)t_sys)[PICA_PAGE_SIZE / 4 - 1], 0x11110000u + PICA_PAGE_SIZE / 4 - 1);
    CHECK_EQ(((uint32_t *)t_sys)[PICA_PAGE_SIZE / 4 + 15], 0x11110000u + PICA_PAGE_SIZE / 4 + 15);
    CHECK_EQ(((uint32_t *)t_sys)[PICA_PAGE_SIZE / 4 + 16], 0xA5A5A5A5u);
    /* a page with no mapping, an unaligned destination, a source outside the
       arena (or in the command staging) and zero bytes are refused */
    CHECK_EQ(pica_drv_copy_page(&d, pa, 0x10000000u, 0x40000000u, PICA_PAGE_SIZE), PICA_ERR_RANGE);
    CHECK_EQ(pica_drv_copy_page(&d, pa, 0x10000001u, pages[0], PICA_PAGE_SIZE), PICA_ERR_RANGE);
    CHECK_EQ(pica_drv_copy_page(&d, d.pa, 0x10000000u, pages[0], PICA_PAGE_SIZE), PICA_ERR_RANGE);
    CHECK_EQ(pica_drv_copy_page(&d, pa, 0x10000000u, pages[0], 0), PICA_ERR_RANGE);
    CHECK_EQ(pica_drv_free(&d, 0, pa), PICA_OK);
    printf("scatter: 3 scattered pages copied, cache maintained, bad page and range refused\n");
}

int main(int argc, char **argv)
{
    test_cmd();
    test_floats();
    test_state();
    test_tex();
    test_ff();
    test_sim();
    test_alloc();
    test_partition();
    test_cam_reserve();
    test_scatter();
    (void)argc; (void)argv;
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
