"""Compare cached tile geometry/pixels with independent full-screen HUD decoding."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
s=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
'''+(root/'engine/hud_atlas_cache.inc').read_text()+r'''
static unsigned rng=2026;
static unsigned next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 static unsigned char chars[0x1e000],map[8192],pal[1024],image[640*480*4];
 for(unsigned i=0;i<sizeof chars;i++)chars[i]=next();
 for(unsigned frame=0;frame<100;frame++){
  for(unsigned i=0;i<sizeof map;i++)map[i]=next();
  for(unsigned i=0;i<sizeof pal;i++)pal[i]=next();
  if(frame%4==0)memset(chars+(next()%960)*128,255,128);
  if(frame%7==0)for(unsigned i=0;i<128;i++)chars[i]=next();
  unsigned sx=next()&1023,sy=next()&1023;
  hud_cache_build(chars,map,pal,sx,sy);memset(image,0,sizeof image);
  for(unsigned i=0;i<hud_tile_count;i++){
   hud_cached_tile t=hud_tiles[i];hud_cached_glyph *g=hud_glyphs+t.slot;
   for(int y=t.y0;y<t.y1;y++)for(int x=t.x0;x<t.x1;x++){
    float fx=(x+0.5f-t.x0)/(t.x1-t.x0),fy=(y+0.5f-t.y0)/(t.y1-t.y0);
    int u=(int)floorf((t.u0+(t.u1-t.u0)*fx)*HUD_ATLAS_W)-(t.slot&63)*16;
    int v=(int)floorf((t.v0+(t.v1-t.v0)*fy)*HUD_ATLAS_H)-(t.slot>>6)*16;
    assert(u>=0&&u<16&&v>=0&&v<16);
    memcpy(image+(y*640+x)*4,g->pixels+(v*16+u)*4,4);
   }
  }
  for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++){
   unsigned px=(x+sx)&1023,py=(y+sy)&1023,at=((py/16)*64+px/16)*2;
   unsigned hi=map[at],code=((hi&3)<<8)|map[at+1],u=px&15,v=py&15;
   if(hi&4)u=15-u;if(hi&8)v=15-v;
   const unsigned char *src=code<960?chars+code*128:map+(code-960)*128;
   unsigned pen=(src[v*8+u/2]>>((u&1)?0:4))&15;
   unsigned char *got=image+(y*640+x)*4;
   assert(got[3]==(pen==15?0:255));
   if(pen!=15)assert(!memcmp(got,pal+((hi>>4)*16+pen)*4,3));
  }
  unsigned n=hud_tile_count;hud_cache_build(chars,map,pal,sx,sy);
  assert(hud_rebuilt==0&&hud_tile_count==n);
 }
 free(hud_glyphs);
 puts("PASS: 30,720,000 HUD pixels match with scroll, flips, palette/character updates, RAM aliases, transparency and cache eviction; unchanged frames decode no glyphs");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(s)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
