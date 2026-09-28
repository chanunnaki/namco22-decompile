#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "gxm_tex.h"
#include "tex_bake.h"

#define TEX_POOL_SIZE (64 * 1024 * 1024)

static SceUID pool_uid = -1;
static uint8_t *pool_base = NULL;
static size_t pool_offset = 0;
#define UNIT 256
/* 0 = free, 1 = live, 2 = retired but possibly still read by the GPU. */
static uint8_t units[TEX_POOL_SIZE / UNIT];
static size_t pool_size, next_unit = 1;
static uint32_t next_slot = 1;
static size_t retired_first = TEX_POOL_SIZE / UNIT, retired_end;

static void retire(GxmTextureSlot *s) {
    if (s->data) {
        memset(units + s->first_unit, 2, s->units);
        if (s->first_unit < retired_first) retired_first = s->first_unit;
        if (s->first_unit + s->units > retired_end) retired_end = s->first_unit + s->units;
    }
    s->data = NULL;
    s->units = 0;
}

void gxm_tex_begin_frame(void) {
    /* Caller has completed the previous scene: retired storage is now safe. */
    for (size_t i = retired_first; i < retired_end; i++)
        if (units[i] == 2) units[i] = 0;
    retired_first = TEX_POOL_SIZE / UNIT;
    retired_end = 0;
}

static size_t alloc_units(size_t count) {
    size_t total = pool_size / UNIT;
    for (int pass = 0; pass < 2; pass++) {
        size_t begin = pass ? 1 : next_unit;
        size_t end = pass ? next_unit : total;
        size_t run = 0;
        for (size_t i = begin; i < end; i++) {
            run = units[i] ? 0 : run + 1;
            if (run == count) {
                size_t start = i + 1 - count;
                memset(units + start, 1, count);
                next_unit = i + 1;
                if (next_unit >= total) next_unit = 1;
                return start;
            }
        }
    }
    return 0;
}

static GxmTextureSlot slots[MAX_GXM_TEXTURES];
uint32_t gxm_tex_current = 0;

static SceGxmTexture white_tex;
static bool white_init = false;

static inline size_t align256(size_t v) {
    return (v + 255) & ~255;
}

bool gxm_tex_init(void) {
    if (pool_base != NULL) return true;

    // Keep the race working set resident; fall back on lower-memory configurations.
    size_t sizes[] = { 64 * 1024 * 1024, 48 * 1024 * 1024, 32 * 1024 * 1024, 24 * 1024 * 1024, 16 * 1024 * 1024 };
    size_t actual_pool_size = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        pool_uid = sceKernelAllocMemBlock("gxm_tex_pool", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sizes[i], NULL);
        if (pool_uid >= 0) {
            actual_pool_size = sizes[i];
            break;
        }
    }
    if (pool_uid < 0) {
        // Fall back to main uncached RAM
        actual_pool_size = 16 * 1024 * 1024;
        pool_uid = sceKernelAllocMemBlock("gxm_tex_pool_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, actual_pool_size, NULL);
    }
    if (pool_uid < 0) {
        fprintf(stderr, "[GXM_TEX] Failed to allocate texture pool memory block\n");
        return false;
    }

    void *base = NULL;
    sceKernelGetMemBlockBase(pool_uid, &base);
    int r = sceGxmMapMemory(base, actual_pool_size, SCE_GXM_MEMORY_ATTRIB_RW);
    if (r != 0) {
        fprintf(stderr, "[GXM_TEX] sceGxmMapMemory failed: 0x%08X\n", r);
        sceKernelFreeMemBlock(pool_uid);
        pool_uid = -1;
        return false;
    }

    pool_size = actual_pool_size;
    tex_cache_budget = actual_pool_size * 3 / 4;
    extern void vita_log(const char *, ...);
    vita_log("[GXM_TEX] pool=%u MB cache=%u MB\n", (unsigned)(pool_size >> 20), (unsigned)(tex_cache_budget >> 20));
    memset(units, 0, sizeof units);
    retired_first = TEX_POOL_SIZE / UNIT; retired_end = 0;
    units[0] = 1; /* permanently reserve the white texture */
    pool_base = (uint8_t *)base;
    pool_offset = 0;
    memset(slots, 0, sizeof(slots));

    // Initialize 8x8 white texture in GPU mapped memory
    uint32_t *wp = (uint32_t *)(pool_base + pool_offset);
    pool_offset += 256;
    for (int i = 0; i < 64; i++) wp[i] = 0xFFFFFFFFu;
    sceGxmTextureInitLinear(&white_tex, wp, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 1);
    sceGxmTextureSetUAddrMode(&white_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&white_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetMinFilter(&white_tex, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&white_tex, SCE_GXM_TEXTURE_FILTER_POINT);
    white_init = true;

    return true;
}

void gxm_tex_shutdown(void) {
    if (pool_uid >= 0) {
        if (pool_base) {
            sceGxmUnmapMemory(pool_base);
            pool_base = NULL;
        }
        sceKernelFreeMemBlock(pool_uid);
        pool_uid = -1;
    }
    pool_offset = 0;
}

uint32_t gxm_tex_alloc(void) {
    for (uint32_t n = 1; n < MAX_GXM_TEXTURES; n++) {
        uint32_t i = next_slot++;
        if (next_slot == MAX_GXM_TEXTURES) next_slot = 1;
        if (!slots[i].in_use) {
            memset(&slots[i], 0, sizeof slots[i]);
            slots[i].in_use = true;
            return i;
        }
    }
    return 0; /* never steal a live cache entry's handle */
}

void gxm_tex_bind(uint32_t id) {
    if (id < MAX_GXM_TEXTURES) {
        gxm_tex_current = id;
    }
}

void gxm_tex_realloc(uint32_t id, int w, int h) {
    if (!id || id >= MAX_GXM_TEXTURES || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    GxmTextureSlot *s = &slots[id];
    /* Always orphan: earlier draws in this scene can still reference old data. */
    retire(s);
    size_t stride = ((size_t)w + 7) & ~(size_t)7;
    size_t count = align256(stride * h * 4) / UNIT;
    size_t at = alloc_units(count);
    if (!at) {
        static unsigned failures;
        if (++failures <= 4) {
            extern void vita_log(const char *, ...);
            vita_log("[GXM_TEX] pool exhausted (%ux%u, capacity=%u); skipping texture\n",
                     w, h, (unsigned)pool_size);
        }
        return;
    }
    s->first_unit = at; s->units = count;
    s->data = pool_base + at * UNIT;
    s->alloc_w = stride; s->alloc_h = h;
    s->width = w; s->height = h;
}

bool gxm_tex_valid(uint32_t id) {
    return id && id < MAX_GXM_TEXTURES && slots[id].in_use && slots[id].data;
}

void gxm_tex_upload(uint32_t id, int x, int y, int w, int h, const void *pixels) {
    if (id == 0 || id >= MAX_GXM_TEXTURES) return;
    GxmTextureSlot *s = &slots[id];
    if (!s->data) return;

    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t *dst = (uint8_t *)s->data;
    uint32_t stride = s->alloc_w * 4;
    if (x < 0 || y < 0 || w < 0 || h < 0 || (unsigned)(x+w) > s->width || (unsigned)(y+h) > s->height) return;

    for (int row = 0; row < h; row++) {
        memcpy(dst + ((y + row) * stride + x * 4), src + row * w * 4, w * 4);
    }

    sceGxmTextureInitLinear(&s->gxm_tex, s->data, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, s->width, s->height, 1);
    sceGxmTextureSetUAddrMode(&s->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&s->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetMinFilter(&s->gxm_tex, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&s->gxm_tex, SCE_GXM_TEXTURE_FILTER_POINT);
}

void gxm_tex_free(uint32_t id) {
    if (id < MAX_GXM_TEXTURES) {
        retire(&slots[id]);
        slots[id].in_use = false;
    }
}

SceGxmTexture *gxm_tex_get(uint32_t id) {
    if (id > 0 && id < MAX_GXM_TEXTURES && slots[id].in_use && slots[id].data) {
        return &slots[id].gxm_tex;
    }
    return gxm_tex_white();
}

SceGxmTexture *gxm_tex_white(void) {
    return &white_tex;
}
