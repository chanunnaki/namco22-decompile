#include "texture_bank_decode.inc"
static bool bank_cache_enabled,bank_cache_full;
static int bank_id=-1;
static uint8_t *bank_memory;
static SceGxmTexture bank_texture;
static SceGxmFragmentProgram *bank_fp;
static unsigned bank_sampler,bank_palette_sampler;
static float bank_area[16];
static const char bank_f_src[] =
 "float4 main(float3 uv:TEXCOORD0, float4 shade:TEXCOORD1, float4 decode:TEXCOORD2,\n"
 " float4 fog:TEXCOORD3, float textured:TEXCOORD4,\n"
 " uniform sampler2D uBank, uniform sampler2D uPalette) {\n"
 " decode=floor(decode+0.5);\n"
 " float3 rgb=float3(1.0,1.0,1.0);\n"
 " if(textured>0.5) {\n"
 "  float pen=floor(tex2D(uBank,(uv.xy/uv.z)/4096.0).r*255.0+0.5);\n"
 "  pen=fmod(floor(pen/decode.z),decode.w);\n"
 "  float base=decode.y;\n"
 "  rgb=tex2D(uPalette,float2((fmod(base,256.0)+pen+0.5)/256.0,(floor(base/256.0)+0.5)/128.0)).rgb;\n"
 " }\n"
 " rgb=saturate(rgb*shade.rgb);\n"
 " rgb=rgb*fog.a+saturate(fog.rgb)*(1.0-fog.a);\n"
 " return float4(rgb,shade.a);\n"
 "}\n";


static bool bank_shader_init(const SceGxmProgram *vp){
    SceGxmProgram *fp=compile_one(bank_f_src,SHARK_FRAGMENT_SHADER,"native_bank_f");
    SceGxmShaderPatcherId id;
    if(!fp || sceGxmShaderPatcherRegisterProgram(s_patcher,fp,&id)<0)return false;
    if(sceGxmShaderPatcherCreateFragmentProgram(s_patcher,id,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE,NULL,vp,&bank_fp)<0)return false;
    const SceGxmProgramParameter *p=sceGxmProgramFindParameterByName(fp,"uBank");if(!p)return false;
    bank_sampler=sceGxmProgramParameterGetResourceIndex(p);
    p=sceGxmProgramFindParameterByName(fp,"uPalette");if(!p)return false;
    bank_palette_sampler=sceGxmProgramParameterGetResourceIndex(p);return true;
}
void quad_gxm_begin_bank_plan(void){memset(bank_area,0,sizeof bank_area);}
void quad_gxm_add_bank_plan(const eng_batch_vertex *v,unsigned count){
    if(!bank_cache_enabled)return;
    for(unsigned i=0;i+2<count;i+=3){
        int bank=(int)(v[i].bank+0.5f);
        if(v[i].textured>0.5f && bank>=0 && bank<16)
            bank_area[bank]+=fabsf((v[i+1].x-v[i].x)*(v[i+2].y-v[i].y)-(v[i+2].x-v[i].x)*(v[i+1].y-v[i].y));
    }
}
/* Called BEFORE BeginScene, after the previous CPU renderer has finished.
 * Drain submitted GPU reads only when replacing immutable cached pixels. */
void quad_gxm_finish_bank_plan(void){
    if(!bank_cache_enabled || bank_cache_full)return;
    int best=-1;float largest=0;
    for(int b=0;b<16;b++)if(bank_area[b]>largest){largest=bank_area[b];best=b;}
    if(best<0 || best==bank_id || (bank_id>=0 && bank_area[bank_id]*4>=largest))return;
    uint64_t start=sceKernelGetProcessTimeWide();
    if(!bank_memory){
        SceUID uid=sceKernelAllocMemBlock("rr_native_bank",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,0x1000000,NULL);
        if(uid<0){bank_cache_full=true;return;}
        void *base=NULL;sceKernelGetMemBlockBase(uid,&base);
        if(sceGxmMapMemory(base,0x1000000,SCE_GXM_MEMORY_ATTRIB_READ)<0){sceKernelFreeMemBlock(uid);bank_cache_full=true;return;}
        if(!batch_texture(&bank_texture,base,SCE_GXM_TEXTURE_FORMAT_U8_RRRR,4096,4096)){
            sceGxmUnmapMemory(base);sceKernelFreeMemBlock(uid);bank_cache_full=true;return;
        }
        bank_memory=base;
        sceGxmTextureSetUAddrMode(&bank_texture,SCE_GXM_TEXTURE_ADDR_REPEAT);
        sceGxmTextureSetVAddrMode(&bank_texture,SCE_GXM_TEXTURE_ADDR_REPEAT);
    } else vita2d_wait_rendering_done();
    decode_texture_bank(bank_memory,best,g_texture_data,g_texture_tilemap);bank_id=best;
    extern void vita_log(const char *,...);vita_log("[NATIVE_BANK] resident=%d decode_ms=%.2f\n",best,(sceKernelGetProcessTimeWide()-start)/1000.0);
}
static bool bank_native_triangle(const BatchVertex *v){
    return bank_cache_enabled && bank_id>=0 && (v->textured<0.5f || (int)(v->bank+0.5f)==bank_id);
}
static int bank_draw(void){
    unsigned runs=0,native_vertices=0;
    for(unsigned first=0;first<batch_count;){
        bool native=bank_native_triangle(batch_cpu+first);
        unsigned end=first+3;
        while(end<batch_count && bank_native_triangle(batch_cpu+end)==native)end+=3;
        sceGxmSetFragmentProgram(s_ctx,native?bank_fp:batch_fp);
        if(native){
            sceGxmSetFragmentTexture(s_ctx,bank_sampler,&bank_texture);
            sceGxmSetFragmentTexture(s_ctx,bank_palette_sampler,&batch_pal_tex);native_vertices+=end-first;
        }else{
            sceGxmSetFragmentTexture(s_ctx,batch_sampler[0],&batch_map_tex);
            sceGxmSetFragmentTexture(s_ctx,batch_sampler[1],&batch_rom_tex);
            sceGxmSetFragmentTexture(s_ctx,batch_sampler[2],&batch_pal_tex);
        }
        sceGxmSetVertexStream(s_ctx,0,batch_gpu+first);
        int r=sceGxmDraw(s_ctx,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,batch_indices,end-first);
        if(r<0)return r;
        first=end;runs++;
    }
    if(g_eng_frame%60==0 && bank_cache_enabled){
        float total=0;for(int i=0;i<16;i++)total+=bank_area[i];
        extern void vita_log(const char *,...);vita_log("[NATIVE_BANK] frame=%u area_hit=%.1f%% bank=%d runs=%u native_vertices=%u\n",g_eng_frame,
            total>0&&bank_id>=0?bank_area[bank_id]*100/total:0,bank_id,runs,native_vertices);
    }
    return 0;
}
