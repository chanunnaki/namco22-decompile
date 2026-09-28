"""Check callback payload lifetime and preservation of GXM queue dependencies."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef struct { volatile unsigned *address; unsigned value; } SceGxmNotification;
typedef void SceGxmDisplayQueueCallback(const void*);
typedef struct { unsigned flags,displayQueueMaxPendingCount; SceGxmDisplayQueueCallback *displayQueueCallback; unsigned displayQueueCallbackDataSize,parameterBufferSize; } SceGxmInitializeParams;
typedef struct { int x; } SceGxmColorSurface;
typedef struct { int x; } SceGxmSyncObject;
static SceGxmNotification frame_fence[3];
static unsigned frame_slot,g_eng_frame;
static bool frame_fence_ready=true,frame_fence_pending[3]={true,true,true};
static unsigned clock_us,waits,callbacks;static bool probe_file,queued;
static SceGxmInitializeParams initialized;
static SceGxmSyncObject *expected_old,*expected_new;
static unsigned char saved[256],expected[64];
static uint64_t sceKernelGetProcessTimeWide(void){return ++clock_us;}
static int sceKernelGetCpuId(void){return 1;}
static int sceGxmNotificationWait(const SceGxmNotification*f){assert(queued);waits++;*f->address=f->value;return 0;}
static FILE *marker(const char *p,const char*m){(void)p;(void)m;return probe_file?tmpfile():NULL;}
#define fopen marker
#define VLOG(...) ((void)0)
'''
source+=(root/'raverace/src/rr_present_trace.inc').read_text()
source+=r'''
int __real_sceGxmInitialize(const SceGxmInitializeParams*p){initialized=*p;return 0;}
int __real_sceGxmPadHeartbeat(const SceGxmColorSurface*s,SceGxmSyncObject*b){assert(s&&b==expected_new);return 0;}
int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject*a,SceGxmSyncObject*b,const void*d){assert(a==expected_old&&b==expected_new);queued=true;memcpy(saved,d,initialized.displayQueueCallbackDataSize);return 0;}
static unsigned payload_size;
static void original_callback(const void*d){assert(!memcmp(d,expected,payload_size));callbacks++;}
int main(void){
 SceGxmSyncObject a={0},b={0};SceGxmColorSurface surface={0};expected_old=&a;expected_new=&b;
 unsigned completed[3]={0};for(int i=0;i<3;i++)frame_fence[i]=(SceGxmNotification){completed+i,0};
 for(unsigned mode=0;mode<2;mode++)for(unsigned size=4;size<=64;size+=60){
  probe_file=mode;payload_size=size;
  SceGxmInitializeParams params={7,2,original_callback,size,0x40000};
  assert(!__wrap_sceGxmInitialize(&params));assert(initialized.flags==7&&initialized.displayQueueMaxPendingCount==2&&initialized.parameterBufferSize==0x40000);
  assert(initialized.displayQueueCallback==traced_display_callback);
  for(unsigned i=0;i<120;i++){
   g_eng_frame++;frame_slot=i%3;frame_fence[frame_slot].value++;present_scene_end=sceKernelGetProcessTimeWide();
   unsigned char data[64];for(unsigned k=0;k<size;k++)expected[k]=data[k]=(i+k)&255;
   unsigned before=waits;queued=false;
   __wrap_sceGxmPadHeartbeat(&surface,&b);assert(waits==before);
   __wrap_sceGxmDisplayQueueAddEntry(&a,&b,data);assert(waits==before+mode);
   memset(data,0,sizeof data); /* Callback must own a copied payload. */
   completed[frame_slot]=frame_fence[frame_slot].value;
   initialized.displayQueueCallback(saved);
  }
 }
 assert(callbacks==480&&atomic_load(&present_submitted)==atomic_load(&present_completed));
 SceGxmInitializeParams big={7,2,original_callback,128,0x40000};
 __wrap_sceGxmInitialize(&big);assert(initialized.displayQueueCallback==original_callback&&initialized.displayQueueCallbackDataSize==128);
 puts("PASS: 480 deferred callback payloads and original sync dependencies preserved; GPU waits occur only in probe mode");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(source)
 subprocess.run(['cc','-std=c11','-O2','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
