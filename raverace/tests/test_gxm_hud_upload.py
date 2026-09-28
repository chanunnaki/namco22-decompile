"""Check actual dirty-span uploads against full copies, including padded strides."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
s=(root/'raverace/src/rr_gxm.c').read_text();a=s.index('static void upload_text(');b=s.index('/* Palette/HUD work',a)
code=s[a:b]
test=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define NW 640
#define NH 480
static uint8_t txt_rgba[NW*NH*4],txt_uploaded[NW*NH*4],gpu[(NW*4+64)*NH];
static bool txt_pixels_changed,txt_uploaded_valid;
static size_t gpu_bytes;
static void *counted_copy(void *d,const void*s,size_t n){if((uintptr_t)d>=(uintptr_t)gpu && (uintptr_t)d<(uintptr_t)(gpu+sizeof gpu))gpu_bytes+=n;return memcpy(d,s,n);}
#define memcpy counted_copy
CODE
#undef memcpy
int main(void){
 size_t stride=NW*4+64;memset(gpu,0xa5,sizeof gpu);
 for(int f=0;f<100;f++){
  if(f==0 || f%10==0)for(size_t i=0;i<sizeof txt_rgba;i++)txt_rgba[i]=rand();
  else for(int j=0;j<20;j++)txt_rgba[rand()%sizeof txt_rgba]^=0x55;
  txt_pixels_changed=true;gpu_bytes=0;upload_text(gpu,stride);
  if(f%10)assert(gpu_bytes<=20*64);
  for(int y=0;y<NH;y++){
   assert(memcmp(gpu+y*stride,txt_rgba+y*NW*4,NW*4)==0);
   for(int x=NW*4;x<(int)stride;x++)assert(gpu[y*stride+x]==0xa5);
  }
  gpu_bytes=0;upload_text(gpu,stride);assert(gpu_bytes==0);
  txt_pixels_changed=false;upload_text(gpu,stride);assert(gpu_bytes==0);
 }
 puts("PASS: 100 HUD uploads match full copies; sparse edits transfer only dirty spans; stride padding preserved");
}
'''.replace('CODE',code)
with tempfile.TemporaryDirectory(prefix='rr-upload-') as d:
 d=Path(d);(d/'test.c').write_text(test)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
