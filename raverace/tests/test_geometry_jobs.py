"""Compare actual parallel object collection/emission against the serial list walker."""
import ast, os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
mod=ast.parse((root/'raverace/tests/test_vita_worker.py').read_text())
shim=next(ast.literal_eval(n.value) for n in mod.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
s=(root/'raverace/src/rr_gxm.c').read_text()
code=s[s.index('/* Geometry lanes use separate'):s.index('/* ------------------------------------------------ the frame */')]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "rr_vita_worker.h"
#define SCE_KERNEL_CPU_MASK_USER_2 4
#define VLOG(...) ((void)0)
static uint32_t poly[0x8000];
static uint32_t poly_word(int i){return poly[i&0x7fff];}
static geo_quad output[4096];static int count;
static void push_quad(const geo_quad*q,void*u){(void)u;assert(count<4096);output[count++]=*q;}
'''
test=r'''
static uint32_t seed=22;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static int record(int at,int len,int code){poly[at]=code;poly[at+1]=len;poly[at+len+3]=at+len+4;return at+2;}
int main(void){
 static int32_t points[16384];static geo_quad expected[4096];
 g_eng_pointrom=points;g_eng_pointrom_n=16384;
 eng_list_cfg cfg={ENG_LIST_HEAD_S22,1,0,0,0,0};
 for(int trial=0;trial<200;trial++){
  memset(poly,0,sizeof poly);memset(points,0,sizeof points);
  for(int obj=0;obj<16;obj++){
   points[0x45+obj]=100+obj*2;points[100+obj*2]=1000+obj*256;points[101+obj*2]=-1;
   int start=1001+obj*256,at=start;
   points[at++]=0x0d;points[at++]=0;
   for(int k=0;k<12;k++)points[at++]=(int16_t)next();
   for(int q=0;q<6;q++){
    points[at++]=0x17;points[at++]=next()&0x40;points[at++]=next()&0xfff;points[at++]=next()&0xffffff;
    for(int k=0;k<8;k++)points[at++]=next()&0xffffff;
    for(int k=0;k<12;k++)points[at++]=(int16_t)next();
   }
   points[start-1]=at-start;
  }
  int at=cfg.head,p=record(at,0x15,0xbb0003);
  poly[p+1]=(64<<16)|32;poly[p+2]=32767;poly[p+3]=1<<16;
  poly[p+6]=(0x2e<<16)|1920;
  for(int k=7;k<=10;k++)poly[p+k]=(0x1e<<16)|32767;
  poly[p+12]=poly[p+16]=poly[p+20]=32767;at+=0x15+4;
  for(int obj=0;obj<32;obj++){
   if(obj%3==0){p=record(at,0x10,0x233002);poly[p+1]=next()&0x7fffff;poly[p+3]=(obj%8)<<21;at+=0x10+4;}
   p=record(at,0x0d,0x45+obj%16);
   for(int k=1;k<=9;k++)poly[p+k]=(uint16_t)next();
   poly[p+10]=(int16_t)next();poly[p+11]=(int16_t)next();poly[p+12]=obj%2?100000:(int16_t)next();
   at+=0x0d+4;
  }
  count=0;eng_walk_list(poly_word,&cfg,push_quad,NULL);int n=count;
  assert(n>0);memcpy(expected,output,n*sizeof *output);
  count=0;build_geometry(&cfg);
  assert(count==n);assert(memcmp(expected,output,n*sizeof *output)==0);
 }
 rr_worker_close(&geometry_worker);
 for(int i=0;i<4096;i++)free(geometry_jobs[i].quads);
 puts("PASS: 200 parallel scenes match serial polygon bytes and emission order, including lighting and object flags");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-jobs-') as d:
 d=Path(d);inc=d/'psp2/kernel';inc.mkdir(parents=True);(inc/'threadmgr.h').write_text(shim)
 (d/'test.c').write_text(prefix+code+test)
 subprocess.run(['cc','-O2','-fwrapv','-pthread','-U__SIZEOF_INT128__','-fsanitize=address,undefined',
 '-I'+str(d),'-I'+str(root/'engine'),'-I'+str(root/'raverace/include'),str(d/'test.c'),
 *[str(root/'engine'/f) for f in ['eng.c','geo_hw.c','geo_hw_lane.c','slave_list.c']],'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
