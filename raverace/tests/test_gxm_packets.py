"""Compare compiled draw packets with the frozen serial triangle builder."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
s=(root/'engine/quad_gxm.c').read_text()
clip=s[s.index('typedef struct { float x, y, s, t, w, bri; }'):s.index('/* ---- GXM Vertex')]
h=(root/'engine/quad_gxm.h').read_text();types=h[h.index('typedef struct {'):h.index('/* Sort far to near */')]
fog=s[s.index('int eng_fog_alpha('):s.index('static int g_sort_tie_emit')]
ref=(root/'raverace/tests/reference_batch_quad.inc').read_text()
code=(root/'engine/quad_compile.inc').read_text()
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "geo_hw.h"
#include "quad_packet.h"
#define ENG_SCREEN_W 640
#define ENG_SCREEN_H 480
#define BATCH_VERTICES 96
typedef eng_batch_vertex BatchVertex;
static float g_scene_x0=-103.529411f,g_scene_x1=743.529411f;
static uint8_t direct_palette[128][256][3];
static BatchVertex batch_cpu[BATCH_VERTICES];
static unsigned batch_count;
void vita_log(const char*s,...){(void)s;}
'''
test=r'''
static unsigned seed=2277;
static unsigned next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static int fog_cb(const geo_quad*q,eng_fog*f){f->rgb[0]=q->color;f->rgb[1]=q->color>>8;f->rgb[2]=q->color>>16;f->alpha_const=q->cz_value&255;return 1;}
int main(){
 for(unsigned i=0;i<sizeof direct_palette;i++)((uint8_t*)direct_palette)[i]=next();
 for(int t=0;t<30000;t++){
  geo_quad q={0};q.nrv=3+next()%8;q.color=next()&0xffffff;q.cmode=next()%16;q.texbank=next()%16;
  q.objectflags=next()%8;q.cz_adjust=next()&0xffffff;q.cz_value=next()%8192;
  q.clip[0]=t%2?0:next()%320;q.clip[1]=t%2?639:320+next()%320;q.clip[2]=next()%240;q.clip[3]=240+next()%240;
  /* Ordered convex polygons with edges both inside and outside the viewport. */
  for(int i=0;i<q.nrv;i++){
   double a=i*6.283185307179586/q.nrv;
   q.rv[i].sx16=(int)(16*(320+800*cos(a)));q.rv[i].sy16=(int)(16*(240+600*sin(a)));
   q.rv[i].z=1+next()%500000;q.rv[i].u=next()%4096;q.rv[i].v=next()%4096;q.rv[i].bri=next()%256;
  }
  eng_draw_cfg cfg={.shade=t%3!=0,.fog=t%5!=0,.fog_before_shade=t%2,.fog_quad=fog_cb,.write_prio_alpha=t%7!=0,.texel_centre=t%2};
  BatchVertex out[90];memset(out,0,sizeof out);memset(batch_cpu,0,sizeof batch_cpu);batch_count=0;
  reference_batch_quad(&q,&cfg);unsigned n=quad_gxm_compile(&q,&cfg,out,90);
  assert(n==batch_count);assert(!memcmp(out,batch_cpu,n*sizeof *out));
  if(n)assert(quad_gxm_compile(&q,&cfg,out,n-1)==0);
 }
 puts("PASS: 30000 compiled polygons produce byte-identical triangle streams, including viewport clipping, lighting, fog, palette modes and capacity boundaries");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-packets-') as td:
 p=Path(td);(p/'test.c').write_text(prefix+types+fog+clip+ref+code+test)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I'+str(root/'engine'),str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
