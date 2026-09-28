#include "hud_atlas_cache.inc"
#define HUD_ATLAS_BYTES (HUD_ATLAS_W*HUD_ATLAS_H*4u)
static bool hud_atlas_mode;
static unsigned hud_atlas_uploaded[3][HUD_CACHE_SLOTS];
static SceGxmTexture hud_atlas_tex;
static uint16_t *hud_atlas_indices;
static unsigned hud_atlas_sampler;
static void hud_atlas_shutdown(void){
    free(hud_glyphs);hud_glyphs=NULL;hud_atlas_mode=false;
    hud_cache_frame=hud_cache_version=0;
    memset(hud_atlas_uploaded,0,sizeof hud_atlas_uploaded);
}
static bool hud_atlas_init(const SceGxmProgram *vertex){
    const char *src="float4 main(float3 uv:TEXCOORD0, uniform sampler2D uAtlas) { float4 c=tex2D(uAtlas,uv.xy); if(c.a<0.5) discard; return c; }";
    SceGxmProgram *fp=compile_one(src,SHARK_FRAGMENT_SHADER,"hud_glyph_atlas_f");
    if(!fp || sceGxmShaderPatcherRegisterProgram(s_patcher,fp,&hud_id)<0)return false;
    SceGxmBlendInfo blend={0};
    blend.colorMask=SCE_GXM_COLOR_MASK_R|SCE_GXM_COLOR_MASK_G|SCE_GXM_COLOR_MASK_B;
    blend.colorFunc=blend.alphaFunc=SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc=SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;blend.colorDst=SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    blend.alphaSrc=SCE_GXM_BLEND_FACTOR_ONE;blend.alphaDst=SCE_GXM_BLEND_FACTOR_ZERO;
    if(sceGxmShaderPatcherCreateFragmentProgram(s_patcher,hud_id,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE,&blend,vertex,&hud_fp)<0)return false;
    const SceGxmProgramParameter *p=sceGxmProgramFindParameterByName(fp,"uAtlas");if(!p)return false;
    hud_atlas_sampler=sceGxmProgramParameterGetResourceIndex(p);
    hud_uid=sceKernelAllocMemBlock("rr_hud_atlas",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,0x640000,NULL);
    if(hud_uid<0)return false;
    void *base=NULL;sceKernelGetMemBlockBase(hud_uid,&base);
    if(sceGxmMapMemory(base,0x640000,SCE_GXM_MEMORY_ATTRIB_READ)<0)return false;
    hud_memory=base;hud_atlas_indices=(uint16_t *)(hud_memory+3*HUD_ATLAS_BYTES);
    for(unsigned i=0;i<HUD_CACHE_TILES*6;i++)hud_atlas_indices[i]=i;
    if(sceGxmTextureInitLinear(&hud_atlas_tex,hud_memory,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,HUD_ATLAS_W,HUD_ATLAS_H,1)<0)return false;
    sceGxmTextureSetMinFilter(&hud_atlas_tex,SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&hud_atlas_tex,SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&hud_atlas_tex,SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&hud_atlas_tex,SCE_GXM_TEXTURE_ADDR_CLAMP);
    hud_active=hud_atlas_mode=true;
    extern void vita_log(const char *,...);vita_log("[HUD_ATLAS] cached glyph renderer enabled\n");return true;
}
static void hud_atlas_upload(const uint8_t *chars,const uint8_t *map,const uint8_t *rgba,unsigned sx,unsigned sy){
    hud_cache_build(chars,map,rgba,sx,sy);
    uint8_t *dst=hud_memory+s_frame_slot*HUD_ATLAS_BYTES;
    sceGxmTextureSetData(&hud_atlas_tex,dst);
    unsigned uploaded=0;
    for(unsigned i=0;i<hud_tile_count;i++){
        unsigned slot=hud_tiles[i].slot;hud_cached_glyph *g=hud_glyphs+slot;
        if(hud_atlas_uploaded[s_frame_slot][slot]==g->version)continue;
        unsigned x=(slot&63)*16,y=(slot>>6)*16;
        for(unsigned row=0;row<16;row++)memcpy(dst+((y+row)*HUD_ATLAS_W+x)*4,g->pixels+row*64,64);
        hud_atlas_uploaded[s_frame_slot][slot]=g->version;uploaded++;
    }
    if(g_eng_frame%60==0){extern void vita_log(const char *,...);vita_log("[HUD_ATLAS] frame=%u tiles=%u rebuilt=%u uploaded=%u\n",g_eng_frame,hud_tile_count,hud_rebuilt,uploaded);}
}
static void hud_atlas_draw(void){
    if(!hud_tile_count)return;
    unsigned n=hud_tile_count*6;size_t bytes=n*sizeof(GxmQuadVertex);
    if(s_vtx_ring_offset+bytes>(s_frame_slot+1)*(VTX_RING_SIZE/4))abort();
    GxmQuadVertex *v=(GxmQuadVertex *)(s_vtx_ring_base+s_vtx_ring_offset);s_vtx_ring_offset+=bytes;
    const unsigned corner[6]={0,1,2,0,2,3};
    for(unsigned i=0;i<hud_tile_count;i++){
        hud_cached_tile t=hud_tiles[i];
        for(unsigned j=0;j<6;j++){
            unsigned c=corner[j];bool right=c==1||c==2,bottom=c>=2;
            v[i*6+j]=(GxmQuadVertex){right?t.x1:t.x0,bottom?t.y1:t.y0,0.5f,right?t.u1:t.u0,bottom?t.v1:t.v0,1,1,1,1,1};
        }
    }
    sceGxmSetVertexProgram(s_ctx,s_vprog);sceGxmSetFragmentProgram(s_ctx,hud_fp);
    sceGxmSetFragmentTexture(s_ctx,hud_atlas_sampler,&hud_atlas_tex);
    void *buf=NULL;sceGxmReserveVertexDefaultUniformBuffer(s_ctx,&buf);
    sceGxmSetUniformDataF(buf,s_u_screen_params,0,4,s_screen_params);
    sceGxmSetVertexStream(s_ctx,0,v);
    sceGxmDraw(s_ctx,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,hud_atlas_indices,n);
}
