/* Native arcade tilemap HUD: byte-addressed character RAM, tile attributes,
 * and a 256-entry palette. The caller waits for the preceding GPU scene before
 * updating this storage. No CPU-expanded 640x480 RGBA framebuffer is needed. */
static bool hud_active;
static SceUID hud_uid = -1;
static uint8_t *hud_memory;
static SceGxmTexture hud_chars, hud_map, hud_palette;
static SceGxmFragmentProgram *hud_fp;
static SceGxmShaderPatcherId hud_id;
static const SceGxmProgramParameter *hud_scroll;
static unsigned hud_samplers[3];
static float hud_offset[2];
static uint8_t hud_previous[3][0x22400];
static bool hud_valid[3];

static const char hud_f_src[] =
 "float4 main(float3 vTexcoord:TEXCOORD0, float4 vColor:COLOR,\n"
 " uniform sampler2D uChars, uniform sampler2D uMap, uniform sampler2D uPalette,\n"
 " uniform float2 uScroll) {\n"
 " float2 p=floor(vTexcoord.xy*float2(640.0,480.0))+uScroll;\n"
 " p=p-floor(p/1024.0)*1024.0;\n"
 " float2 tile=floor(p/16.0); float2 local=p-tile*16.0;\n"
 " float2 entry=floor(tex2D(uMap,(tile+0.5)/64.0).rg*255.0+0.5);\n"
 " float hi=entry.x; float code=fmod(hi,4.0)*256.0+entry.y;\n"
 " if(fmod(floor(hi/4.0),2.0)>0.5) local.x=15.0-local.x;\n"
 " if(fmod(floor(hi/8.0),2.0)>0.5) local.y=15.0-local.y;\n"
 " float address=code*128.0+local.y*8.0+floor(local.x/2.0);\n"
 " float byte=floor(tex2D(uChars,float2((fmod(address,256.0)+0.5)/256.0,(floor(address/256.0)+0.5)/512.0)).r*255.0+0.5);\n"
 " float pen=fmod(local.x,2.0)<0.5 ? floor(byte/16.0) : fmod(byte,16.0);\n"
 " if(pen>14.5) discard;\n"
 " float index=floor(hi/16.0)*16.0+pen;\n"
 " return float4(tex2D(uPalette,float2((index+0.5)/256.0,0.5)).rgb,1.0);\n"
 "}\n";

static void hud_atlas_shutdown(void);
static void hud_shutdown(void) {
    hud_atlas_shutdown();
    if(hud_fp) sceGxmShaderPatcherReleaseFragmentProgram(s_patcher,hud_fp);
    if(hud_id) sceGxmShaderPatcherUnregisterProgram(s_patcher,hud_id);
    if(hud_memory) sceGxmUnmapMemory(hud_memory);
    if(hud_uid>=0) sceKernelFreeMemBlock(hud_uid);
    hud_fp=NULL;hud_id=0;hud_memory=NULL;hud_uid=-1;hud_active=false;
}
#include "quad_gxm_hud_atlas.inc"

static bool hud_init(const SceGxmProgram *vertex) {
    extern void vita_log(const char *, ...);
    FILE *f=fopen("ux0:/data/raverace_hud_legacy.enable","rb");
    if(f){fclose(f);return false;}
    f=fopen("ux0:/data/raverace_hud_decoder.enable","rb");
    if(f)fclose(f);
    else {if(hud_atlas_init(vertex))return true;hud_shutdown();}
    SceGxmProgram *fp=compile_one(hud_f_src,SHARK_FRAGMENT_SHADER,"hud_tilemap_f");
    if(!fp) return false;
    if(sceGxmShaderPatcherRegisterProgram(s_patcher,fp,&hud_id)<0) return false;
    SceGxmBlendInfo blend={0};
    blend.colorMask=SCE_GXM_COLOR_MASK_R|SCE_GXM_COLOR_MASK_G|SCE_GXM_COLOR_MASK_B;
    blend.colorFunc=blend.alphaFunc=SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc=SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.colorDst=SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    blend.alphaSrc=SCE_GXM_BLEND_FACTOR_ONE;
    blend.alphaDst=SCE_GXM_BLEND_FACTOR_ZERO;
    if(sceGxmShaderPatcherCreateFragmentProgram(s_patcher,hud_id,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,SCE_GXM_MULTISAMPLE_NONE,
        &blend,vertex,&hud_fp)<0) goto fail;
    const char *names[]={"uChars","uMap","uPalette"};
    for(int i=0;i<3;i++){
        const SceGxmProgramParameter *p=sceGxmProgramFindParameterByName(fp,names[i]);
        if(!p)goto fail;
        hud_samplers[i]=sceGxmProgramParameterGetResourceIndex(p);
    }
    hud_scroll=sceGxmProgramFindParameterByName(fp,"uScroll");
    if(!hud_scroll)goto fail;
    hud_uid=sceKernelAllocMemBlock("rr_hud_tiles",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,0xc0000,NULL);
    if(hud_uid<0)goto fail;
    void *base=NULL;sceKernelGetMemBlockBase(hud_uid,&base);
    if(sceGxmMapMemory(base,0xc0000,SCE_GXM_MEMORY_ATTRIB_READ)<0)goto fail;
    hud_memory=base;
    if(sceGxmTextureInitLinear(&hud_chars,hud_memory,SCE_GXM_TEXTURE_FORMAT_U8_RRRR,256,512,1)<0 ||
       sceGxmTextureInitLinear(&hud_map,hud_memory+0x20000,SCE_GXM_TEXTURE_FORMAT_U8U8_GR,64,64,1)<0 ||
       sceGxmTextureInitLinear(&hud_palette,hud_memory+0x22000,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,256,1,1)<0)goto fail;
    SceGxmTexture *textures[]={&hud_chars,&hud_map,&hud_palette};
    for(int i=0;i<3;i++){
        sceGxmTextureSetMinFilter(textures[i],SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(textures[i],SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(textures[i],SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(textures[i],SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
    hud_active=true;vita_log("[HUD_GPU] tilemap renderer enabled\n");return true;
fail:
    hud_shutdown();return false;
}
bool quad_gxm_native_hud(void){return hud_active;}
static void hud_upload(size_t offset,const uint8_t *src,size_t count){
    /* Cached shadow detects unchanged 128-byte character/tile blocks. */
    for(size_t i=0;i<count;i+=128){
        size_t n=count-i<128?count-i:128;
        if(!hud_valid[s_frame_slot] || memcmp(hud_previous[s_frame_slot]+offset+i,src+i,n)){
            memcpy(hud_memory+s_frame_slot*0x40000+offset+i,src+i,n);
            memcpy(hud_previous[s_frame_slot]+offset+i,src+i,n);
        }
    }
}
void quad_gxm_upload_hud(const uint8_t *chars,const uint8_t *map,
                         const uint8_t *rgba,unsigned sx,unsigned sy){
    if(!hud_active)return;
    if(hud_atlas_mode){hud_atlas_upload(chars,map,rgba,sx,sy);return;}
    sceGxmTextureSetData(&hud_chars,hud_memory+s_frame_slot*0x40000);
    sceGxmTextureSetData(&hud_map,hud_memory+s_frame_slot*0x40000+0x20000);
    sceGxmTextureSetData(&hud_palette,hud_memory+s_frame_slot*0x40000+0x22000);
    hud_upload(0,chars,0x1e000);
    hud_upload(0x1e000,map,0x2000); /* character codes 960..1023 alias tile RAM */
    hud_upload(0x20000,map,0x2000);
    hud_upload(0x22000,rgba,1024);
    hud_offset[0]=sx;hud_offset[1]=sy;hud_valid[s_frame_slot]=true;
}
void quad_gxm_draw_native_hud(void){
    if(!hud_active)return;
    if(hud_atlas_mode){hud_atlas_draw();return;}
    size_t bytes=4*sizeof(GxmQuadVertex);
    if(s_vtx_ring_offset+bytes>VTX_RING_SIZE)return;
    GxmQuadVertex *v=(GxmQuadVertex *)(s_vtx_ring_base+s_vtx_ring_offset);
    s_vtx_ring_offset+=bytes;
    for(int i=0;i<4;i++){
        float u=(i==1||i==2)?1.0f:0.0f,t=i>=2?1.0f:0.0f;
        v[i]=(GxmQuadVertex){u*ENG_SCREEN_W,t*ENG_SCREEN_H,0.5f,u,t,1,1,1,1,1};
    }
    sceGxmSetVertexProgram(s_ctx,s_vprog);sceGxmSetFragmentProgram(s_ctx,hud_fp);
    sceGxmSetFragmentTexture(s_ctx,hud_samplers[0],&hud_chars);
    sceGxmSetFragmentTexture(s_ctx,hud_samplers[1],&hud_map);
    sceGxmSetFragmentTexture(s_ctx,hud_samplers[2],&hud_palette);
    void *buf=NULL;sceGxmReserveFragmentDefaultUniformBuffer(s_ctx,&buf);
    sceGxmSetUniformDataF(buf,hud_scroll,0,2,hud_offset);
    sceGxmSetVertexStream(s_ctx,0,v);draw_fan(4);
}
