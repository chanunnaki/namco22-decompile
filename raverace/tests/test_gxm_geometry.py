"""Whole-polygon equivalence for the no-near-clip projection reuse path."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'engine/geo_hw.c').read_text()
a = source.index('    if (!q.behind) {\n        /* No edge')
b = source.index('        const int UF = 16, TF = 24;', a)
reference = source[:a] + '    {\n' + source[b:]
names = ['geo_hw_set_view', 'geo_hw_zoom_from_dspfloat', 'geo_hw_object',
         'g_geo_stats', 'g_bbox_cur', 'g_zord_ap', 'g_zord_os']
wrapper = '''
static void save_quad(const geo_quad *q, void *out) { *(geo_quad *)out = *q; }
void RUN(const geo_view *view, int color, int flags, int format, geo_quad *out) {
    memset(out, 0, sizeof *out);
    geo_hw_set_view(view);
    quad_fixed(color, 0, 0, flags, format, save_quad, out);
}
'''
test = r'''
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "eng.h"
#include "geo_hw.h"
unsigned g_eng_frame;
void reference_run(const geo_view *, int, int, int, geo_quad *);
void optimized_run(const geo_view *, int, int, int, geo_quad *);
static uint32_t state=22;
static uint32_t next(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int main(void) {
 int32_t points[20];g_eng_pointrom=points;g_eng_pointrom_n=20;
 for(int trial=0;trial<20000;trial++) {
  geo_view v={0};
  for(int i=0;i<3;i++) {
   for(int j=0;j<3;j++)v.m[i][j]=(int16_t)next();
   v.t[i]=(int16_t)next();
  }
  v.zoom_mant=(int16_t)next();v.zoom_shift=next()%16;
  v.vx=(int16_t)next();v.vy=(int16_t)next();
  v.absolute_priority=next()%8;v.cullflip=next()%2;
  v.have_clip=1;v.cl=-320;v.cr=-320;v.cu=-240;v.cd=-240;
  for(int i=0;i<8;i++)points[i]=next()&0xffffff;
  for(int i=8;i<20;i++)points[i]=(int16_t)next();
  if(trial%2==0) {v.t[2]=100000;v.zoom_shift=3;v.zoom_mant=1920;}
  int color=next()&0xffffff,flags=next()&0xfff,format=next()&0x40;
  geo_quad a,b;
  reference_run(&v,color,flags,format,&a);optimized_run(&v,color,flags,format,&b);
  if(memcmp(&a,&b,sizeof a)) {fprintf(stderr,"quad mismatch %d\n",trial);return 1;}
 }
 puts("PASS: 20000 entire polygons match original, including near clipping, guard clipping and culling");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-geometry-') as d:
    paths=[]
    for label,code in [('reference',reference),('optimized',source)]:
        prefix=''.join('#define '+n+' '+label+'_'+n+'\n' for n in names)
        path=Path(d)/(label+'.c')
        path.write_text(prefix + code + wrapper.replace('RUN',label+'_run'))
        paths.append(str(path))
    path=Path(d)/'test.c';path.write_text(test)
    binary=Path(d)/'test'
    subprocess.run(['cc','-O2','-fwrapv','-U__SIZEOF_INT128__','-fsanitize=address,undefined',
                    '-I'+str(root/'engine'),*paths,str(path),str(root/'engine/eng.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
