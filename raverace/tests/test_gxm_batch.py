"""Run the actual Cg pixel formula as host vector C++ against the CPU texture oracle."""
from pathlib import Path
import ast, re, subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
s=(root/'engine/quad_gxm_batch.inc').read_text()
a=s.index('static const char batch_f_src[] =');b=s.index('static bool batch_texture',a)
shader=''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"',s[a:b]))
shader=re.sub(r':TEXCOORD\d','',shader).replace('uniform ','').replace('float4 main(', 'float4 shader_sample(')
for t in ['float2','float3','float4']:shader=shader.replace(t+'(', 'make_'+t+'(')
tex=(root/'engine/tex_bake.c').read_text()
oracle=tex[tex.index('static int get_tile_attr'):tex.index('/* When set,')]
a=tex.index('static void cmode_params');oracle+=tex[a:tex.index('/* ========== Per-Quad',a)]
map_loop=s[s.index('    for (unsigned i = 0; i < BATCH_MAP_BYTES / 4; i++)'):s.index('    memcpy(gpu_map, map, BATCH_MAP_BYTES);')]
packer=s[s.index('static void batch_pack_rom('):s.index('static bool batch_init(void)')]
prefix=r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef float float2 __attribute__((ext_vector_type(2)));
typedef float float3 __attribute__((ext_vector_type(3)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef int sampler2D;
static float2 make_float2(float x,float y){return {x,y};}
static float3 make_float3(float x,float y,float z){return {x,y,z};}
static float4 make_float4(float3 v,float a){return {v.x,v.y,v.z,a};}
static float2 floor(float2 v){return {floorf(v.x),floorf(v.y)};}
static float3 floor(float3 v){return {floorf(v.x),floorf(v.y),floorf(v.z)};}
static float4 floor(float4 v){return {floorf(v.x),floorf(v.y),floorf(v.z),floorf(v.w)};}
static float3 saturate(float3 v){for(int i=0;i<3;i++)v[i]=fminf(fmaxf(v[i],0),1);return v;}
#define TEXTUREMAP_SIZE 0x280000
#define TEXTURE_TOTAL_SIZE 0x1000000
#define BATCH_MAP_BYTES 0x400000
static uint8_t *g_texture_data,*g_texture_tilemap,*map,*rom_atlas;
static uint8_t palette[128][256][3];
static float4 tex2D(int sampler,float2 uv){
 int w=sampler==1?4096:256,h=sampler==2?128:4096;
 int x=(int)floorf(uv.x*w),y=(int)floorf(uv.y*h);x=x<0?0:x>=w?w-1:x;y=y<0?0:y>=h?h-1:y;
 float4 out={0,0,0,1};
 if(sampler==1)out.x=rom_atlas[y*w+x]/255.0f;
 else if(sampler==0)for(int c=0;c<3;c++)out[c]=map[(y*w+x)*4+c]/255.0f;
 else for(int c=0;c<3;c++)out[c]=palette[y][x][c]/255.0f;
 return out;
}
'''
test=r'''
static uint32_t seed=22;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void){
 g_texture_data=(uint8_t*)malloc(TEXTURE_TOTAL_SIZE);g_texture_tilemap=(uint8_t*)malloc(TEXTUREMAP_SIZE);map=(uint8_t*)malloc(BATCH_MAP_BYTES);
 for(int i=0;i<TEXTURE_TOTAL_SIZE;i++)g_texture_data[i]=next();
 for(int i=0;i<TEXTUREMAP_SIZE;i++)g_texture_tilemap[i]=next();
 for(int g=0;g<128;g++)for(int i=0;i<256;i++)for(int c=0;c<3;c++)palette[g][i][c]=next();
 rom_atlas=(uint8_t*)malloc(TEXTURE_TOTAL_SIZE);batch_pack_rom(rom_atlas,map,g_texture_data);
 for(unsigned i=0;i<TEXTURE_TOTAL_SIZE;i++) {
  unsigned tile=i/256,x=i%16,y=(i/16)%16;
  assert(rom_atlas[((tile/256)*16+y)*4096+(tile%256)*16+x]==g_texture_data[i]);
 }
 MAP_LOOP
 for(int i=0;i<200000;i++){
  int u=(int16_t)next(),v=(int16_t)next(),bank=next()%16,group=next()%128,cm=next()%16;
  int offset,shift,mask;cmode_params(cm,&offset,&shift,&mask);
  float3 uv={(float)u+0.5f,(float)v+0.5f,1};
  float4 shade={1,1,1,0.5f},decode={(float)bank,(float)(group*256+offset),(float)(1<<shift),(float)(mask+1)},fog={0,0,0,1};
  float4 got=shader_sample(uv,shade,decode,fog,1,0,1,2);
  int pen=offset+((texture_pen_lookup(u,v,bank)>>shift)&mask);
  for(int c=0;c<3;c++)if(fabsf(got[c]-palette[group][pen][c]/255.0f)>1e-6f){fprintf(stderr,"mismatch %d u%d v%d bank%d cm%d\n",i,u,v,bank,cm);return 1;}
  assert(got.a==0.5f);
 }
 free(rom_atlas);free(g_texture_data);free(g_texture_tilemap);free(map);
 puts("PASS: 200000 shader texture samples match CPU oracle across banks, flips, transpose, cmodes and wrapped coordinates");
}
'''.replace('MAP_LOOP',map_loop)
with tempfile.TemporaryDirectory(prefix='rr-batch-') as d:
 d=Path(d);(d/'test.cpp').write_text(prefix+packer+oracle+shader+test)
 subprocess.run(['clang++','-O2','-fsanitize=address,undefined',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
