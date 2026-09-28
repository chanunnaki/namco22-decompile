/* Included by quad_gxm.c: ROM-backed textures and ordered triangle batching.
 * Tilemap and pen ROM are immutable; only the small palette changes per frame.
 * Retain the baked renderer if initialization fails or the disable marker exists. */
#define BATCH_VERTICES 65532
#define BATCH_ROM_BYTES (16u * 1024 * 1024)
#define BATCH_MAP_BYTES (4u * 1024 * 1024)
#define BATCH_PAL_BYTES (256u * 128 * 4)
typedef eng_batch_vertex BatchVertex;
static bool batch_active, batch_attempted;
static SceUID batch_uid = -1;
static void *batch_memory;
static bool batch_mapped, batch_released_cache;
static uint64_t batch_started;
static BatchVertex *batch_cpu, *batch_gpu;
static uint16_t *batch_indices;
static uint8_t *batch_palette, *batch_frames;
static size_t batch_frame_stride;
static SceGxmTexture batch_rom_tex, batch_map_tex, batch_pal_tex;
static SceGxmVertexProgram *batch_vp;
static SceGxmFragmentProgram *batch_fp;
static const SceGxmProgramParameter *batch_screen;
static unsigned batch_count;
static uint16_t batch_palette_gen[3][128];
static bool batch_palette_valid[3];
static unsigned batch_sampler[3];

static const char batch_v_src[] =
 "void main(float2 aPosition, float3 aUV, float4 aShade, float4 aDecode, float4 aFog, float aTextured,\n"
 " uniform float4 uScreen, out float4 position:POSITION,\n"
 " out float3 uv:TEXCOORD0, out float4 shade:TEXCOORD1,\n"
 " out float4 decode:TEXCOORD2, out float4 fog:TEXCOORD3, out float textured:TEXCOORD4) {\n"
 " position=float4(aPosition.x*uScreen.x+uScreen.z,aPosition.y*uScreen.y+uScreen.w,0.5,1.0);\n"
 " uv=aUV; shade=aShade; decode=aDecode; fog=aFog; textured=aTextured;\n"
 "}\n";
static const char batch_f_src[] =
 "float4 main(float3 uv:TEXCOORD0, float4 shade:TEXCOORD1, float4 decode:TEXCOORD2,\n"
 " float4 fog:TEXCOORD3, float textured:TEXCOORD4,\n"
 " uniform sampler2D uMap, uniform sampler2D uROM, uniform sampler2D uPalette) {\n"
 " decode=floor(decode+0.5);\n"
 " float3 rgb=float3(1.0,1.0,1.0);\n"
 " if(textured>0.5) {\n"
 "  float2 p=floor(uv.xy/uv.z); p=p-floor(p/4096.0)*4096.0;\n"
 "  float2 tile=floor(p/16.0); float2 local=p-tile*16.0;\n"
 "  float3 map=floor(tex2D(uMap,float2((tile.x+0.5)/256.0,(tile.y+decode.x*256.0+0.5)/4096.0)).rgb*255.0+0.5);\n"
 "  float attr=map.z;\n"
 "  if(fmod(floor(attr/4.0),2.0)>0.5) local.x=15.0-local.x;\n"
 "  if(fmod(floor(attr/2.0),2.0)>0.5) local.y=15.0-local.y;\n"
 "  if(attr>=8.0) local=local.yx;\n"
 "  float2 romuv=float2(map.x*16.0+local.x,map.y*16.0+local.y);\n"
 "  float pen=floor(tex2D(uROM,(romuv+0.5)/4096.0).r*255.0+0.5);\n"
 "  if(fmod(attr,2.0)>0.5) pen=0.0;\n"
 "  pen=fmod(floor(pen/decode.z),decode.w);\n"
 "  float base=decode.y;\n"
 "  rgb=tex2D(uPalette,float2((fmod(base,256.0)+pen+0.5)/256.0,(floor(base/256.0)+0.5)/128.0)).rgb;\n"
 " }\n"
 " rgb=saturate(rgb*shade.rgb);\n"
 " rgb=rgb*fog.a+saturate(fog.rgb)*(1.0-fog.a);\n"
 " return float4(rgb,shade.a);\n"
 "}\n";

static bool batch_texture(SceGxmTexture *tex, void *data, SceGxmTextureFormat fmt, int w, int h) {
    if (sceGxmTextureInitLinear(tex, data, fmt, w, h, 1) < 0) return false;
    sceGxmTextureSetUAddrMode(tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetMinFilter(tex, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(tex, SCE_GXM_TEXTURE_FILTER_POINT);
    return true;
}
/* Convert tile-major ROM bytes to a spatial 256x256 atlas of 16x16
 * tiles. Reuse 64KB of tilemap staging so VRAM receives contiguous copies. */
static void batch_pack_rom(uint8_t *rom, uint8_t *scratch, const uint8_t *data) {
    for (unsigned ty=0; ty<256; ty++) {
        for (unsigned y=0; y<16; y++)
            for (unsigned tx=0; tx<256; tx++)
                memcpy(scratch+y*4096+tx*16, data+((ty*256+tx)*256)+y*16, 16);
        memcpy(rom+ty*65536, scratch, 65536);
    }
}

static bool batch_init(void) {
    extern void vita_log(const char *, ...);
    FILE *disable = fopen("ux0:/data/raverace_batch.disable", "rb");
    if (disable) { fclose(disable); vita_log("[BATCH] disabled by marker\n"); return false; }
    if (!g_texture_data || !g_texture_tilemap) return false;
    SceGxmProgram *vp = compile_one(batch_v_src, SHARK_VERTEX_SHADER, "batch_v");
    SceGxmProgram *fp = compile_one(batch_f_src, SHARK_FRAGMENT_SHADER, "batch_f");
    if (!vp || !fp) return false;
    SceGxmShaderPatcherId vi, fi;
    if (sceGxmShaderPatcherRegisterProgram(s_patcher, vp, &vi) < 0 ||
        sceGxmShaderPatcherRegisterProgram(s_patcher, fp, &fi) < 0) return false;
    const char *names[] = {"aPosition", "aUV", "aShade", "aDecode", "aFog", "aTextured"};
    const int offsets[] = {0, 8, 20, 36, 52, 68};
    const int counts[] = {2, 3, 4, 4, 4, 1};
    SceGxmVertexAttribute attr[6] = {0};
    for (int i = 0; i < 6; i++) {
        const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(vp, names[i]);
        if (!p) return false;
        attr[i].streamIndex = 0; attr[i].offset = offsets[i];
        attr[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; attr[i].componentCount = counts[i];
        attr[i].regIndex = sceGxmProgramParameterGetResourceIndex(p);
    }
    SceGxmVertexStream stream = { sizeof(BatchVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    if (sceGxmShaderPatcherCreateVertexProgram(s_patcher, vi, attr, 6, &stream, 1, &batch_vp) < 0 ||
        sceGxmShaderPatcherCreateFragmentProgram(s_patcher, fi, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &batch_fp) < 0) return false;
    batch_screen = sceGxmProgramFindParameterByName(vp, "uScreen");
    if (!batch_screen) return false;
    size_t vertices_size = (BATCH_VERTICES * sizeof(BatchVertex) + 4095) & ~(size_t)4095;
    size_t index_size = (BATCH_VERTICES * sizeof(uint16_t) + 4095) & ~(size_t)4095;
    batch_frame_stride=BATCH_PAL_BYTES+vertices_size;
    size_t size = (BATCH_ROM_BYTES + BATCH_MAP_BYTES + 3*batch_frame_stride + index_size + 0x3ffff) & ~(size_t)0x3ffff;
    /* No baked texture has been used yet. Release its reserved pool so the
     * ROM sampler does not spill into uncached system RAM beside a idle cache. */
    gxm_tex_shutdown(); batch_released_cache = true;
    batch_uid = sceKernelAllocMemBlock("rr_batch", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    bool cdram = batch_uid >= 0;
    if (batch_uid < 0)
        batch_uid = sceKernelAllocMemBlock("rr_batch_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    vita_log("[BATCH] memory=%s\n", cdram ? "CDRAM" : "system uncached");
    if (batch_uid < 0) return false;
    sceKernelGetMemBlockBase(batch_uid, &batch_memory);
    if (sceGxmMapMemory(batch_memory, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) return false;
    batch_mapped = true;
    batch_cpu = malloc(BATCH_VERTICES * sizeof *batch_cpu);
    uint8_t *map = malloc(BATCH_MAP_BYTES);
    if (!batch_cpu || !map) { free(map); return false; }
    uint8_t *rom = batch_memory, *gpu_map = rom + BATCH_ROM_BYTES;
    batch_frames = gpu_map + BATCH_MAP_BYTES;
    batch_palette = batch_frames;
    batch_gpu = (BatchVertex *)(batch_palette + BATCH_PAL_BYTES);
    batch_indices = (uint16_t *)(batch_frames + 3*batch_frame_stride);
    batch_pack_rom(rom, map, g_texture_data);
    vita_log("[BATCH] pen ROM packed as spatial 16x16 tiles\n");
    for (unsigned i = 0; i < BATCH_MAP_BYTES / 4; i++) {
        map[i*4] = g_texture_tilemap[i*2]; map[i*4+1] = g_texture_tilemap[i*2+1];
        uint8_t attr = g_texture_tilemap[0x200000 + i/2];
        map[i*4+2] = (i&1) ? attr&15 : attr>>4; map[i*4+3] = 255;
    }
    memcpy(gpu_map, map, BATCH_MAP_BYTES); free(map);
    for (unsigned i = 0; i < BATCH_VERTICES; i++) batch_indices[i] = i;
    if (!batch_texture(&batch_rom_tex, rom, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, 4096, 4096) ||
        !batch_texture(&batch_map_tex, gpu_map, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 256, 4096) ||
        !batch_texture(&batch_pal_tex, batch_palette, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 256, 128)) return false;
    /* Sampler registers are reflected, just like vertex attributes. */
    const char *samplers[] = {"uMap", "uROM", "uPalette"};
    for (int i = 0; i < 3; i++) {
        const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(fp, samplers[i]);
        if (!p) return false;
        batch_sampler[i] = sceGxmProgramParameterGetResourceIndex(p);
        vita_log("[BATCH] sampler %s=%u\n", samplers[i], batch_sampler[i]);
    }
    vita_log("[BATCH] ROM renderer ready, GPU bytes=%u, vertex bytes=%u\n", (unsigned)size, (unsigned)sizeof(BatchVertex));
    return true;
}

static bool batch_begin(void) {
    if (!batch_attempted) {
        batch_attempted = true;
        batch_active = batch_init();
        if (!batch_active) {
            extern void vita_log(const char *, ...);
            vita_log("[BATCH] initialization failed; using baked renderer\n");
            free(batch_cpu); batch_cpu = NULL;
            if (batch_mapped) sceGxmUnmapMemory(batch_memory);
            if (batch_uid >= 0) sceKernelFreeMemBlock(batch_uid);
            batch_uid = -1; batch_memory = NULL; batch_mapped = false;
            if (batch_released_cache && !gxm_tex_init()) abort();
        }
    }
    if (!batch_active) return false;
    batch_count = 0;
    batch_palette=batch_frames+s_frame_slot*batch_frame_stride;
    batch_gpu=(BatchVertex *)(batch_palette+BATCH_PAL_BYTES);
    sceGxmTextureSetData(&batch_pal_tex,batch_palette);
    uint8_t rgba[256 * 4];
    for (int group = 0; group < 128; group++) {
        if (batch_palette_valid[s_frame_slot] && batch_palette_gen[s_frame_slot][group] == g_eng_pal_gen[group]) continue;
        for (int i = 0; i < 256; i++) {
            memcpy(rgba + i*4, direct_palette[group][i], 3); rgba[i*4+3] = 255;
        }
        memcpy(batch_palette + group * sizeof rgba, rgba, sizeof rgba);
        batch_palette_gen[s_frame_slot][group] = g_eng_pal_gen[group];
    }
    batch_palette_valid[s_frame_slot] = true;
    batch_started = sceKernelGetProcessTimeWide();
    return true;
}

#include "quad_compile.inc"
static void batch_quad(const geo_quad *q, const eng_draw_cfg *cfg) {
    batch_count+=quad_gxm_compile(q,cfg,batch_cpu+batch_count,BATCH_VERTICES-batch_count);
}
bool quad_gxm_packets_ready(void){return batch_active;}
void quad_gxm_append_packet(const eng_batch_vertex *vertices,unsigned count){
    if(!batch_active || count>BATCH_VERTICES-batch_count)return;
    if(count)memcpy(batch_cpu+batch_count,vertices,count*sizeof *vertices);
    batch_count+=count;
}
static void batch_end(void) {
    if(g_eng_frame%60==0){
        extern void vita_log(const char *, ...);
        vita_log("[DRAW_CHECK] frame=%u vertices=%u crc=%08lx\n",g_eng_frame,batch_count,
            (unsigned long)crc32(0,(const Bytef *)batch_cpu,batch_count*sizeof *batch_cpu));
    }
    if (!batch_count) return;
    uint64_t copy_start = sceKernelGetProcessTimeWide();
    memcpy(batch_gpu,batch_cpu,batch_count*sizeof *batch_gpu);
    uint64_t draw_start = sceKernelGetProcessTimeWide();
    sceGxmSetVertexProgram(s_ctx,batch_vp);
    sceGxmSetFragmentProgram(s_ctx,batch_fp);
    sceGxmSetFragmentTexture(s_ctx,batch_sampler[0],&batch_map_tex);
    sceGxmSetFragmentTexture(s_ctx,batch_sampler[1],&batch_rom_tex);
    sceGxmSetFragmentTexture(s_ctx,batch_sampler[2],&batch_pal_tex);
    void *uniform=NULL;
    sceGxmReserveVertexDefaultUniformBuffer(s_ctx,&uniform);
    sceGxmSetUniformDataF(uniform,batch_screen,0,4,s_screen_params);
    sceGxmSetVertexStream(s_ctx,0,batch_gpu);
    int r=sceGxmDraw(s_ctx,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,batch_indices,batch_count);
    extern void vita_log(const char *, ...);
    uint64_t draw_end = sceKernelGetProcessTimeWide();
    static uint64_t build_us, copy_us, submit_us;
    static unsigned frames;
    build_us += copy_start - batch_started; copy_us += draw_start - copy_start; submit_us += draw_end - draw_start;
    if (++frames == 60) {
        vita_log("[BATCH_TIME] ms/frame build=%.2f upload=%.2f submit=%.2f\n", build_us/60000.0, copy_us/60000.0, submit_us/60000.0);
        frames=0; build_us=copy_us=submit_us=0;
    }
    if(r<0)vita_log("[BATCH] draw failed %08X\n",r);
    if(g_eng_frame%60==0)vita_log("[BATCH] triangles=%u draws=1\n",batch_count/3);
}
