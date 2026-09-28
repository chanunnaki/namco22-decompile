/*
 * quad_gxm.h -- GXM polygon rasteriser for Namco System 22 on PS Vita.
 * Native sceGxm implementation replacing quad_gl.c.
 */
#ifndef ENG_QUAD_GXM_H
#define ENG_QUAD_GXM_H

#include <stdint.h>
#include <stdbool.h>
#include <psp2/gxm.h>
#include "geo_hw.h"

#include "quad_packet.h"

#define ENG_SCREEN_W 640
#define ENG_SCREEN_H 480

extern float g_scene_x0, g_scene_x1;

typedef struct {
    uint8_t        rgb[3];
    const uint8_t *tab;
    int            sdelta;
    int            alpha_const;
} eng_fog;

int eng_fog_alpha(const eng_fog *f, int32_t z);

typedef struct {
    int  shade;              /* per-vertex brightness (bri/64) */
    int  fog;                /* run the fog stage */
    int  fog_before_shade;   /* System 22: shade(fog(texel)); Super 22: fog(shade(texel)) */
    int  flat_white;         /* coverage probe: every quad flat white */
    int  write_prio_alpha;   /* write priority bit to alpha */
    int  texel_centre;       /* sample (u + 0.5, v + 0.5) */
    int  (*fog_quad)(const geo_quad *q, eng_fog *f);
    void (*fade_rgb)(float *r, float *g, float *b);
} eng_draw_cfg;

/* Sort far to near */
void eng_quad_sort(geo_quad *buf, int n, int tie_emit);

/* GXM quad rasterizer interface */
bool quad_gxm_init(SceGxmContext *ctx, SceGxmShaderPatcher *patcher);
void quad_gxm_shutdown(void);
void quad_gxm_set_scene_extents(float x0, float x1);

void eng_draw_begin(void);
void eng_draw_quad(const geo_quad *q, const eng_draw_cfg *cfg);
void eng_draw_end(void);
bool quad_gxm_packets_ready(void);
void quad_gxm_begin_bank_plan(void);
void quad_gxm_add_bank_plan(const eng_batch_vertex *vertices,unsigned count);
void quad_gxm_finish_bank_plan(void);
unsigned quad_gxm_compile(const geo_quad *q, const eng_draw_cfg *cfg,
                          eng_batch_vertex *out, unsigned capacity);
void quad_gxm_append_packet(const eng_batch_vertex *vertices, unsigned count);
void quad_gxm_draw_text(const SceGxmTexture *tex);
bool quad_gxm_native_hud(void);
bool quad_gxm_buffered(void);
void quad_gxm_select_frame(unsigned slot);
void quad_gxm_upload_hud(const uint8_t *chars, const uint8_t *map,
                         const uint8_t *rgba, unsigned sx, unsigned sy);
void quad_gxm_draw_native_hud(void);

extern double g_perf_bake, g_perf_gl, g_perf_clip;
extern int    g_bri_min, g_bri_max, g_fogged_quads, g_fogA_min, g_fogA_max;
extern int    g_tex_clipbox;
extern int    g_eng_degen_uv_legacy;
extern int    g_perf_enabled;
double eng_now(void);

/* Valid until the next sort; NULL permits full-struct sorting fallback. */
uint32_t *quad_gxm_sort_indices(const geo_quad *buf, int n, int tie_emit);

#endif /* ENG_QUAD_GXM_H */
