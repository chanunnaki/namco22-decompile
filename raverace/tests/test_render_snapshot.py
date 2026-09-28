"""Renderer inputs remain byte-identical and isolated while emulation advances."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
s=(root/'raverace/src/rr_gxm.c').read_text()
a=s.index('typedef struct {\n    uint32_t poly[RR_POLY_WORDS]');b=s.index('static rr_vita_worker render_worker',a)
record=s[a:b]
a=s.index('static void capture_frame(');b=s.index('/* ---------------- GXM Platform',a)
capture=s[a:b]
a=s.index('static int32_t pointram_read(');b=s.index('\nbool rr_gxm_init',a)
lookup=s[a:b]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"
#include "c25.h"
#include "rr_scene.h"
rr_sys_t g_rr;
static uint32_t live_points[C71_PTRAM_WORDS];
void rr_dsp_copy_pointram(uint32_t*d){memcpy(d,live_points,sizeof live_points);}
RECORD
CAPTURE
LOOKUP
int main(void){
 rr_render_snapshot *saved=malloc(sizeof *saved);
 uint32_t state=22;
 for(int frame=0;frame<40;frame++){
  unsigned char*p=(unsigned char*)&g_rr;
  for(size_t i=0;i<sizeof g_rr;i++){state=state*1664525+1013904223;p[i]=state>>24;}
  for(int i=0;i<C71_PTRAM_WORDS;i++)live_points[i]=state+(uint32_t)i*31337u;
  uint16_t commands[4096][0x1c];int n=frame==0?4096:frame;
  for(int i=0;i<n;i++){for(int j=0;j<0x1c;j++)commands[i][j]=state+i+j;rr_scene_direct_poly(commands[i]);}
  rr_scene_force_walk();capture_frame(true);
#define SAME(field) assert(memcmp(frame_mem.field,g_rr.field,sizeof frame_mem.field)==0)
  SAME(poly);SAME(czram);SAME(mixer);SAME(pal);SAME(cgram);SAME(text);SAME(tilemapattr);
#undef SAME
  assert(frame_mem.walk && frame_mem.direct_count==n && rr_scene_direct_count()==0);
  assert(memcmp(frame_mem.direct,commands,n*sizeof commands[0])==0);
  assert(memcmp(frame_mem.point_ram,live_points,sizeof live_points)==0);
  memcpy(saved,&frame_mem,sizeof *saved);
  memset(&g_rr,0x99,sizeof g_rr);memset(live_points,0xff,sizeof live_points);
  rr_scene_render_refresh();rr_scene_direct_poly(commands[0]);
  assert(memcmp(saved,&frame_mem,sizeof *saved)==0);
  for(int i=0;i<C71_PTRAM_WORDS;i+=97)assert(pointram_read(C71_PTRAM_S22+i)==(int32_t)(saved->point_ram[i]<<8)>>8);
  assert(pointram_read(C71_PTRAM_S22-1)==-1 && pointram_read(C71_PTRAM_S22+C71_PTRAM_WORDS)==-1);
  rr_scene_consume();
 }
 free(saved);puts("PASS: 40 complete frame snapshots preserve all renderer inputs and remain isolated from subsequent emulation writes");
}
'''.replace('RECORD',record).replace('CAPTURE',capture).replace('LOOKUP',lookup)
with tempfile.TemporaryDirectory(prefix='rr-snapshot-') as td:
 d=Path(td);(d/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(root/'raverace/include'),'-I'+str(root/'engine/c25'),str(d/'test.c'),str(root/'raverace/src/rr_scene.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
