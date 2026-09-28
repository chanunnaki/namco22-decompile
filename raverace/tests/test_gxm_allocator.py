"""Host regression checks for the actual Vita pool allocator (no SDK required)."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s=(root/'engine/gxm_tex.c').read_text();h=(root/'engine/gxm_tex.h').read_text()
struct=h[h.index('typedef struct'):h.index('bool gxm_tex_init')]
core=s[s.index('#define TEX_POOL_SIZE'):s.index('static SceGxmTexture white_tex')]
# core references the slot type and contains all allocator routines
align=s[s.index('static inline size_t align256'):s.index('bool gxm_tex_init')]
alloc=s[s.index('uint32_t gxm_tex_alloc'):s.index('void gxm_tex_upload')]
free=s[s.index('void gxm_tex_free'):s.index('SceGxmTexture *gxm_tex_get')]
preamble='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
typedef int SceUID; typedef struct { int dummy; } SceGxmTexture;
#define MAX_GXM_TEXTURES 8192
void vita_log(const char *fmt, ...) {(void)fmt;}
'''
test='''
int main(void) {
 pool_size=16*1024*1024; pool_base=malloc(pool_size); units[0]=1;
 memset(pool_base,0x5a,256);
 uint32_t ids[100]; int n=0;
 for(;n<100;n++) {ids[n]=gxm_tex_alloc();gxm_tex_realloc(ids[n],256,256);if(!gxm_tex_valid(ids[n]))break;}
 assert(n==63); // white texture reservation prevents a 64th 256KB allocation
 for(int i=0;i<n;i++) {
  GxmTextureSlot *a=&slots[ids[i]];
  assert((uint8_t*)a->data>=pool_base+256 && (uint8_t*)a->data+a->units*256<=pool_base+pool_size);
  memset(a->data,i,a->units*256);
 }
 gxm_tex_free(ids[0]);
 uint32_t id=gxm_tex_alloc();gxm_tex_realloc(id,256,256);assert(!gxm_tex_valid(id));
 gxm_tex_begin_frame();gxm_tex_realloc(id,256,256);assert(gxm_tex_valid(id));
 for(int i=1;i<n;i++) {unsigned char *p=slots[ids[i]].data;for(size_t j=0;j<slots[ids[i]].units*256;j++)assert(p[j]==i);}
 for(int i=0;i<256;i++)assert(pool_base[i]==0x5a);
 gxm_tex_free(id);gxm_tex_begin_frame();gxm_tex_realloc(id,17,19);assert(slots[id].alloc_w==24);
 memset(slots,0,sizeof slots);next_slot=1;
 for(int i=1;i<MAX_GXM_TEXTURES;i++)assert(gxm_tex_alloc()!=0);
 assert(gxm_tex_alloc()==0);
 puts("PASS: 16MB bounds, exhaustion, deferred reuse, live texture contents, white reservation, stride and handle exhaustion");
 free(pool_base); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rr-allocator-') as d:
    source = Path(d)/'test.c'
    binary = Path(d)/'test'
    source.write_text(preamble+struct+core+align+alloc+free+test)
    subprocess.run(['cc', '-O1', '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
