"""Exercise the production worker handoff using host pthreads as Vita primitives."""
from pathlib import Path
import subprocess, tempfile, os
root = Path(__file__).resolve().parents[2]
shim = r'''
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef int SceUID; typedef size_t SceSize;
static struct { pthread_mutex_t lock; pthread_cond_t cond; int value; } sems[16];
static int ns, fail_create;
static pthread_t thread;
static int (*entry)(SceSize,void *);
static unsigned char args[32]; static SceSize args_size;
static int sceKernelCreateSema(const char *n,int a,int v,int m,void *p) {
 (void)n;(void)a;(void)m;(void)p;if(fail_create)return -1;
 int i=++ns;assert(i<16);pthread_mutex_init(&sems[i].lock,0);pthread_cond_init(&sems[i].cond,0);sems[i].value=v;return i;
}
static int sceKernelWaitSema(int i,int n,void *p){(void)p;pthread_mutex_lock(&sems[i].lock);while(sems[i].value<n)pthread_cond_wait(&sems[i].cond,&sems[i].lock);sems[i].value-=n;pthread_mutex_unlock(&sems[i].lock);return 0;}
static int sceKernelSignalSema(int i,int n){pthread_mutex_lock(&sems[i].lock);sems[i].value+=n;assert(sems[i].value<=1);pthread_cond_signal(&sems[i].cond);pthread_mutex_unlock(&sems[i].lock);return 0;}
static int sceKernelDeleteSema(int i){pthread_cond_destroy(&sems[i].cond);pthread_mutex_destroy(&sems[i].lock);return 0;}
static void *start_entry(void *p){(void)p;entry(args_size,args);return 0;}
static int sceKernelCreateThread(const char *n,int(*e)(SceSize,void*),int p,int s,int a,int mask,void *opt){(void)n;(void)p;(void)s;(void)a;(void)mask;(void)opt;entry=e;return 1;}
static int sceKernelStartThread(int id,SceSize s,void *p){(void)id;assert(s<=sizeof args);args_size=s;memcpy(args,p,s);return pthread_create(&thread,0,start_entry,0);}
static int sceKernelWaitThreadEnd(int id,void*a,void*b){(void)id;(void)a;(void)b;return pthread_join(thread,0);}
static int sceKernelDeleteThread(int id){(void)id;return 0;}
'''
test = r'''
#include <stdio.h>
#include "rr_vita_worker.h"
static struct {unsigned input, output[1024];} data;
static void job(void *p){assert(p==&data);for(int i=0;i<1024;i++)data.output[i]=data.input+(unsigned)i;}
int main(void){
 for(int fail=0;fail<2;fail++) {
  rr_vita_worker w;fail_create=fail;
  assert(rr_worker_init(&w,"test",1)==!fail);
  for(unsigned j=0;j<5000;j++) {
   data.input=j;rr_worker_dispatch(&w,job,&data);rr_worker_join(&w);
   for(int i=0;i<1024;i++)assert(data.output[i]==j+(unsigned)i);
  }
  rr_worker_dispatch(&w,job,&data);rr_worker_close(&w); /* close joins a pending job */
 }
 puts("PASS: 10000 job handoffs, complete output visibility, pending shutdown and synchronous fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='rr-worker-') as d:
    d=Path(d);inc=d/'psp2/kernel';inc.mkdir(parents=True)
    (inc/'threadmgr.h').write_text(shim)
    (d/'test.c').write_text(test)
    subprocess.run(['cc','-O2','-pthread','-fsanitize=address,undefined','-I'+str(d),
                    '-I'+str(root/'raverace/include'),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
