"""Validate submitted native/fallback draw runs against the original vertex stream."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "quad_packet.h"
typedef eng_batch_vertex BatchVertex;
static BatchVertex cpu[30000],gpu[30000],*batch_cpu=cpu,*batch_gpu=gpu;
static unsigned batch_count,g_eng_frame=1,visited;
static unsigned short batch_indices[30000];
static int s_ctx,bank_fp=1,batch_fp=2,bank_texture,batch_pal_tex,batch_map_tex,batch_rom_tex;
static unsigned bank_sampler,bank_palette_sampler,batch_sampler[3];
static bool bank_cache_enabled;static int bank_id;
static float bank_area[16];static int program;static BatchVertex *stream;
#define SCE_GXM_PRIMITIVE_TRIANGLES 0
#define SCE_GXM_INDEX_FORMAT_U16 0
static void sceGxmSetFragmentProgram(int c,int p){(void)c;program=p;}
static void sceGxmSetFragmentTexture(int c,unsigned i,const int*t){(void)c;(void)i;(void)t;}
static void sceGxmSetVertexStream(int c,int i,BatchVertex*v){(void)c;(void)i;stream=v;}
void vita_log(const char *s,...){(void)s;}
static int sceGxmDraw(int c,int p,int f,const unsigned short *indices,unsigned n){
 (void)c;(void)p;(void)f;assert(n%3==0);
 for(unsigned i=0;i<n;i++){
  const BatchVertex *want=cpu+visited,*got=stream+indices[i];assert(!memcmp(want,got,sizeof *got));
  bool native=bank_cache_enabled&&bank_id>=0&&(want->textured<0.5f||(int)(want->bank+0.5f)==bank_id);
  assert(program==(native?bank_fp:batch_fp));visited++;
 }return 0;
}
'''
test=r'''int main(void){
 unsigned rng=47;for(unsigned i=0;i<30000;i++)batch_indices[i]=i;
 for(unsigned trial=0;trial<120;trial++){
  bank_cache_enabled=trial%3!=0;bank_id=trial%17-1;batch_count=30000;visited=0;
  for(unsigned i=0;i<batch_count;i+=3){
   rng=rng*1664525+1013904223;float bank=(rng>>16)&15,textured=(rng>>8)&1;
   for(unsigned j=0;j<3;j++)cpu[i+j]=(BatchVertex){.x=i+j,.bank=bank,.textured=textured};
  }
  memcpy(gpu,cpu,sizeof gpu);assert(!bank_draw());assert(visited==batch_count);
 }
 puts("PASS: 3,600,000 submitted vertices preserve painter order and native/fallback shader selection across draw-run boundaries");
}
'''
source=(root/'engine/quad_gxm_banks.inc').read_text()
source=source[source.index('static bool bank_native_triangle'):]
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(prefix+source+test)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I'+str(root/'engine'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
