"""Run actual GPU slot acquisition/notification code against delayed completions."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
s=(root/'raverace/src/rr_gxm.c').read_text();a=s.index('static SceGxmNotification frame_fence');b=s.index('static void draw_snapshot',a)
body=s[a:b].replace('#include "rr_present_trace.inc"','static uint64_t present_scene_end;')
hud=(root/'engine/quad_gxm_hud.inc').read_text()
upload=hud[hud.index('static void hud_upload('):hud.index('void quad_gxm_draw_native_hud')]

prefix=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef int SceGxmContext;
typedef struct {volatile unsigned *address;unsigned value;} SceGxmNotification;
static SceGxmContext context,*gxm_context=&context;
static volatile unsigned region[3];
static bool busy[3],buffered;
static unsigned submitted[3],selected,waits,finishes;
static uint64_t sceKernelGetProcessTimeWide(void){return 0;}
#define VLOG(...) ((void)0)
static volatile unsigned *sceGxmGetNotificationRegion(void){return region;}
static bool quad_gxm_buffered(void){return buffered;}
static void quad_gxm_select_frame(unsigned slot){assert(!busy[slot]);selected=slot;}
static int sceGxmNotificationWait(const SceGxmNotification *n){
 unsigned i=n->address-region;assert(i<3 && n->value==submitted[i]);
 busy[i]=false;*n->address=n->value;waits++;return 0;
}
static void vita2d_wait_rendering_done(void){
 for(int i=0;i<3;i++){busy[i]=false;region[i]=submitted[i];}finishes++;
}
int __real_sceGxmEndScene(SceGxmContext *c,const SceGxmNotification *v,const SceGxmNotification *f){
 (void)c;(void)v;
 if(f){unsigned i=f->address-region;assert(i<3 && !busy[i]);busy[i]=true;submitted[i]=f->value;}
 return 0;
}
'''
storage=r'''
typedef struct {void *data;} SceGxmTexture;
static SceGxmTexture hud_chars,hud_map,hud_palette;
static void sceGxmTextureSetData(SceGxmTexture *t,void *p){t->data=p;}
static bool hud_active=true,hud_valid[3],hud_atlas_mode;
static void hud_atlas_upload(const uint8_t*c,const uint8_t*m,const uint8_t*p,unsigned x,unsigned y){(void)c;(void)m;(void)p;(void)x;(void)y;abort();}
static unsigned s_frame_slot;
static uint8_t hud_memory[3*0x40000],hud_previous[3][0x22400];
static float hud_offset[2];
'''
test=r'''
int main(void){
 for(int mode=0;mode<3;mode++){
  memset(busy,0,sizeof busy);memset(frame_fence_pending,0,sizeof frame_fence_pending);
  frame_slot=0;buffered_mode=mode!=0;buffered=mode!=2;frame_fence_ready=false;
  /* Initialize region through production setup, then restore requested mode. */
  buffered_mode=-1;acquire_frame_resources();buffered_mode=mode!=0;
  unsigned old_waits=waits,old_finishes=finishes;
  for(int frame=0;frame<1000;frame++){
   acquire_frame_resources();assert(!busy[selected]);
   __wrap_sceGxmEndScene(gxm_context,NULL,NULL);
   assert(busy[selected]);
  }
  if(mode==1){assert(waits-old_waits==997);assert(finishes==old_finishes);}
  else assert(finishes-old_finishes==1000);
 }
 static uint8_t chars[0x1e000],map[0x2000],pal[1024],expected[3][0x22400];
 srand(22);
 for(int f=0;f<600;f++){
  s_frame_slot=f%3;
  chars[rand()%sizeof chars]=(uint8_t)rand();map[rand()%sizeof map]=(uint8_t)rand();pal[rand()%sizeof pal]=(uint8_t)rand();
  quad_gxm_upload_hud(chars,map,pal,f&1023,(f*3)&1023);
  uint8_t *e=expected[s_frame_slot];memcpy(e,chars,sizeof chars);memcpy(e+0x1e000,map,sizeof map);
  memcpy(e+0x20000,map,sizeof map);memcpy(e+0x22000,pal,sizeof pal);
  for(int i=0;i<3;i++)assert(!memcmp(expected[i],hud_memory+i*0x40000,sizeof expected[i]));
  assert(hud_chars.data==hud_memory+s_frame_slot*0x40000);
  assert(hud_map.data==hud_memory+s_frame_slot*0x40000+0x20000);
  assert(hud_palette.data==hud_memory+s_frame_slot*0x40000+0x22000);
 }
 puts("PASS: 600 dirty HUD updates preserve all three slots and character RAM aliases");
 puts("PASS: 3000 frame handoffs never reuse GPU-owned data; buffered mode waits only for its own slot, serial/fallback drain safely");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-gpu-slots-') as td:
 p=Path(td);(p/'test.c').write_text(prefix+body+storage+upload+test)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
