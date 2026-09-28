/*
 * quad_gxm.c -- GXM polygon rasteriser for Namco System 22 on PS Vita.
 * Direct sceGxm hardware rasterization replacing quad_gl.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitashark.h>
#include "eng.h"
#include "geo_hw.h"
#include "tex_bake.h"
#include "gxm_tex.h"
#include "quad_gxm.h"

float  g_scene_x0 = 0.0f, g_scene_x1 = (float)ENG_SCREEN_W;
double g_perf_bake, g_perf_gl, g_perf_clip;
int    g_bri_min = 9999, g_bri_max = -9999;
int    g_fogged_quads, g_fogA_min = 999, g_fogA_max = -999;
int    g_tex_clipbox = 0;
int    g_eng_degen_uv_legacy = 0;
int    g_perf_enabled = 0;

double eng_now(void) {
    if (!g_perf_enabled) return 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int eng_fog_alpha(const eng_fog *f, int32_t z) {
    if (!f->tab) return f->alpha_const < 0 ? 255 : f->alpha_const;
    int cz = (int)(z >> 8);
    if (cz < 0) cz = 0;
    if (cz > 0x1fff) cz = 0x1fff;
    int ff = f->tab[cz] + f->sdelta;
    if (ff <= 0) return 255;
    if (ff > 0xff) ff = 0xff;
    return 0xff - ff;
}

static int g_sort_tie_emit;
static int zcmp(const void *a, const void *b) {
    const geo_quad *qa = (const geo_quad *)a, *qb = (const geo_quad *)b;
    if (qa->zsort != qb->zsort)
        return (qa->zsort > qb->zsort) ? -1 : 1;
    if (g_sort_tie_emit) return (qa->order < qb->order) ? -1 : (qa->order > qb->order) ? 1 : 0;
    return (qa->order > qb->order) ? -1 : (qa->order < qb->order) ? 1 : 0;
}

typedef struct { int32_t zsort, order; uint32_t index; } quad_sort_key;
static int key_zcmp(const void *a, const void *b) {
    const quad_sort_key *qa = a, *qb = b;
    if (qa->zsort != qb->zsort) return qa->zsort > qb->zsort ? -1 : 1;
    if (g_sort_tie_emit) return qa->order < qb->order ? -1 : qa->order > qb->order ? 1 : 0;
    return qa->order > qb->order ? -1 : qa->order < qb->order ? 1 : 0;
}

/* Keep depth keys contiguous and leave the large vertex records in emission
 * order. The renderer can follow indices without copying the polygon array. */
uint32_t *quad_gxm_sort_indices(const geo_quad *buf, int n, int tie_emit) {
    static quad_sort_key *keys;
    static uint32_t *indices;
    static size_t capacity;
    if (n < 1) return NULL;
    g_sort_tie_emit = tie_emit;
    if ((size_t)n > capacity) {
        quad_sort_key *new_keys = malloc((size_t)n * sizeof *keys);
        uint32_t *new_indices = malloc((size_t)n * sizeof *indices);
        if (!new_keys || !new_indices) { free(new_keys); free(new_indices); return NULL; }
        free(keys); free(indices);
        keys = new_keys; indices = new_indices; capacity = n;
    }
    for (int i = 0; i < n; i++) {
        keys[i].zsort = buf[i].zsort; keys[i].order = buf[i].order; keys[i].index = i;
    }
    qsort(keys, (size_t)n, sizeof keys[0], key_zcmp);
    for (int i = 0; i < n; i++) indices[i] = keys[i].index;
    return indices;
}

void eng_quad_sort(geo_quad *buf, int n, int tie_emit) {
    if (n < 2) return;
    uint32_t *indices = quad_gxm_sort_indices(buf, n, tie_emit);
    if (!indices) { qsort(buf, (size_t)n, sizeof buf[0], zcmp); return; }
    /* Apply permutation cycles: each large polygon is moved just once. */
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        if (indices[i] == i) continue;
        geo_quad saved = buf[i];
        uint32_t dst = i;
        while (indices[dst] != i) {
            uint32_t src = indices[dst];
            buf[dst] = buf[src];
            indices[dst] = dst;
            dst = src;
        }
        buf[dst] = saved;
        indices[dst] = dst;
    }
}

/* Screen space clipping against scene boundaries */
typedef struct { float x, y, s, t, w, bri; } geo_sv;

static int clip_edge(const geo_sv *in, int n, geo_sv *out, int axis,
                     float limit, int keep_greater) {
    int m = 0;
    for (int i = 0; i < n && m < 30; i++) {
        const geo_sv *a = &in[i], *b = &in[(i + 1) % n];
        float av = axis ? a->y : a->x, bv = axis ? b->y : b->x;
        int ain = keep_greater ? (av >= limit) : (av <= limit);
        int bin = keep_greater ? (bv >= limit) : (bv <= limit);
        if (ain) out[m++] = *a;
        if (ain != bin && m < 30) {
            float d = bv - av;
            float t = (d != 0.0f) ? (limit - av) / d : 0.0f;
            geo_sv v;
            v.x = a->x + (b->x - a->x) * t;
            v.y = a->y + (b->y - a->y) * t;
            if (axis) v.y = limit; else v.x = limit;
            v.s = a->s + (b->s - a->s) * t;
            v.t = a->t + (b->t - a->t) * t;
            v.w = a->w + (b->w - a->w) * t;
            v.bri = a->bri + (b->bri - a->bri) * t;
            out[m++] = v;
        }
    }
    return m;
}

static int clip_to_screen(const geo_sv *in, int n, geo_sv *out,
                          float x0, float x1, float y0, float y1) {
    geo_sv a[32], b[32];
    if (n > 16) n = 16;
    unsigned all = 15, any = 0;
    for (int i = 0; i < n; i++) {
        unsigned code = (in[i].x < x0 ? 1 : 0) | (in[i].x > x1 ? 2 : 0) |
                        (in[i].y < y0 ? 4 : 0) | (in[i].y > y1 ? 8 : 0);
        all &= code; any |= code;
    }
    if (n < 3 || all) return 0;
    if (!any) { memcpy(out, in, (size_t)n * sizeof *out); return n; }
    for (int i = 0; i < n; i++) a[i] = in[i];
    n = clip_edge(a, n, b, 0, x0, 1);           if (n < 3) return 0;
    n = clip_edge(b, n, a, 0, x1, 0);           if (n < 3) return 0;
    n = clip_edge(a, n, b, 1, y0, 1);                 if (n < 3) return 0;
    n = clip_edge(b, n, a, 1, y1, 0); if (n < 3) return 0;
    for (int i = 0; i < n && i < 32; i++) out[i] = a[i];
    return n;
}

/* ---- GXM Vertex and Shader Structures ---- */
typedef struct {
    float x, y, z;
    float u, v, iw;
    float r, g, b, a;
} GxmQuadVertex;

static SceGxmContext *s_ctx = NULL;
static SceGxmShaderPatcher *s_patcher = NULL;

static SceGxmShaderPatcherId s_vp_id;
static SceGxmShaderPatcherId s_fp_opaque_id;
static SceGxmShaderPatcherId s_fp_blend_id;

static SceGxmVertexProgram *s_vprog = NULL;
static SceGxmFragmentProgram *s_fprog_opaque = NULL;
static SceGxmFragmentProgram *s_fprog_blend = NULL;
static SceGxmFragmentProgram *s_fprog_text = NULL;

static const SceGxmProgramParameter *s_u_screen_params = NULL;
static const SceGxmProgramParameter *s_u_params = NULL;

#define VTX_RING_SIZE (4 * 1024 * 1024)
static SceUID s_vtx_ring_uid = -1;
static uint8_t *s_vtx_ring_base = NULL;
static size_t s_vtx_ring_offset = 0;

static SceUID s_idx_ring_uid = -1;
static uint16_t *s_fan_indices = NULL;

static float s_screen_params[4];

static const char quad_v_src[] =
  "void main(float3 aPosition, float3 aTexcoord, float4 aColor,\n"
  "          uniform float4 uScreenParams,\n"
  "          out float4 vPosition : POSITION, out float3 vTexcoord : TEXCOORD0, out float4 vColor : COLOR)\n"
  "{\n"
  "  vPosition = float4(aPosition.x * uScreenParams.x + uScreenParams.z,\n"
  "                     aPosition.y * uScreenParams.y + uScreenParams.w,\n"
  "                     aPosition.z,\n"
  "                     1.0f);\n"
  "  vTexcoord = aTexcoord;\n"
  "  vColor = aColor;\n"
  "}\n";

static const char quad_f_src[] =
  "float4 main(float3 vTexcoord : TEXCOORD0, float4 vColor : COLOR,\n"
  "            uniform sampler2D uTex,\n"
  "            uniform float4 uParams) /* x: is_textured, y: alpha_ref, z: shade_scale, w: unused */\n"
  "{\n"
  "  float4 col = uParams.x > 0.5f ? tex2D(uTex, vTexcoord.xy / vTexcoord.z) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
  "  if (col.a <= uParams.y) discard;\n"
  "  col.rgb *= vColor.rgb * uParams.z;\n"
  "  col.a = vColor.a;\n"
  "  return col;\n"
  "}\n";

void quad_gxm_set_scene_extents(float x0, float x1) {
    g_scene_x0 = x0;
    g_scene_x1 = x1;

    float w = g_scene_x1 - g_scene_x0;
    float h = (float)ENG_SCREEN_H;

    s_screen_params[0] = 2.0f / w;
    s_screen_params[1] = -2.0f / h;
    s_screen_params[2] = (-2.0f * g_scene_x0 / w) - 1.0f;
    s_screen_params[3] = 1.0f;
}

static SceGxmProgram *compile_one(const char *src, shark_type type, const char *what)
{
    uint32_t sz = (uint32_t)strlen(src);
    SceGxmProgram *p = shark_compile_shader(src, &sz, type);
    if (!p || !sz) {
#ifdef __vita__
        extern void vita_log(const char *fmt, ...);
        vita_log("[GXM] %s failed to compile\n", what);
#endif
        return NULL;
    }
    SceGxmProgram *copy = malloc(sz);
    if (!copy) return NULL;
    memcpy(copy, p, sz);
    shark_clear_output();
#ifdef __vita__
    extern void vita_log(const char *fmt, ...);
    vita_log("[GXM] %s -> %u bytes compiled & copied\n", what, sz);
#endif
    return copy;
}

static unsigned s_frame_slot;
static void draw_fan(int n);
#include "quad_gxm_hud.inc"

bool quad_gxm_init(SceGxmContext *ctx, SceGxmShaderPatcher *patcher) {
    s_ctx = ctx;
    s_patcher = patcher;

    /* Initialize vertex ring buffer in CDRAM */
    s_vtx_ring_uid = sceKernelAllocMemBlock("gxm_quad_vtx", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, VTX_RING_SIZE, NULL);
    if (s_vtx_ring_uid < 0) {
        s_vtx_ring_uid = sceKernelAllocMemBlock("gxm_quad_vtx_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, VTX_RING_SIZE, NULL);
    }
    if (s_vtx_ring_uid < 0) return false;

    void *base = NULL;
    sceKernelGetMemBlockBase(s_vtx_ring_uid, &base);
    sceGxmMapMemory(base, VTX_RING_SIZE, SCE_GXM_MEMORY_ATTRIB_RW);
    s_vtx_ring_base = (uint8_t *)base;
    s_vtx_ring_offset = 0;

    /* Initialize fan index array 0, 1, 2, ... 63 */
    s_idx_ring_uid = sceKernelAllocMemBlock("gxm_quad_idx", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 4096, NULL);
    if (s_idx_ring_uid < 0) {
        s_idx_ring_uid = sceKernelAllocMemBlock("gxm_quad_idx_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, NULL);
    }
    if (s_idx_ring_uid >= 0) {
        sceKernelGetMemBlockBase(s_idx_ring_uid, &base);
        sceGxmMapMemory(base, 4096, SCE_GXM_MEMORY_ATTRIB_READ);
        s_fan_indices = (uint16_t *)base;
        for (int i = 0; i < 64; i++) s_fan_indices[i] = (uint16_t)i;
    }

    /* Compile Cg shaders via vitashark */
    SceGxmProgram *vp_prog = compile_one(quad_v_src, SHARK_VERTEX_SHADER, "quad_v");
    SceGxmProgram *fp_prog = compile_one(quad_f_src, SHARK_FRAGMENT_SHADER, "quad_f");

    if (!vp_prog || !fp_prog) {
        fprintf(stderr, "[GXM] Shader compilation failed!\n");
        return false;
    }

    sceGxmShaderPatcherRegisterProgram(s_patcher, vp_prog, &s_vp_id);
    sceGxmShaderPatcherRegisterProgram(s_patcher, fp_prog, &s_fp_opaque_id);
    sceGxmShaderPatcherRegisterProgram(s_patcher, fp_prog, &s_fp_blend_id);

    /* Register indices are scalar offsets assigned by the compiler, not
     * sequential attribute numbers (float3 consumes three registers). */
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(vp_prog, "aPosition");
    const SceGxmProgramParameter *uv = sceGxmProgramFindParameterByName(vp_prog, "aTexcoord");
    const SceGxmProgramParameter *color = sceGxmProgramFindParameterByName(vp_prog, "aColor");
    if (!pos || !uv || !color) return false;
    extern void vita_log(const char *fmt, ...);
    vita_log("[GXM] attribute registers: position=%u uv=%u color=%u\n",
        sceGxmProgramParameterGetResourceIndex(pos),
        sceGxmProgramParameterGetResourceIndex(uv),
        sceGxmProgramParameterGetResourceIndex(color));
    /* Vertex attributes */
    SceGxmVertexAttribute attr[3];
    memset(attr, 0, sizeof(attr));
    attr[0].streamIndex = 0;
    attr[0].offset = 0;
    attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 3;
    attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);

    attr[1].streamIndex = 0;
    attr[1].offset = 12;
    attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[1].componentCount = 3;
    attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(uv);

    attr[2].streamIndex = 0;
    attr[2].offset = 24;
    attr[2].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[2].componentCount = 4;
    attr[2].regIndex = sceGxmProgramParameterGetResourceIndex(color);

    SceGxmVertexStream stream;
    stream.stride = sizeof(GxmQuadVertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

    int result = sceGxmShaderPatcherCreateVertexProgram(s_patcher, s_vp_id, attr, 3, &stream, 1, &s_vprog);
    if (result < 0) { vita_log("[GXM] vertex program failed: %08X\n", result); return false; }

    const SceGxmProgram *vp_header = sceGxmShaderPatcherGetProgramFromId(s_vp_id);

    /* Fragment program - opaque */
    sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fp_opaque_id,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
        NULL, vp_header, &s_fprog_opaque);

    /* Fragment program - alpha blend */
    SceGxmBlendInfo blend;
    memset(&blend, 0, sizeof(blend));
    blend.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B;
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fp_blend_id,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
        &blend, vp_header, &s_fprog_blend);

    /* The scene alpha channel is the arcade polygon-over-text priority bit. */
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_fp_opaque_id,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
        &blend, vp_header, &s_fprog_text);

    s_u_screen_params = sceGxmProgramFindParameterByName(vp_prog, "uScreenParams");
    s_u_params = sceGxmProgramFindParameterByName(fp_prog, "uParams");

    if (!s_fprog_opaque || !s_fprog_blend || !s_fprog_text || !s_u_screen_params || !s_u_params || !s_fan_indices)
        return false;
    hud_init(vp_header);
    quad_gxm_set_scene_extents(0.0f, (float)ENG_SCREEN_W);
    return true;
}

void quad_gxm_shutdown(void) {
    hud_shutdown();
    if (s_fprog_text) { sceGxmShaderPatcherReleaseFragmentProgram(s_patcher, s_fprog_text); s_fprog_text = NULL; }
    if (s_vprog) { sceGxmShaderPatcherReleaseVertexProgram(s_patcher, s_vprog); s_vprog = NULL; }
    if (s_fprog_opaque) { sceGxmShaderPatcherReleaseFragmentProgram(s_patcher, s_fprog_opaque); s_fprog_opaque = NULL; }
    if (s_fprog_blend) { sceGxmShaderPatcherReleaseFragmentProgram(s_patcher, s_fprog_blend); s_fprog_blend = NULL; }
    if (s_vp_id) { sceGxmShaderPatcherUnregisterProgram(s_patcher, s_vp_id); s_vp_id = 0; }
    if (s_fp_opaque_id) { sceGxmShaderPatcherUnregisterProgram(s_patcher, s_fp_opaque_id); s_fp_opaque_id = 0; }
    if (s_fp_blend_id) { sceGxmShaderPatcherUnregisterProgram(s_patcher, s_fp_blend_id); s_fp_blend_id = 0; }

    if (s_vtx_ring_uid >= 0) {
        if (s_vtx_ring_base) sceGxmUnmapMemory(s_vtx_ring_base);
        sceKernelFreeMemBlock(s_vtx_ring_uid);
        s_vtx_ring_uid = -1;
        s_vtx_ring_base = NULL;
    }
    if (s_idx_ring_uid >= 0) {
        if (s_fan_indices) sceGxmUnmapMemory(s_fan_indices);
        sceKernelFreeMemBlock(s_idx_ring_uid);
        s_idx_ring_uid = -1;
        s_fan_indices = NULL;
    }
}

#include "quad_gxm_batch.inc"

bool quad_gxm_buffered(void) { return batch_active && hud_active; }
void quad_gxm_select_frame(unsigned slot) { s_frame_slot=slot%3; }

void eng_draw_begin(void) {
    batch_begin();
    /* rr_gxm_draw has waited for the preceding scene. */
    s_vtx_ring_offset = s_frame_slot*(VTX_RING_SIZE/4);
    if (s_ctx && s_vprog) {
        sceGxmSetCullMode(s_ctx, SCE_GXM_CULL_NONE);
        sceGxmSetFrontDepthFunc(s_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(s_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
        sceGxmSetBackDepthFunc(s_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetBackDepthWriteEnable(s_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
        sceGxmSetRegionClip(s_ctx, SCE_GXM_REGION_CLIP_NONE, 0, 0, 0, 0);
        sceGxmSetVertexProgram(s_ctx, s_vprog);

    }
}

void eng_draw_end(void) {
    if (batch_active) batch_end();
    /* Reset scissor to full screen */
    if (s_ctx) {
        sceGxmSetRegionClip(s_ctx, SCE_GXM_REGION_CLIP_NONE, 0, 0, 0, 0);
    }
}

static void draw_fan(int n) {
    void *buf = NULL;
    int result = sceGxmReserveVertexDefaultUniformBuffer(s_ctx, &buf);
    if (result >= 0) result = sceGxmSetUniformDataF(buf, s_u_screen_params, 0, 4, s_screen_params);
    if (result >= 0) result = sceGxmDraw(s_ctx, SCE_GXM_PRIMITIVE_TRIANGLE_FAN,
                                       SCE_GXM_INDEX_FORMAT_U16, s_fan_indices, n);
    static int reported;
    if (result < 0 && !reported) {
        extern void vita_log(const char *fmt, ...);
        vita_log("[GXM] draw failed: %08X\n", result);
        reported = 1;
    }
}

void eng_draw_quad(const geo_quad *q, const eng_draw_cfg *cfg) {
    if (batch_active) { batch_quad(q, cfg); return; }
    if (!s_ctx || !s_vprog || !s_vtx_ring_base) return;

    if (cfg->flat_white) {
        if (q->nrv < 3) return;
        int n = q->nrv > 32 ? 32 : q->nrv;
        size_t bytes = n * sizeof(GxmQuadVertex);
        if (s_vtx_ring_offset + bytes > VTX_RING_SIZE) return;
        GxmQuadVertex *vtx = (GxmQuadVertex *)(s_vtx_ring_base + s_vtx_ring_offset);
        s_vtx_ring_offset += bytes;

        for (int i = 0; i < n; i++) {
            vtx[i].x = q->rv[i].sx16 / 16.0f;
            vtx[i].y = q->rv[i].sy16 / 16.0f;
            vtx[i].z = 0.5f;
            vtx[i].u = 0.0f; vtx[i].v = 0.0f; vtx[i].iw = 1.0f;
            vtx[i].r = 1.0f; vtx[i].g = 1.0f; vtx[i].b = 1.0f; vtx[i].a = 1.0f;
        }

        sceGxmSetFragmentProgram(s_ctx, s_fprog_opaque);
        sceGxmSetFragmentTexture(s_ctx, 0, gxm_tex_white());

        void *params_buf = NULL;
        sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &params_buf);
        float params[4] = { 0.0f, -1.0f, 1.0f, 0.0f };
        sceGxmSetUniformDataF(params_buf, s_u_params, 0, 4, params);

        sceGxmSetVertexStream(s_ctx, 0, vtx);
        draw_fan(n);
        return;
    }

    int pal_group = (q->color >> 8) & 0x7F;
    int solid = q->objectflags != 0, solid_noshade = 0;
    float solid_rgb[3] = { 0, 0, 0 };
    if (solid) {
        int col = (int)((q->color >> 8) & 0xff), pen;
        if (q->objectflags & 6) { pen = q->cz_adjust & 0x7fff; solid_noshade = 1; }
        else pen = ((col & 0x7f) << 8) + (((q->cz_adjust >> 16) & 0x7f) & (col | 0x1f));
        pen &= 0x7fff;
        for (int c = 0; c < 3; c++) solid_rgb[c] = direct_palette[pen >> 8][pen & 0xff][c] / 255.0f;
    }

    int min_u = 0xFFFF, min_v = 0xFFFF, max_u = 0, max_v = 0;
    if (!g_tex_clipbox) {
        min_u = q->uvbox[0]; max_u = q->uvbox[1];
        min_v = q->uvbox[2]; max_v = q->uvbox[3];
    } else {
        for (int i = 0; i < q->nrv; i++) {
            int uu = (int)q->rv[i].u, vv = (int)q->rv[i].v;
            if (uu < min_u) min_u = uu;  if (uu > max_u) max_u = uu;
            if (vv < min_v) min_v = vv;  if (vv > max_v) max_v = vv;
        }
    }
    int range_u = max_u - min_u + 1, range_v = max_v - min_v + 1;
    if (range_u < 1) range_u = 1;
    if (range_v < 1) range_v = 1;

    /* Scissor mapping */
    const int sx0 = (int)g_scene_x0, sx1 = (int)g_scene_x1 - 1;
    const int fullw = q->clip[0] <= 0 && q->clip[1] >= ENG_SCREEN_W - 1;
    int cminx = fullw ? sx0 : (q->clip[0] < 0 ? 0 : q->clip[0]);
    int cmaxx = fullw ? sx1 : (q->clip[1] > ENG_SCREEN_W - 1 ? ENG_SCREEN_W - 1 : q->clip[1]);
    int cminy = q->clip[2] < 0 ? 0 : q->clip[2];
    int cmaxy = q->clip[3] > ENG_SCREEN_H - 1 ? ENG_SCREEN_H - 1 : q->clip[3];
    if (cminx > cmaxx || cminy > cmaxy) return;

    /* Map scissor to Vita 960x544 coords */
    float kx = 960.0f / (g_scene_x1 - g_scene_x0);
    float ky = 544.0f / (float)ENG_SCREEN_H;
    int sc_x0 = (int)((cminx - g_scene_x0) * kx + 0.5f);
    int sc_x1 = (int)((cmaxx + 1 - g_scene_x0) * kx + 0.5f) - 1;
    int sc_y0 = (int)(cminy * ky + 0.5f);
    int sc_y1 = (int)((cmaxy + 1) * ky + 0.5f) - 1;

    if (sc_x0 < 0) sc_x0 = 0;
    if (sc_y0 < 0) sc_y0 = 0;
    if (sc_x1 > 959) sc_x1 = 959;
    if (sc_y1 > 543) sc_y1 = 543;
    if (sc_x0 > sc_x1 || sc_y0 > sc_y1) return;

    sceGxmSetRegionClip(s_ctx, SCE_GXM_REGION_CLIP_NONE, 0, 0, 0, 0);

    uint32_t tex_id = 0;
    float bsu = 1.0f, bsv = 1.0f;

    int bmin_u = min_u, bmin_v = min_v, brange_u = range_u, brange_v = range_v;
    if (range_u < 16) { bmin_u = (min_u * 2 + range_u) / 2 - 8; brange_u = 16; }
    if (range_v < 16) { bmin_v = (min_v * 2 + range_v) / 2 - 8; brange_v = 16; }
    if (bmin_u < 0) bmin_u = 0;
    if (bmin_v < 0) bmin_v = 0;

    geo_sv sv[8], cv[32];
    int nsv = 0;
    for (int i = 0; i < q->nrv && nsv < 8; i++) {
        float cu = (range_u == 1 && brange_u != range_u) ? 0.5f : 0.0f;
        float cv_ = (range_v == 1 && brange_v != range_v) ? 0.5f : 0.0f;
        if (cfg->texel_centre) { if (cu == 0.0f) cu = 0.5f; if (cv_ == 0.0f) cv_ = 0.5f; }
        float tu = ((float)((int)q->rv[i].u - bmin_u) + cu) / (float)brange_u * bsu;
        float tv = ((float)((int)q->rv[i].v - bmin_v) + cv_) / (float)brange_v * bsv;
        float iw = (q->rv[i].z > 0) ? 1.0f / (float)q->rv[i].z : 1.0f;

        float sh = 1.0f;
        if (cfg->shade) {
            sh = (float)q->rv[i].bri / 64.0f;
            if (sh < 0.0f) sh = 0.0f;
        }
        sv[nsv].x = q->rv[i].sx16 / 16.0f;
        sv[nsv].y = q->rv[i].sy16 / 16.0f;
        sv[nsv].s = tu * iw;
        sv[nsv].t = tv * iw;
        sv[nsv].w = iw;
        sv[nsv].bri = sh;
        nsv++;
    }

    /* Exact per-viewport clipping is essential for the rear-view mirror.
     * GXM region clipping is tile-granular; clip interpolants on the CPU. */
    int ncv = clip_to_screen(sv, nsv, cv,
                             fullw ? g_scene_x0 : (float)cminx,
                             fullw ? g_scene_x1 : (float)(cmaxx + 1),
                             (float)cminy, (float)(cmaxy + 1));
    if (ncv < 3) return;
    /* Invisible polygons must not consume texture memory or bake time. */
    if (!solid) {
        tex_id = bake_quad_texture(min_u, min_v, range_u, range_v,
                                   q->texbank, pal_group, q->cmode, &bsu, &bsv);
        if (!gxm_tex_valid(tex_id)) return;
        for (int i = 0; i < ncv; i++) { cv[i].s *= bsu; cv[i].t *= bsv; }
    }

    int rgb_scale = 1;
    if (cfg->shade) {
        float peak = 0.0f;
        for (int i = 0; i < ncv; i++) if (cv[i].bri > peak) peak = cv[i].bri;
        if (peak > 2.0f)      rgb_scale = 4;
        else if (peak > 1.0f) rgb_scale = 2;
    }

    const float prio_a = !cfg->write_prio_alpha ? 1.0f : ((q->cmode & 7) == 1 ? 1.0f : 0.0f);
    int n = ncv > 32 ? 32 : ncv;
    size_t bytes = n * sizeof(GxmQuadVertex);
    if (s_vtx_ring_offset + bytes > VTX_RING_SIZE) return;
    GxmQuadVertex *vtx = (GxmQuadVertex *)(s_vtx_ring_base + s_vtx_ring_offset);
    s_vtx_ring_offset += bytes;

    float inv = 1.0f / (float)rgb_scale;
    if (solid) {
        for (int i = 0; i < n; i++) {
            float k = solid_noshade ? 1.0f : cv[i].bri;
            vtx[i].x = cv[i].x; vtx[i].y = cv[i].y; vtx[i].z = 0.5f;
            vtx[i].u = 0.0f; vtx[i].v = 0.0f; vtx[i].iw = 1.0f;
            vtx[i].r = solid_rgb[0] * k;
            vtx[i].g = solid_rgb[1] * k;
            vtx[i].b = solid_rgb[2] * k;
            vtx[i].a = prio_a;
        }
    } else {
        for (int i = 0; i < n; i++) {
            vtx[i].x = cv[i].x; vtx[i].y = cv[i].y; vtx[i].z = 0.5f;
            /* Interpolate u/z, v/z and 1/z; divide in the fragment shader. */
            vtx[i].u = cv[i].s;
            vtx[i].v = cv[i].t;
            vtx[i].iw = cv[i].w;
            vtx[i].r = cv[i].bri * inv;
            vtx[i].g = cv[i].bri * inv;
            vtx[i].b = cv[i].bri * inv;
            vtx[i].a = prio_a;
        }
    }

    sceGxmSetFragmentProgram(s_ctx, s_fprog_opaque);
    SceGxmTexture *tex = solid ? gxm_tex_white() : gxm_tex_get(tex_id);
    sceGxmSetFragmentTexture(s_ctx, 0, tex);

    void *params_buf = NULL;
    sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &params_buf);
    float params[4] = {
        solid ? 0.0f : 1.0f,
        cfg->write_prio_alpha ? -1.0f : 0.05f,
        (float)rgb_scale,
        0.0f
    };
    sceGxmSetUniformDataF(params_buf, s_u_params, 0, 4, params);

    sceGxmSetVertexStream(s_ctx, 0, vtx);
    draw_fan(n);

    /* CZ Depth Fog (second pass) */
    if (cfg->fog && cfg->fog_quad) {
        int cz_off = (q->cz_adjust & 0x800000) != 0;
        eng_fog f;
        memset(&f, 0, sizeof f);
        f.alpha_const = -1;
        if (!cz_off && cfg->fog_quad(q, &f)) {
            float fr = f.rgb[0] / 255.0f, fg = f.rgb[1] / 255.0f, fb = f.rgb[2] / 255.0f;
            int any = 0;
            for (int i = 0; i < ncv; i++) {
                int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                if (eng_fog_alpha(&f, zz) < 255) { any = 1; break; }
            }
            if (any) {
                if (s_vtx_ring_offset + bytes > VTX_RING_SIZE) return;
                GxmQuadVertex *fvtx = (GxmQuadVertex *)(s_vtx_ring_base + s_vtx_ring_offset);
                s_vtx_ring_offset += bytes;

                for (int i = 0; i < n; i++) {
                    int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                    int a = 255 - eng_fog_alpha(&f, zz);
                    fvtx[i].x = cv[i].x; fvtx[i].y = cv[i].y; fvtx[i].z = 0.5f;
                    fvtx[i].u = 0.0f; fvtx[i].v = 0.0f; fvtx[i].iw = 1.0f;
                    float k = cfg->fog_before_shade ? cv[i].bri : 1.0f;
                    fvtx[i].r = fr * k; fvtx[i].g = fg * k; fvtx[i].b = fb * k;
                    fvtx[i].a = a / 255.0f;
                }

                sceGxmSetFragmentProgram(s_ctx, s_fprog_blend);
                sceGxmSetFragmentTexture(s_ctx, 0, gxm_tex_white());

                sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &params_buf);
                float fparams[4] = { 0.0f, -1.0f, 1.0f, 0.0f };
                sceGxmSetUniformDataF(params_buf, s_u_params, 0, 4, fparams);

                sceGxmSetVertexStream(s_ctx, 0, fvtx);
                draw_fan(n);
            }
        }
    }
}

/* Text and screen-space polygons share the same arcade coordinates. */
void quad_gxm_draw_text(const SceGxmTexture *tex) {
    if (!s_ctx || !s_fprog_text || !tex) return;
    size_t bytes = 4 * sizeof(GxmQuadVertex);
    if (s_vtx_ring_offset + bytes > VTX_RING_SIZE) return;
    GxmQuadVertex *v = (GxmQuadVertex *)(s_vtx_ring_base + s_vtx_ring_offset);
    s_vtx_ring_offset += bytes;
    for (int i = 0; i < 4; i++) {
        float u = (i == 1 || i == 2) ? 1.0f : 0.0f;
        float t = i >= 2 ? 1.0f : 0.0f;
        v[i] = (GxmQuadVertex){u * ENG_SCREEN_W, t * ENG_SCREEN_H, 0.5f,
                               u, t, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    }
    sceGxmSetVertexProgram(s_ctx, s_vprog);
    sceGxmSetFragmentProgram(s_ctx, s_fprog_text);
    sceGxmSetFragmentTexture(s_ctx, 0, tex);
    void *buf = NULL;
    sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &buf);
    float params[4] = {1.0f, 0.05f, 1.0f, 0.0f};
    sceGxmSetUniformDataF(buf, s_u_params, 0, 4, params);
    sceGxmSetVertexStream(s_ctx, 0, v);
    draw_fan(4);
}
