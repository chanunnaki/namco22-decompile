"""Verify actual atlas uploader updates only its selected GPU resource slot."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
s=(root/'engine/quad_gxm_hud_atlas.inc').read_text();upload=s[s.index('static void hud_atlas_upload'):s.index('static void hud_atlas_draw')]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
'''+(root/'engine/hud_atlas_cache.inc').read_text()+r'''
#define HUD_ATLAS_BYTES (HUD_ATLAS_W*HUD_ATLAS_H*4u)
static uint8_t *hud_memory;static unsigned s_frame_slot,g_eng_frame;
static unsigned hud_atlas_uploaded[3][HUD_CACHE_SLOTS];
typedef struct {void *data;} SceGxmTexture;static SceGxmTexture hud_atlas_tex;
static void sceGxmTextureSetData(SceGxmTexture*t,void*p){t->data=p;}
void vita_log(const char *s,...){(void)s;}
'''
test=r'''
int main(void){
 static uint8_t chars[0x1e000],map[8192],pal[1024];
 hud_memory=calloc(3,HUD_ATLAS_BYTES);uint8_t *before=malloc(3*HUD_ATLAS_BYTES);
 unsigned rng=24;
 for(unsigned i=0;i<sizeof chars;i++){rng=rng*1664525+1013904223;chars[i]=rng>>24;}
 for(unsigned f=0;f<80;f++){
  for(unsigned i=0;i<sizeof map;i++){rng=rng*1664525+1013904223;map[i]=rng>>24;}
  for(unsigned i=0;i<sizeof pal;i++){rng=rng*1664525+1013904223;pal[i]=rng>>24;}
  chars[f%sizeof chars]^=0xff;s_frame_slot=f%3;g_eng_frame=f+1;
  memcpy(before,hud_memory,3*HUD_ATLAS_BYTES);
  hud_atlas_upload(chars,map,pal,f*13&1023,f*19&1023);
  assert(hud_atlas_tex.data==hud_memory+s_frame_slot*HUD_ATLAS_BYTES);
  for(unsigned k=0;k<3;k++)if(k!=s_frame_slot)assert(!memcmp(before+k*HUD_ATLAS_BYTES,hud_memory+k*HUD_ATLAS_BYTES,HUD_ATLAS_BYTES));
  for(unsigned i=0;i<hud_tile_count;i++){
   unsigned slot=hud_tiles[i].slot;unsigned x=(slot&63)*16,y=(slot>>6)*16;
   for(unsigned row=0;row<16;row++)assert(!memcmp(hud_memory+s_frame_slot*HUD_ATLAS_BYTES+((y+row)*HUD_ATLAS_W+x)*4,hud_glyphs[slot].pixels+row*64,64));
  }
 }
 free(before);free(hud_memory);free(hud_glyphs);
 puts("PASS: 80 mutable glyph-cache frames preserve nonselected GPU slots and upload every referenced glyph version correctly");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(prefix+upload+test)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
