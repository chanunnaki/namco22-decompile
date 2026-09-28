/*
 * rr_gxm.c -- Rave Racer native GXM renderer for PS Vita.
 * Direct hardware rendering through sceGxm (no vitaGL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <stdatomic.h>
#include <zlib.h>
#include <psp2/gxm.h>
#include <vita2d.h>
#include <vitashark.h>

#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "quad_gxm.h"
#include "tex_bake.h"
#include "gxm_tex.h"
#include "rr_mem.h"
#include "rr_dsp.h"
#include "rr_scene.h"
#include "rr_gxm.h"
#include "rr_vita_worker.h"
#include "c25.h"

#define DISPLAY_WIDTH 960
#define DISPLAY_HEIGHT 544

#define NW 640
#define NH 480

static uint8_t gamma_prom[3][256];
static bool    assets_ok, gxm_ready;

/* The renderer owns this immutable copy until its job completes. The game
 * continues to mutate g_rr and DSP point RAM on the main thread. */
typedef struct {
    uint32_t poly[RR_POLY_WORDS];
    uint8_t czram[RR_CZRAM_SIZE], mixer[RR_MIXER_SIZE], pal[RR_PAL_SIZE];
    uint8_t cgram[RR_CGRAM_SIZE], text[RR_TEXT_SIZE], tilemapattr[0x10];
    uint32_t point_ram[C71_PTRAM_WORDS];
    uint16_t direct[4096][0x1c];
    int direct_count;
    bool walk;
} rr_render_snapshot;
static rr_render_snapshot frame_mem;
static rr_vita_worker render_worker;
static bool pipeline_attempted, pipeline_active;
static atomic_int presented_quads;

/* Main thread only; caller has joined the previous render job. */
static void capture_frame(bool slave_active) {
#define CAPTURE(field) memcpy(frame_mem.field, g_rr.field, sizeof frame_mem.field)
    CAPTURE(poly); CAPTURE(czram); CAPTURE(mixer); CAPTURE(pal);
    CAPTURE(cgram); CAPTURE(text); CAPTURE(tilemapattr);
#undef CAPTURE
    rr_dsp_copy_pointram(frame_mem.point_ram);
    frame_mem.direct_count = rr_scene_direct_count();
    frame_mem.walk = rr_scene_frame(slave_active);
    for (int i = 0; i < frame_mem.direct_count; i++)
        memcpy(frame_mem.direct[i], rr_scene_direct(i), sizeof frame_mem.direct[i]);
    rr_scene_consume();
}

/* ---------------- GXM Platform State via vita2d ---------------- */
static SceGxmContext *gxm_context = NULL;
static SceGxmShaderPatcher *gxm_shader_patcher = NULL;
static vita2d_texture *s_txt_tex = NULL;

static void shark_log_cb(const char *msg, shark_log_level msg_level, int line) {
    VLOG("[SHARK][%d][L%d] %s\n", (int)msg_level, line, msg ? msg : "");
}

static bool init_gxm_platform(void) {
    if (gxm_ready) return true;

    VLOG("[GXM] Initializing vita2d...\n");
    int r = vita2d_init();
    if (r < 0) {
        VLOG("[GXM] vita2d_init failed: 0x%08X\n", r);
        return false;
    }
    vita2d_set_vblank_wait(0);

    gxm_context = vita2d_get_context();
    gxm_shader_patcher = vita2d_get_shader_patcher();
    if (!gxm_context || !gxm_shader_patcher) {
        VLOG("[GXM] Failed to get context or shader patcher from vita2d!\n");
        return false;
    }
    VLOG("[GXM] vita2d context=%p patcher=%p\n", gxm_context, gxm_shader_patcher);

    // Initialize vitashark runtime compiler
    int sr = shark_init(NULL);
    if (sr < 0) sr = shark_init("ur0:data/libshacccg.suprx");
    VLOG("[GXM] shark_init returned: %d\n", sr);
    if (sr < 0) {
        VLOG("[GXM] shark_init failed: %d\n", sr);
        fprintf(stderr, "[GXM] shark_init failed: %d\n", sr);
        return false;
    }
    shark_install_log_cb(shark_log_cb);
    shark_set_warnings_level(SHARK_WARN_MAX);

    // Initialize GXM texture pool & quad renderer
    if (!gxm_tex_init()) {
        VLOG("[GXM] gxm_tex_init failed!\n");
        return false;
    }
    if (!quad_gxm_init(gxm_context, gxm_shader_patcher)) {
        VLOG("[GXM] quad_gxm_init failed!\n");
        return false;
    }
    VLOG("[GXM] quad_gxm_init OK!\n");

    // Text texture buffer for overlay
    s_txt_tex = vita2d_create_empty_texture_format(NW, NH, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (s_txt_tex) {
        vita2d_texture_set_filters(s_txt_tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        VLOG("[GXM] Text overlay texture created: %p (data=%p, stride=%u)\n",
             s_txt_tex, vita2d_texture_get_datap(s_txt_tex), vita2d_texture_get_stride(s_txt_tex));
    } else {
        VLOG("[GXM] Warning: Failed to create text overlay texture!\n");
    }

    gxm_ready = true;
    return true;
}

/* ------------------------------------------------ assets */
static int32_t pointram_read(uint32_t a) {
    a &= 0xffffff;
    if (a < C71_PTRAM_S22 || a >= C71_PTRAM_S22 + C71_PTRAM_WORDS) return -1;
    return (int32_t)(frame_mem.point_ram[a - C71_PTRAM_S22] << 8) >> 8;
}

bool rr_gxm_init(const char *dir) {
    VLOG("[GXM] rr_gxm_init('%s')...\n", dir);
    if (!init_gxm_platform()) {
        VLOG("[GXM] init_gxm_platform FAILED!\n");
        return false;
    }
    VLOG("[GXM] init_gxm_platform OK!\n");

    static const char *const cg[8] = { "rv1cg0.1a", "rv1cg1.1c", "rv1cg2.1d", "rv1cg3.1e",
                                       "rv1cg4.1f", "rv1cg5.1j", "rv1cg6.1k", "rv1cg7.1n" };
    if (!eng_load_texture_roms(dir, cg, "rv1ccrl.5a", "rv1ccrh.5c")) {
        VLOG("[GXM] eng_load_texture_roms FAILED!\n");
        return false;
    }
    VLOG("[GXM] eng_load_texture_roms OK!\n");

    static const char *gp[3] = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" };
    for (int i = 0; i < 3; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, gp[i]);
        FILE *f = fopen(p, "rb");
        if (!f || fread(gamma_prom[i], 1, 256, f) != 256) {
            if (f) fclose(f);
            VLOG("[GXM] cannot read gamma prom: %s\n", p);
            return false;
        }
        fclose(f);
    }
    VLOG("[GXM] gamma proms OK!\n");

    g_eng_pointrom   = g_pointrom;
    g_eng_pointrom_n = g_pointrom_words;
    g_eng_pointram   = pointram_read;
    g_tex_opaque = 1;
    assets_ok = g_eng_pointrom != NULL;
    VLOG("[GXM] assets_ok = %d (g_pointrom=%p, words=%u)\n", assets_ok, g_pointrom, g_pointrom_words);
    return assets_ok;
}

/* ------------------------------------------------ board helpers */
static inline uint8_t mixer_b(int n) { return frame_mem.mixer[n & (RR_MIXER_SIZE - 1)]; }
static inline void pen_rgb(int pen, uint8_t o[3]) {
    pen &= 0x7fff;
    o[0] = gamma_prom[0][frame_mem.pal[pen]];
    o[1] = gamma_prom[1][frame_mem.pal[pen + 0x8000]];
    o[2] = gamma_prom[2][frame_mem.pal[pen + 0x10000]];
}

static int s22_fog_quad(const geo_quad *q, eng_fog *f)
{
    const int color = (int)((q->color >> 8) & 0xff);
    if (color & 0x80) return 0;
    const int cz_type = q->cz_type & 3;
    const int cz_color = cz_type & mixer_b(0x84 + cz_type);
    const int ff = frame_mem.czram[((cz_type << 13) | (q->cz_value & 0x1fff)) & (RR_CZRAM_SIZE - 1)];
    if (!ff) return 0;
    f->rgb[0] = mixer_b(0x100 + cz_color);
    f->rgb[1] = mixer_b(0x180 + cz_color);
    f->rgb[2] = mixer_b(0x200 + cz_color);
    f->tab = NULL;
    f->alpha_const = 0xff - ff;              /* blend(rgb, fog, 0xff - ff) */
    return 1;
}

/* ------------------------------------------------ the quads */
static geo_quad *qbuf;
static const uint32_t *draw_order;
static int       qn, qcap, qorder;
static bool packet_mode;
static int packet_enabled=-1;
static void push_direct_packet(const geo_quad *q);

static void push_quad(const geo_quad *q, void *user) {
    (void)user;
    if(packet_mode){push_direct_packet(q);return;}
    if (qn == qcap) {
        int nc = qcap ? qcap * 2 : 8192;
        geo_quad *nb = realloc(qbuf, (size_t)nc * sizeof *qbuf);
        if (!nb) return;
        qbuf = nb; qcap = nc;
    }
    qbuf[qn] = *q;
    qbuf[qn].order = qorder++;
    qn++;
}

static uint32_t poly_word(int i) { return frame_mem.poly[i & 0x7fff]; }

static void direct_quad(const uint16_t *src) {
    geo_quad q;
    memset(&q, 0, sizeof q);
    q.zsort   = (int32_t)(((uint32_t)(src[1] & 0xfff) << 12) | (src[0] & 0xfff));
    q.cmode   = (src[4] & 0xf000) >> 12;
    q.texbank = (src[5] & 0xf000) >> 12;
    q.color   = (uint32_t)(src[2] & 0xff00);
    q.cz_value = (src[3] >> 2) & 0x1fff;
    q.cz_type  = src[3] & 3;
    q.clip[0] = 0; q.clip[1] = NW - 1; q.clip[2] = 0; q.clip[3] = NH - 1;
    q.direct = 1;
    q.nrv = 4;
    int u0 = 0xfff, u1 = 0, v0 = 0xfff, v1 = 0;
    const uint16_t *s = src + 4;
    for (int i = 0; i < 4; i++, s += 6) {
        geo_vert *v = &q.rv[i];
        v->u = s[0] & 0x0fff; v->v = s[1] & 0x0fff;
        float ooz = (float)0x10000;
        if (s[5]) { ooz = (float)s[5]; for (int e = s[4] & 0x3f; e < 0x2e; e++) ooz /= 2.0f; }
        v->sx16 = (NW / 2 + (int16_t)s[2]) * 16;
        v->sy16 = (NH / 2 + (int16_t)s[3]) * 16;
        double z = ooz > 0.0f ? 1.0 / ooz : 1.0;
        v->z = z < 1.0 ? 1 : z > 2e9 ? 2000000000 : (int32_t)z;
        v->bri = s[4] >> 8;
        v->valid = 1;
        if ((int)v->u < u0) u0 = v->u;  if ((int)v->u > u1) u1 = v->u;
        if ((int)v->v < v0) v0 = v->v;  if ((int)v->v > v1) v1 = v->v;
        q.v[i] = *v;
    }
    q.uvbox[0] = (uint16_t)u0; q.uvbox[1] = (uint16_t)u1;
    q.uvbox[2] = (uint16_t)v0; q.uvbox[3] = (uint16_t)v1;
    push_quad(&q, NULL);
}

/* ------------------------------------------------ text layer */
static _Alignas(4) uint8_t txt_rgba[NW * NH * 4];
static uint8_t txt_uploaded[NW * NH * 4];
static bool txt_uploaded_valid;
static bool txt_pixels_changed;

static bool build_text(int text_palbase) {
    static uint8_t last_text[sizeof frame_mem.text], last_cgram[sizeof frame_mem.cgram];
    static uint8_t last_attr[sizeof frame_mem.tilemapattr], last_palette[768];
    static int last_base = -1;
    static bool last_any;
    uint8_t palette[768];
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++)
        palette[c * 256 + i] = frame_mem.pal[((text_palbase + i) & 0x7fff) + c * 0x8000];
    txt_pixels_changed = last_base != text_palbase ||
        memcmp(last_palette, palette, sizeof palette) ||
        memcmp(last_attr, frame_mem.tilemapattr, sizeof last_attr) ||
        memcmp(last_text, frame_mem.text, sizeof last_text) ||
        memcmp(last_cgram, frame_mem.cgram, sizeof last_cgram);
    if (!txt_pixels_changed) return last_any;
    last_base = text_palbase;
    memcpy(last_palette, palette, sizeof palette);
    memcpy(last_attr, frame_mem.tilemapattr, sizeof last_attr);
    memcpy(last_text, frame_mem.text, sizeof last_text);
    memcpy(last_cgram, frame_mem.cgram, sizeof last_cgram);
    const uint16_t a0 = (uint16_t)(frame_mem.tilemapattr[0] << 8 | frame_mem.tilemapattr[1]);
    const uint16_t a1 = (uint16_t)(frame_mem.tilemapattr[2] << 8 | frame_mem.tilemapattr[3]);
    const int sx = (a0 - 0x35c) & 0x3ff, sy = a1 & 0x3ff;
    uint32_t colors[256];
    for (int i = 0; i < 256; i++) {
        uint8_t rgba[4] = {0, 0, 0, 0};
        if ((i & 15) != 15) {
            pen_rgb(text_palbase + i, rgba);
            rgba[3] = 255;
        }
        memcpy(&colors[i], rgba, sizeof colors[i]);
    }
    bool any = false;
    for (int y = 0; y < NH; y++) {
        const int ty = (y + sy) & 0x3ff, trow = ty >> 4, cy = ty & 15;
        uint32_t *dst = (uint32_t *)(txt_rgba + (size_t)y * NW * 4);
        /* Tile attributes, row address and palette are constant for each
         * span. Decode them once per tile row, rather than once per pixel. */
        for (int x = 0; x < NW;) {
            const int tx = (x + sx) & 0x3ff;
            const int ti = (trow * 64 + (tx >> 4)) * 2;
            const uint16_t w = (uint16_t)(frame_mem.text[ti] << 8 | frame_mem.text[ti + 1]);
            const int ccy = (w & 0x800) ? 15 - cy : cy;
            const uint32_t off = (uint32_t)(w & 0x3ff) * 128 + (uint32_t)ccy * 8;
            const uint8_t *row = off < 0x1e000 ? frame_mem.cgram + off : frame_mem.text + (off - 0x1e000);
            const uint32_t *pal = colors + (w >> 12) * 16;
            int count = 16 - (tx & 15);
            if (count > NW - x) count = NW - x;
            for (int j = 0; j < count; j++) {
                int cx = (tx & 15) + j;
                if (w & 0x400) cx = 15 - cx;
                const uint8_t byte = row[cx >> 1];
                const int pix = (cx & 1) ? (byte & 15) : (byte >> 4);
                dst[x + j] = pal[pix];
                any |= pix != 15;
            }
            x += count;
        }
    }
    last_any = any;
    return any;
}

/* Compare cached pixels in small spans. A single changing digit should not
 * force the entire 640-pixel row across the slow CPU-to-CDRAM interface. */
static void upload_text(uint8_t *dst, size_t stride) {
    if (txt_pixels_changed || !txt_uploaded_valid) {
        for (int y = 0; y < NH; y++) {
            const uint8_t *src = txt_rgba + y * NW * 4;
            uint8_t *last = txt_uploaded + y * NW * 4;
            if (!txt_uploaded_valid) {
                memcpy(dst + y * stride, src, NW * 4);
                memcpy(last, src, NW * 4);
            } else if (memcmp(last, src, NW * 4)) {
                for (int x = 0; x < NW * 4; x += 64) {
                    if (!memcmp(last + x, src + x, 64)) continue;
                    memcpy(dst + y * stride + x, src + x, 64);
                    memcpy(last + x, src + x, 64);
                }
            }
        }
    }
    txt_uploaded_valid = true;
}

/* The legacy palette/HUD worker reads the immutable render snapshot while the
 * CPU/DSP can emulate the next frame. Join before palette consumers; the main
 * thread cannot overwrite the snapshot until the render job completes. */
static int frame_mixer_flags, frame_bg_palbase, frame_text_palbase;
static uint8_t s_gamma_pal[0x18000];
static SceUID s_build_thread = -1, s_build_start = -1, s_build_done = -1;
static bool s_build_pending, s_text_any;
static atomic_bool s_build_ready, s_build_complete;
static uint64_t s_build_us;

static void build_frame_layers(void) {
    uint64_t begin = sceKernelGetProcessTimeWide();
    for (size_t i = 0; i < 0x8000; i++) {
        s_gamma_pal[i]           = gamma_prom[0][frame_mem.pal[i]];
        s_gamma_pal[i + 0x8000]  = gamma_prom[1][frame_mem.pal[i + 0x8000]];
        s_gamma_pal[i + 0x10000] = gamma_prom[2][frame_mem.pal[i + 0x10000]];
    }
    eng_palette_from_planar(s_gamma_pal, 0x8000);
    if (!quad_gxm_native_hud()) s_text_any = build_text(frame_text_palbase);
    s_build_us = sceKernelGetProcessTimeWide() - begin;
}

static int frame_build_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(s_build_start, 1, NULL) < 0) return 0;
        if (!atomic_load_explicit(&s_build_ready, memory_order_acquire)) return 0;
        build_frame_layers();
        atomic_store_explicit(&s_build_complete, true, memory_order_release);
        sceKernelSignalSema(s_build_done, 1);
    }
}

static void start_frame_build(void) {
    static bool attempted;
    if (!attempted) {
        attempted = true;
        s_build_start = sceKernelCreateSema("rr_layers_start", 0, 0, 1, NULL);
        s_build_done = sceKernelCreateSema("rr_layers_done", 0, 0, 1, NULL);
        if (s_build_start >= 0 && s_build_done >= 0) {
            s_build_thread = sceKernelCreateThread("rr_frame_layers", frame_build_thread,
                pipeline_active ? 0x10000110 : 0x10000100, 64 * 1024, 0,
                pipeline_active ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
            if (s_build_thread >= 0) {
                int r = sceKernelStartThread(s_build_thread, 0, NULL);
                if (r < 0) { sceKernelDeleteThread(s_build_thread); s_build_thread = -1; }
            }
        }
        int main_affinity = 0;
        if (s_build_thread >= 0 && !pipeline_active)
            main_affinity = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        else if (s_build_thread < 0) {
            if (s_build_start >= 0) sceKernelDeleteSema(s_build_start);
            if (s_build_done >= 0) sceKernelDeleteSema(s_build_done);
            s_build_start = s_build_done = -1;
        }
        VLOG("[WORKER] palette/HUD thread=%d core=%d background=%d main affinity result=%08X\n",
             s_build_thread, pipeline_active ? 0 : 1, pipeline_active, main_affinity);
    }
    if (s_build_thread < 0) return;
    atomic_store_explicit(&s_build_complete, false, memory_order_relaxed);
    atomic_store_explicit(&s_build_ready, true, memory_order_release);
    s_build_pending = true;
    sceKernelSignalSema(s_build_start, 1);
}

static void finish_frame_build(void) {
    uint64_t begin = sceKernelGetProcessTimeWide();
    if (s_build_pending) {
        int r = sceKernelWaitSema(s_build_done, 1, NULL);
        if (r < 0 || !atomic_load_explicit(&s_build_complete, memory_order_acquire)) {
            VLOG("[WORKER] completion failed: %08X\n", r);
            abort();
        }
        s_build_pending = false;
    } else build_frame_layers();
    static uint64_t busy, wait;
    static unsigned frames;
    busy += s_build_us;
    wait += sceKernelGetProcessTimeWide() - begin;
    if (++frames == 60) {
        VLOG("[WORKER] ms/frame work=%.2f join=%.2f\n", busy / 60000.0, wait / 60000.0);
        frames = 0; busy = wait = 0;
    }
}

/* Geometry lanes use separate engine state and per-object output buffers.
 * Jobs can complete out of order; emission order is restored before sorting. */
extern unsigned g_geo_lane_frame;
void geo_lane_native_meshes(int enabled);
void geo_lane_set_view(const geo_view *view);
void geo_lane_object(int32_t code, geo_quad_cb cb, void *user);
typedef struct { unsigned first, count; int32_t zsort; } packet_span;
typedef struct { const eng_batch_vertex *vertices; unsigned count; int32_t zsort, order; } draw_packet;
static draw_packet *draw_packets;
static unsigned draw_packet_capacity;
static const eng_draw_cfg packet_cfg={.shade=1,.fog=1,.fog_before_shade=1,
    .fog_quad=s22_fog_quad,.write_prio_alpha=1,.texel_centre=1};
typedef struct {
    geo_view view;
    int32_t code;
    geo_quad *quads;
    int count, capacity;
    packet_span *spans;
    eng_batch_vertex *vertices;
    unsigned span_capacity, vertex_count, vertex_capacity;
} geometry_job;
static geometry_job direct_packets;

static void collect_packet(const geo_quad *q,geometry_job *job){
    eng_batch_vertex vertices[90];
    unsigned n=quad_gxm_compile(q,&packet_cfg,vertices,90);
    if((unsigned)job->count==job->span_capacity){
        unsigned cap=job->span_capacity?job->span_capacity*2:32;
        void *p=realloc(job->spans,cap*sizeof *job->spans);if(!p)abort();
        job->spans=p;job->span_capacity=cap;
    }
    if(job->vertex_count+n>job->vertex_capacity){
        unsigned cap=job->vertex_capacity?job->vertex_capacity*2:256;
        if(cap<job->vertex_count+n)cap=job->vertex_count+n;
        void *p=realloc(job->vertices,cap*sizeof *job->vertices);if(!p)abort();
        job->vertices=p;job->vertex_capacity=cap;
    }
    job->spans[job->count++]=(packet_span){job->vertex_count,n,q->zsort};
    if(n)memcpy(job->vertices+job->vertex_count,vertices,n*sizeof *vertices);
    job->vertex_count+=n;
}
static void push_direct_packet(const geo_quad *q){collect_packet(q,&direct_packets);}
static void append_packets(const geometry_job *job){
    if((unsigned)(qn+job->count)>draw_packet_capacity){
        unsigned cap=(unsigned)(qn+job->count)*2;
        void *p=realloc(draw_packets,cap*sizeof *draw_packets);if(!p)abort();
        draw_packets=p;draw_packet_capacity=cap;
    }
    for(int i=0;i<job->count;i++){
        packet_span span=job->spans[i];
        draw_packets[qn++]=(draw_packet){span.count?job->vertices+span.first:NULL,span.count,span.zsort,qorder++};
    }
}
static int packet_compare(const void *a,const void *b){
    const draw_packet *x=a,*y=b;
    if(x->zsort!=y->zsort)return x->zsort>y->zsort?-1:1;
    return x->order>y->order?-1:x->order<y->order?1:0;
}
static geometry_job geometry_jobs[4096];
static int geometry_count;
static atomic_int geometry_next;
static rr_vita_worker geometry_worker;
static bool geometry_attempted;
static unsigned geometry_lane_jobs[2];

static void collect_object(int32_t code, const geo_view *view, void *user) {
    (void)user;
    if (geometry_count == 4096) abort(); /* same bound as the list walker */
    geometry_job *job = &geometry_jobs[geometry_count++];
    job->code = code; job->view = *view; job->count = 0; job->vertex_count=0;
}
static void collect_polygon(const geo_quad *q, void *user) {
    geometry_job *job = user;
    if(packet_mode){collect_packet(q,job);return;}
    if (job->count == job->capacity) {
        int next = job->capacity ? job->capacity * 2 : 32;
        geo_quad *buf = realloc(job->quads, (size_t)next * sizeof *buf);
        if (!buf) abort();
        job->quads = buf; job->capacity = next;
    }
    job->quads[job->count++] = *q;
}
static void geometry_run(void *arg) {
    int lane = (int)(uintptr_t)arg;
    if (lane) g_geo_lane_frame = g_eng_frame;
    geometry_lane_jobs[lane] = 0;
    for (;;) {
        int i = atomic_fetch_add_explicit(&geometry_next, 1, memory_order_relaxed);
        if (i >= geometry_count) return;
        geometry_job *job = &geometry_jobs[i];
        if (lane) {
            geo_lane_set_view(&job->view);
            geo_lane_object(job->code, collect_polygon, job);
        } else {
            geo_hw_set_view(&job->view);
            geo_hw_object(job->code, collect_polygon, job);
        }
        geometry_lane_jobs[lane]++;
    }
}
static void build_geometry(const eng_list_cfg *cfg) {
    if (!geometry_attempted) {
        geometry_attempted = true;
        rr_worker_init(&geometry_worker, "rr_geometry", SCE_KERNEL_CPU_MASK_USER_2);
        VLOG("[GEO_WORKER] thread=%d affinity=USER_2\n", geometry_worker.thread);
    }
    geometry_count = 0;
    eng_walk_objects(poly_word, cfg, collect_object, NULL);
    atomic_store_explicit(&geometry_next, 0, memory_order_relaxed);
    if (geometry_worker.thread >= 0)
        rr_worker_dispatch(&geometry_worker, geometry_run, (void *)(uintptr_t)1);
    else geometry_lane_jobs[1] = 0;
    geometry_run(NULL);
    rr_worker_join(&geometry_worker);
    for (int i = 0; i < geometry_count; i++) {
        if(packet_mode)append_packets(&geometry_jobs[i]);
        else for (int j = 0; j < geometry_jobs[i].count; j++)
            push_quad(&geometry_jobs[i].quads[j], NULL);
    }
    if (g_eng_frame % 60 == 0)
        VLOG("[GEO_WORKER] objects=%d main=%u worker=%u\n", geometry_count,
             geometry_lane_jobs[0], geometry_lane_jobs[1]);
}

/* ------------------------------------------------ the frame */
static void prepare_snapshot(void) {
    if (!assets_ok) return;
    g_eng_frame++;
    frame_mixer_flags  = mixer_b(0x00) << 8 | mixer_b(0x01);
    frame_bg_palbase   = mixer_b(0x04) << 8 & 0x7f00;
    frame_text_palbase = mixer_b(0x07) << 8 & 0x7f00;

    uint64_t prep_start = sceKernelGetProcessTimeWide();
    if (gxm_ready) {
        if (quad_gxm_native_hud()) build_frame_layers();
        else start_frame_build();
    }
    int direct_count = frame_mem.direct_count;
    const bool walk = frame_mem.walk;
    if(packet_enabled<0){
        FILE *f=fopen("ux0:/data/raverace_geometry_legacy.enable","rb");
        packet_enabled=!f;if(f)fclose(f);
        geo_hw_native_meshes(packet_enabled);geo_lane_native_meshes(packet_enabled);
        VLOG("[GEOMETRY] worker packets=%d indexed meshes=%d\n",packet_enabled,packet_enabled);
    }
    packet_mode=packet_enabled && quad_gxm_packets_ready() && quad_gxm_native_hud();
    const double aspect=960.0/544.0;
    const float extent=(float)((NH*aspect-NW)/2.0);
    quad_gxm_set_scene_extents(-extent,NW+extent);
    qn = 0; qorder = 0;
    direct_packets.count=0;direct_packets.vertex_count=0;
    for (int i = 0; i < direct_count; i++) direct_quad(frame_mem.direct[i]);
    if(packet_mode)append_packets(&direct_packets);
    uint64_t walk_start = sceKernelGetProcessTimeWide();
    if (walk) {
        eng_list_cfg cfg = { ENG_LIST_HEAD_S22, 1, NULL, NULL, NULL, NULL };
        build_geometry(&cfg);
    }
    uint64_t sort_start = sceKernelGetProcessTimeWide();
    if(packet_mode)qsort(draw_packets,qn,sizeof *draw_packets,packet_compare);
    else {
        draw_order = quad_gxm_sort_indices(qbuf, qn, 0);
        if (!draw_order) eng_quad_sort(qbuf, qn, 0);
    }
    uint64_t prep_end = sceKernelGetProcessTimeWide();
    static uint64_t direct_total, walk_total, sort_total;
    direct_total += walk_start - prep_start;
    walk_total += sort_start - walk_start;
    sort_total += prep_end - sort_start;

    static int s_prep_cnt = 0;
    if (++s_prep_cnt % 60 == 0) {
        VLOG("[PREP] ms/frame direct=%.2f geometry=%.2f sort=%.2f\n",
             direct_total / 60000.0, walk_total / 60000.0, sort_total / 60000.0);
        direct_total = walk_total = sort_total = 0;
        VLOG("[GXM_PREP] frame=%u slave=%d walk=%d direct=%d qn=%d\n",
             g_eng_frame, walk, walk, direct_count, qn);
    }
}

int rr_gxm_quads(void) { return atomic_load_explicit(&presented_quads, memory_order_relaxed); }

/* Mutable GPU resources are owned by a slot until its fragment notification.
 * Wrap vita2d's EndScene to retain its own drawing/display bookkeeping. */
static SceGxmNotification frame_fence[3];
static bool frame_fence_pending[3], frame_fence_ready;
static unsigned frame_slot, frame_sequence;
static int buffered_mode=-1;
#include "rr_present_trace.inc"
int __real_sceGxmEndScene(SceGxmContext *,const SceGxmNotification *,const SceGxmNotification *);
int __wrap_sceGxmEndScene(SceGxmContext *context,const SceGxmNotification *vertex,
                        const SceGxmNotification *fragment) {
    if(context!=gxm_context || !frame_fence_ready || fragment)
        return __real_sceGxmEndScene(context,vertex,fragment);
    frame_fence[frame_slot].value=++frame_sequence;
    int r=__real_sceGxmEndScene(context,vertex,&frame_fence[frame_slot]);
    if(r<0){VLOG("[GPU_FENCE] EndScene failed %08X\n",r);abort();}
    frame_fence_pending[frame_slot]=true;
    present_scene_end=sceKernelGetProcessTimeWide();
    return r;
}
static void acquire_frame_resources(void) {
    if(buffered_mode<0){
        FILE *f=fopen("ux0:/data/raverace_gpu_serial.enable","rb");
        buffered_mode=!f;if(f)fclose(f);
        volatile unsigned int *region=sceGxmGetNotificationRegion();
        if(region){
            for(unsigned i=0;i<3;i++){frame_fence[i].address=region+i;region[i]=0;}
            frame_fence_ready=true;
        }
        VLOG("[GPU_FENCE] buffered=%d notifications=%d\n",buffered_mode,frame_fence_ready);
    }
    if(buffered_mode && frame_fence_ready && quad_gxm_buffered()) {
        frame_slot=(frame_slot+1)%3;
        if(frame_fence_pending[frame_slot]){
            int r=sceGxmNotificationWait(&frame_fence[frame_slot]);
            if(r<0){VLOG("[GPU_FENCE] wait failed %08X\n",r);abort();}
            frame_fence_pending[frame_slot]=false;
        }
    } else {
        vita2d_wait_rendering_done();
        memset(frame_fence_pending,0,sizeof frame_fence_pending);
        frame_slot=0;
    }
    quad_gxm_select_frame(frame_slot);
}

static void draw_snapshot(int vw, int vh) {
    (void)vw; (void)vh;
    if (!assets_ok || !gxm_ready) return;

    uint64_t start_us = sceKernelGetProcessTimeWide();
    /* Acquire the selected resource slot; fallback rendering drains all reads. */
    acquire_frame_resources();
    uint64_t waited_us = sceKernelGetProcessTimeWide();
    gxm_tex_begin_frame();
    /* Hor+ widescreen aspect on 960x544 Vita screen */
    const double aspect = 960.0 / 544.0;
    const float E = (float)((NH * aspect - NW) / 2.0);
    quad_gxm_set_scene_extents(-E, NW + E);

    if (!quad_gxm_native_hud()) finish_frame_build();
    if (quad_gxm_native_hud()) {
        uint8_t colors[1024];
        for(int i=0;i<256;i++){pen_rgb(frame_text_palbase+i,colors+i*4);colors[i*4+3]=255;}
        unsigned sx=((frame_mem.tilemapattr[0]<<8 | frame_mem.tilemapattr[1])-0x35c)&1023;
        unsigned sy=(frame_mem.tilemapattr[2]<<8 | frame_mem.tilemapattr[3])&1023;
        quad_gxm_upload_hud(frame_mem.cgram,frame_mem.text,colors,sx,sy);
    }

    uint64_t palette_us = sceKernelGetProcessTimeWide();
    if(packet_mode){
        quad_gxm_begin_bank_plan();
        for(int i=0;i<qn;i++)quad_gxm_add_bank_plan(draw_packets[i].vertices,draw_packets[i].count);
        quad_gxm_finish_bank_plan();
    }
    /* Begin GXM scene via vita2d */
    vita2d_start_drawing();
    uint64_t scene_us = sceKernelGetProcessTimeWide();

    /* Clear background color */
    uint8_t bg[3]; pen_rgb(frame_bg_palbase | 0xff, bg);
    vita2d_set_clear_color(RGBA8(bg[0], bg[1], bg[2], 0));
    vita2d_clear_screen();

    static int s_draw_cnt = 0;
    if (++s_draw_cnt % 60 == 0) {
        VLOG("[GXM_DRAW] frame=%u qn=%d bg=(%u,%u,%u)\n",
             g_eng_frame, qn, bg[0], bg[1], bg[2]);
    }

    /* Draw scene quads */
    eng_draw_cfg dc;
    memset(&dc, 0, sizeof dc);
    dc.shade = 1;
    dc.fog = 1;
    dc.fog_before_shade = 1;
    dc.fog_quad = s22_fog_quad;
    dc.write_prio_alpha = 1;
    dc.texel_centre = 1;

    eng_draw_begin();
    for (int i = 0; i < qn; i++) {
        if(packet_mode)quad_gxm_append_packet(draw_packets[i].vertices,draw_packets[i].count);
        else eng_draw_quad(&qbuf[draw_order ? draw_order[i] : i], &dc);
    }
    eng_draw_end();

    uint64_t quads_us = sceKernelGetProcessTimeWide();
    /* Draw text layer */
    if (quad_gxm_native_hud()) quad_gxm_draw_native_hud();
    else if (s_text_any && s_txt_tex) {
        void *dst = vita2d_texture_get_datap(s_txt_tex);
        if (dst) {
            uint32_t stride = vita2d_texture_get_stride(s_txt_tex);
            upload_text(dst, stride);
            quad_gxm_draw_text(&s_txt_tex->gxm_tex);
        }
    }

    uint64_t hud_us = sceKernelGetProcessTimeWide();
    /* End GXM scene via vita2d */
    vita2d_end_drawing();
    uint64_t end_us = sceKernelGetProcessTimeWide();
    static uint64_t palette_total, quads_total, text_total, gpu_total, layer_total, hud_total, end_total, begin_total, triangles_total;
    static unsigned samples;
    gpu_total += waited_us-start_us; layer_total += palette_us-waited_us;
    hud_total += hud_us-quads_us; end_total += end_us-hud_us;
    begin_total += scene_us-palette_us; triangles_total += quads_us-scene_us;
    palette_total += palette_us - start_us;
    quads_total += quads_us - palette_us;
    text_total += end_us - quads_us;
    if (++samples == 60) {
        VLOG("[RENDER] ms/frame palette=%.2f quads=%.2f text=%.2f cache_hits=%d misses=%d\n",
             palette_total / 60000.0, quads_total / 60000.0, text_total / 60000.0,
             tex_frame_hits, tex_frame_misses);
        VLOG("[SYNC] ms/frame gpu_wait=%.2f layers_upload=%.2f hud_draw=%.2f scene_end=%.2f\n",
             gpu_total/60000.0,layer_total/60000.0,hud_total/60000.0,end_total/60000.0);
        VLOG("[SCENE] ms/frame begin=%.2f triangles=%.2f\n",begin_total/60000.0,triangles_total/60000.0);
        begin_total=triangles_total=0;
        gpu_total=layer_total=hud_total=end_total=0;
        samples = 0;
        palette_total = quads_total = text_total = 0;
        tex_frame_hits = tex_frame_misses = 0;
    }
}

static void swap_snapshot(void) {
    if (!gxm_ready) return;
    vita2d_swap_buffers();
    /* Optional diagnostic capture, requested by creating this marker file. */
    if (g_eng_frame % 8 == 0) {
        const char *request = "ux0:/data/raverace_capture.request";
        FILE *f = fopen(request, "rb");
        if (f) {
            fclose(f);
            remove(request);
            vita2d_wait_rendering_done();
            SceDisplayFrameBuf fb = {0};
            fb.size = sizeof fb;
            if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) == 0 && fb.base) {
                f = fopen("ux0:/data/raverace_capture.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
                    uint8_t row[DISPLAY_WIDTH * 3];
                    for (unsigned y = 0; y < fb.height && fb.width <= DISPLAY_WIDTH; y++) {
                        const uint8_t *src = (const uint8_t *)fb.base + y * fb.pitch * 4;
                        for (unsigned x = 0; x < fb.width; x++)
                            memcpy(row + x * 3, src + x * 4, 3);
                        fwrite(row, 3, fb.width, f);
                    }
                    fclose(f);
                    VLOG("[CAPTURE] saved frame %u\n", g_eng_frame);
                }
            }
        }
    }
}

static void render_frame_job(void *arg) {
    (void)arg;
    uint64_t start = sceKernelGetProcessTimeWide();
    prepare_snapshot();
    uint64_t prepared = sceKernelGetProcessTimeWide();
    draw_snapshot(960, 544);
    uint64_t drawn = sceKernelGetProcessTimeWide();
    swap_snapshot();
    uint64_t swapped = sceKernelGetProcessTimeWide();
    atomic_store_explicit(&presented_quads, qn, memory_order_relaxed);
    static uint64_t work, prepare_work, draw_work, swap_work;
    static unsigned samples;
    work += sceKernelGetProcessTimeWide() - start;
    prepare_work += prepared-start; draw_work += drawn-prepared; swap_work += swapped-drawn;
    if (++samples == 60) {
        VLOG("[PIPELINE] render work=%.2f ms/frame core=%d\n", work/60000.0, sceKernelGetCpuId());
        VLOG("[RENDER_STAGE] frame=%u prepare=%.2f draw=%.2f present=%.2f\n",g_eng_frame,
            prepare_work/60000.0,draw_work/60000.0,swap_work/60000.0);
        prepare_work=draw_work=swap_work=0;
        samples=0; work=0;
    }
}

/* The main thread joins this job immediately: simulation and sound execution
 * remain paused while an immutable race snapshot is replayed on USER_1.
 * Warm caches for 60 iterations, then time 180 completed presentations. */
static void replay_snapshot_job(void *arg) {
    (void)arg;
    unsigned saved_frame = g_eng_frame;
    unsigned source_frame = saved_frame + 1;
    unsigned long before = crc32(0, (const Bytef *)&frame_mem, sizeof frame_mem);
    for (int cached = 0; cached < 2; cached++) {
        uint64_t begin = 0, prep = 0, draw = 0, present = 0;
        for (unsigned i = 0; i < 240; i++) {
            if (i == 60) {
                vita2d_wait_rendering_done(); sceGxmDisplayQueueFinish();
                begin = sceKernelGetProcessTimeWide();
            }
            uint64_t a = sceKernelGetProcessTimeWide();
            if (!cached) prepare_snapshot(); else g_eng_frame++;
            uint64_t b = sceKernelGetProcessTimeWide();
            draw_snapshot(960, 544);
            uint64_t c = sceKernelGetProcessTimeWide();
            swap_snapshot();
            uint64_t d = sceKernelGetProcessTimeWide();
            if (i >= 60) { prep += b-a; draw += c-b; present += d-c; }
        }
        vita2d_wait_rendering_done(); sceGxmDisplayQueueFinish();
        uint64_t end = sceKernelGetProcessTimeWide();
        unsigned long after = crc32(0, (const Bytef *)&frame_mem, sizeof frame_mem);
        VLOG("[REPLAY_RESULT] source=%u cached_geometry=%d frames=180 total_ms=%.3f prepare_ms=%.3f draw_ms=%.3f present_ms=%.3f quads=%d snapshot=%08lx unchanged=%d\n",
             source_frame, cached, (end-begin)/180000.0, prep/180000.0,
             draw/180000.0, present/180000.0, qn, before, before==after);
        if (before != after) abort();
    }
    g_eng_frame = saved_frame;
}

void rr_gxm_prepare(bool slave_active) {
    if (!assets_ok) return;
    if (rr_vita_isolation == 1) {
        if (!pipeline_attempted) {
            pipeline_attempted = true;
            int affinity = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
            VLOG("[ISOLATION] simulation main=USER_0 affinity_result=%d\n", affinity);
        }
        /* Keep the guest-visible display-list lifecycle without rendering. */
        rr_scene_frame(slave_active); rr_scene_consume(); g_eng_frame++;
        return;
    }
    if (!pipeline_attempted) {
        pipeline_attempted = true;
        /* A marker provides a serial A/B path without another install. */
        FILE *disabled = fopen("ux0:/data/raverace_pipeline.disable", "rb");
        if (disabled) fclose(disabled);
        else pipeline_active = rr_worker_init(&render_worker, "rr_render", SCE_KERNEL_CPU_MASK_USER_1);
        if (pipeline_active)
            sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        VLOG("[PIPELINE] snapshot renderer enabled=%d main=USER_0 render=USER_1\n", pipeline_active);
    }
    uint64_t start = sceKernelGetProcessTimeWide();
    if (pipeline_active) rr_worker_join(&render_worker);
    uint64_t joined = sceKernelGetProcessTimeWide();
    capture_frame(slave_active);
    uint64_t captured = sceKernelGetProcessTimeWide();
    if (rr_vita_isolation == 2 &&
        (g_eng_frame + 1 == 600 || g_eng_frame + 1 == 1200 || g_eng_frame + 1 == 1740)) {
        VLOG("[REPLAY_BEGIN] source=%u\n", g_eng_frame+1);
        if (pipeline_active) {
            rr_worker_dispatch(&render_worker, replay_snapshot_job, NULL);
            rr_worker_join(&render_worker);
        } else replay_snapshot_job(NULL);
    }
    if (pipeline_active) rr_worker_dispatch(&render_worker, render_frame_job, NULL);
    else prepare_snapshot();
    static uint64_t wait, copy;
    static unsigned frames;
    wait += joined-start; copy += captured-joined;
    if (++frames == 60) {
        VLOG("[PIPELINE] main wait=%.2f snapshot=%.2f ms/frame\n", wait/60000.0, copy/60000.0);
        frames=0; wait=copy=0;
    }
}

/* In the pipelined mode only the renderer thread touches GXM after init. */
void rr_gxm_draw(int vw, int vh) { if (rr_vita_isolation != 1 && !pipeline_active) draw_snapshot(vw,vh); }
void rr_gxm_swap(void) {
    if (rr_vita_isolation == 1) return;
    if (!pipeline_active) {
        swap_snapshot();
        atomic_store_explicit(&presented_quads, qn, memory_order_relaxed);
    }
}

void rr_gxm_finish(void) {
    if (pipeline_active) { rr_worker_close(&render_worker); pipeline_active=false; }
    if (geometry_attempted) rr_worker_close(&geometry_worker);
    if (s_build_pending) finish_frame_build();
    if (s_build_thread >= 0) {
        atomic_store_explicit(&s_build_ready, false, memory_order_release);
        sceKernelSignalSema(s_build_start, 1);
        sceKernelWaitThreadEnd(s_build_thread, NULL, NULL);
        sceKernelDeleteThread(s_build_thread);
        sceKernelDeleteSema(s_build_start);
        sceKernelDeleteSema(s_build_done);
        s_build_thread = s_build_start = s_build_done = -1;
    }
    vita2d_wait_rendering_done();
    /* Fragment completion does not guarantee the display callback returned. */
    int result=sceGxmDisplayQueueFinish();
    if(result<0)VLOG("[GPU_FENCE] display queue shutdown failed %08X\n",result);
}
