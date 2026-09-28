"""Execute the actual native HUD shader as host C++ against the arcade decoder."""
from pathlib import Path
import ast, re, subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
s=(root/'engine/quad_gxm_hud.inc').read_text()
a=s.index('static const char hud_f_src[]');b=s.index('static void hud_shutdown',a)
shader=''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"',s[a:b]))
shader=re.sub(r':(?:TEXCOORD\d|COLOR)','',shader).replace('uniform ','').replace('float4 main(', 'float4 sample_hud(')
shader=shader.replace('discard;', 'return make_float4(make_float3(0,0,0),0);')
for t in ('float2','float3','float4'):shader=shader.replace(t+'(', 'make_'+t+'(')
shader=shader.replace('make_make_', 'make_')
source=r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef float float2 __attribute__((ext_vector_type(2)));
typedef float float3 __attribute__((ext_vector_type(3)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef int sampler2D;
static float2 make_float2(float x,float y){return {x,y};}
static float3 make_float3(float x,float y,float z){return {x,y,z};}
static float4 make_float4(float3 x,float a){return {x.x,x.y,x.z,a};}
static float2 floor(float2 v){return {floorf(v.x),floorf(v.y)};}
static uint8_t chars[0x20000],map[0x2000],palette[1024];
static float4 tex2D(int sampler,float2 uv){
 int w=sampler==1?64:256,h=sampler==0?512:sampler==1?64:1;
 int x=(int)floorf(uv.x*w),y=(int)floorf(uv.y*h);
 assert(x>=0&&x<w&&y>=0&&y<h);
 if(sampler==0)return {chars[y*w+x]/255.f,0,0,1};
 if(sampler==1)return {map[(y*w+x)*2]/255.f,map[(y*w+x)*2+1]/255.f,0,1};
 return {palette[x*4]/255.f,palette[x*4+1]/255.f,palette[x*4+2]/255.f,1};
}
'''+shader+r'''
int main(){
 srand(2244);
 for(int trial=0;trial<64;trial++){
  for(unsigned i=0;i<sizeof chars;i++)chars[i]=rand();
  for(unsigned i=0;i<sizeof map;i++)map[i]=chars[0x1e000+i]=rand();
  for(unsigned i=0;i<sizeof palette;i++)palette[i]=rand();
  int sx=rand()&1023,sy=rand()&1023;
  for(int n=0;n<10000;n++){
   int x=rand()%640,y=rand()%480,tx=(x+sx)&1023,ty=(y+sy)&1023;
   int ti=((ty>>4)*64+(tx>>4))*2,w=map[ti]*256+map[ti+1];
   int cx=tx&15,cy=ty&15;if(w&0x400)cx=15-cx;if(w&0x800)cy=15-cy;
   int off=(w&1023)*128+cy*8+cx/2;
   int pen=(cx&1)?chars[off]&15:chars[off]>>4;
   float3 uv={(x+0.5f)/640.f,(y+0.5f)/480.f,1};
   float4 color={1,1,1,1};float2 scroll={(float)sx,(float)sy};
   float4 got=sample_hud(uv,color,0,1,2,scroll);
   assert(got.w==(pen==15?0:1));
   if(pen!=15)for(int c=0;c<3;c++)assert(fabsf(got[c]-palette[((w>>12)*16+pen)*4+c]/255.f)<0.00001f);
  }
 }
 puts("PASS: 640000 native HUD shader samples match arcade decoding, flips, scroll wrapping, RAM aliases and transparent pens");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-hud-tiles-') as td:
 p=Path(td);(p/'test.cpp').write_text(source)
 subprocess.run(['clang++','-O2','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
