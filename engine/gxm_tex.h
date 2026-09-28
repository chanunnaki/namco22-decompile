#ifndef GXM_TEX_H
#define GXM_TEX_H

#include <stdint.h>
#include <stdbool.h>
#include <psp2/gxm.h>

#define MAX_GXM_TEXTURES 8192

typedef struct {
    void *data;
    uint32_t alloc_w, alloc_h;
    uint32_t width, height;
    uint32_t first_unit, units;
    SceGxmTexture gxm_tex;
    bool in_use;
} GxmTextureSlot;

bool gxm_tex_init(void);
void gxm_tex_shutdown(void);
void gxm_tex_begin_frame(void); /* after waiting for prior GPU work */
bool gxm_tex_valid(uint32_t id);

uint32_t gxm_tex_alloc(void);
void gxm_tex_bind(uint32_t id);
void gxm_tex_realloc(uint32_t id, int w, int h);
void gxm_tex_upload(uint32_t id, int x, int y, int w, int h, const void *pixels);
void gxm_tex_free(uint32_t id);

SceGxmTexture *gxm_tex_get(uint32_t id);
SceGxmTexture *gxm_tex_white(void);

extern uint32_t gxm_tex_current;

#endif /* GXM_TEX_H */
