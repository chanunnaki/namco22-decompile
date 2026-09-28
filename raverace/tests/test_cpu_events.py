"""Compare native CPU event control with actual upstream lifted instructions."""
from pathlib import Path
import importlib.util, subprocess, tempfile, os, re
root=Path(__file__).resolve().parents[2]
src=(root/'raverace/gen/rr_lifted.c').read_text()
spec=importlib.util.spec_from_file_location('cpu_native',root/'raverace/tools/gen/cpu_native.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
assert 'rr_cpu_frame_loop();' in gen.attach(src)
try: gen.attach(src.replace('A_40EE: RR_INS(0x40EEU); /* addq.l #0x1,D0 */','A_40EE: RR_INS(0x40EEU); /* changed */',1))
except ValueError: pass
else: raise AssertionError('changed oracle accepted')

def extract(name,start,end,replace):
    f=src.index('void L_'+name+'_at(')
    declarations=src[f:src.index('  if (pc_',f)]
    declarations=declarations[declarations.index('{')+1:]
    block=src[src.index('A_'+start+':'):src.index(end,src.index('A_'+start+':'))]
    for a,b in replace.items():block=block.replace(a,b)
    return declarations+block
frame=extract('4000','40EE','  if (rd_stop_on && rd_jump_stop(0x4120U',{
 'goto resume_;':'goto A_40EE;', # Stub callees always return at their call site; patched below.
})
# Direct continuations eliminate the large function's resume switch, not instructions.
frame=frame.replace('pc_ = rr_ret_to; goto A_40EE;', 'if (rr_ret_to == 0x410a) goto A_410A; if (rr_ret_to == 0x411c) goto A_411C; abort();')
service=extract('2B056','2B05C','A_2B064:',{})
test=r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rd.h"
#include "rr_cpu.h"
#include "rr_lifted.h"
uint8_t R[RR_REGSPACE];
rr_sys_t g_rr;
int32_t rr_budget;
int rd_poll_rec;
uint32_t rr_ret_to;
static int ticks, frames, interval, service_mode, stop_tick, shadow;
static int testing, observations;
static jmp_buf done;
struct observed { uint8_t r[RR_REGSPACE]; uint8_t wram[RR_WRAM_SIZE]; int budget, ticks, frames, shadow; uint32_t ret; };
static struct observed snapshots[256];
static void observe(void) {
 struct observed s={0}; memcpy(s.r,R,sizeof R);memcpy(s.wram,g_rr.wram,sizeof s.wram);
 s.budget=rr_budget;s.ticks=ticks;s.frames=frames;s.shadow=shadow;s.ret=rr_ret_to;
 assert(observations<256);
 if(!testing) snapshots[observations]=s;
 else if(memcmp(&snapshots[observations],&s,sizeof s)) {
  fprintf(stderr,"mismatch observation=%d ticks=%d frames=%d budget=%d/%d D0=%x\n",observations,ticks,frames,snapshots[observations].budget,rr_budget,d_reg(0));
  for(int i=0;i<RR_REGSPACE;i++)if(s.r[i]!=snapshots[observations].r[i])fprintf(stderr,"reg byte %x: %x/%x\n",i,snapshots[observations].r[i],s.r[i]);
  abort();
 }
 observations++;
}
uint32_t rr_read(vaddr_t a,int size) {
 assert((a&0xf8000000)==0x10000000 || (a&0xf8000000)==0x18000000);
 unsigned o=a&0x7ffffff;assert(o+size<=RR_WRAM_SIZE);
 uint32_t v=0;for(int i=0;i<size;i++)v=(v<<8)|g_rr.wram[o+i];return v;
}
void rr_write(vaddr_t a,int size,uint32_t v) {
 assert((a&0xf8000000)==0x10000000 || (a&0xf8000000)==0x18000000);
 unsigned o=a&0x7ffffff;assert(o+size<=RR_WRAM_SIZE);
 for(int i=size-1;i>=0;i--){g_rr.wram[o+i]=v;v>>=8;}
}
void rr_tick(void) {
 observe();ticks++;rr_budget=interval;
 if(service_mode) {if(ticks==stop_tick)vwr16(a_reg(6)-0x77fa,1);}
 else if(ticks%3==0) vwr16(a_reg(6)-0x77fa,0);
 /* Model an IRQ restoring CPU registers but changing the wait word and RAM. */
 vwr32(a_reg(6)+0x100,ticks);
 if(ticks%2==0) {set_d(0,d_reg(0)^0x80000000u);set_a(6,a_reg(6)^0x08000000u);}
 assert(ticks<250);
}
void rd_poll_snap(void) {assert(0);}
int rr_call_push(uint32_t ret) {rr_ret_to=ret;return shadow++;}
int rr_after_call(int j) {assert(j==shadow-1);shadow--;return 0;}
void rr_jump(uint32_t pc,uint32_t at) {(void)pc;(void)at;abort();}
static void callee_return(void){rr_ret_to=vrd32(a_reg(7));set_a(7,a_reg(7)+4);RS4(0x50,rr_ret_to);}
void L_4120_at(uint32_t pc) {
 assert(pc==0x4120);observe();
 set_d(0,0xfffffff0u+frames);set_d(3,frames*17);vwr32(a_reg(6)+0x104,frames);
 charge(13+frames%5);RR_POLL();callee_return();
}
void L_4D52_at(uint32_t pc) {
 assert(pc==0x4d52);observe();charge(7);RR_POLL();callee_return();
 if(++frames==4)longjmp(done,1);
}
static void reference_frame(void) { FRAME }
static void reference_service(void) { SERVICE }
static uint32_t rng=0x220020;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void) {
 unsigned cases=0;
 for(int trial=0;trial<180;trial++)for(int kind=0;kind<2;kind++) {
  uint8_t initial[RR_REGSPACE];for(unsigned i=0;i<sizeof initial;i++)initial[i]=next();
  const uint32_t d0[]={0,1,0x7ffffffe,0x7fffffff,0xfffffffe,0xffffffff};
  interval=trial<100?1+trial:5187+trial%3;service_mode=kind;stop_tick=1+trial%7;
  int baseline_obs=0;
  for(testing=0;testing<2;testing++) {
   memcpy(R,initial,sizeof R);memset(&g_rr,0,sizeof g_rr);
   ticks=frames=observations=shadow=0;rr_ret_to=0;rr_budget=interval-(trial%11);
   set_a(6,trial%2?0x18008000:0x10008000);set_a(7,0x1001f000);set_d(0,d0[trial%6]);
   vwr16(a_reg(6)-0x77fa,kind?(trial%2?0:3):(trial%3?(trial%2?0x8000:1):0));
   if(!setjmp(done)) {
    if(kind){if(testing)rr_cpu_wait_service();else reference_service();}
    else {if(testing)rr_cpu_frame_loop();else reference_frame();}
   }
   observe();
   if(!testing)baseline_obs=observations;else assert(observations==baseline_obs);
  }
  cases++;
 }
 printf("PASS: %u full controller/wait runs match lifted registers, WRAM, stacks, budgets and every scheduler/call boundary\n",cases);
}
'''.replace('FRAME',frame).replace('SERVICE',service)
with tempfile.TemporaryDirectory(prefix='rr-cpu-events-') as td:
 p=Path(td);(p/'test.c').write_text(test)
 subprocess.run(['cc','-O1','-fwrapv','-fsanitize=address,undefined','-I'+str(root/'raverace/include'),'-I'+str(root/'raverace/gen'),str(p/'test.c'),str(root/'raverace/src/rr_cpu.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
