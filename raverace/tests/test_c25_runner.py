"""Gate linked execution against the original step runner, including bus state."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
gen=(root/'raverace/gen/rr_c25.c').read_text()
variants=[tuple(int(x,16) if x else -1 for x in row) for row in re.findall(r'if \(d->prog\[0x([0-9A-F]+)\] == 0x([0-9A-F]+)(?: && d->prog\[0x[0-9A-F]+\] == 0x([0-9A-F]+))?\)',gen)]
board=(root/'raverace/src/rr_dsp.c').read_text()
a=board.index('    while (steps > 0) {',board.index('void rr_dsp_run'))
b=board.index('\nvoid rr_dsp_vblank',a)
loop=board[a:b].replace('m->','d->').replace('c71_step(m)','c71_step(d)').replace('faulted = true; return;','return false;')
loop=loop[:loop.rfind('}')]+ '    return true;\n}\n'
test=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c25.h"
bool rr_c25_exec(c71_t*,int);
bool rr_c25_exec_run(c71_t*,long);
static bool reference(c71_t*d,long steps) { LOOP
static const int variants[][3]={VARIANTS};
static uint32_t rng=22;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static unsigned callbacks;
static void pre(c71_t*d,int pc){callbacks=callbacks*33+pc+d->tim;}
static void post(c71_t*d){callbacks=callbacks*33+d->pc+d->acc;}
static void iter(void){callbacks++;}
int main(int argc,char**argv){
 c71_t *base=calloc(1,sizeof *base),*a=malloc(sizeof *a),*b=malloc(sizeof *b);
 static uint32_t poly[C71_POLY_WORDS],saved[C71_POLY_WORDS],rom[256];
 for(int i=0;i<256;i++)rom[i]=next()&0xffffff;
 base->poly=poly;base->ptrom=rom;base->ptrom_words=256;base->ptram_base=C71_PTRAM_S22;
 base->idle_halts=base->port3_bioz=1;base->xlat=rr_c25_exec;
 assert(argc==2 && c71_load(base,argv[1],NULL));
 unsigned tests=0;
 for(unsigned k=0;k<sizeof variants/sizeof *variants+300;k++) {
  c71_reset(base);base->xlat=rr_c25_exec;
  long budget;
  if(k<sizeof variants/sizeof *variants){
   base->pc=variants[k][0];base->prog[base->pc]=variants[k][1];
   if(variants[k][2]>=0)base->prog[(base->pc+1)&65535]=variants[k][2];
   budget=1+k%19;base->sp=k%65;
   base->rpt=k%4;base->intm=k%2;base->ifr=k%64;
   base->imr=k%64;base->tim=k%7;base->prd=31;base->idle=k%11==0;base->tint_pend=k%13==0;
   for(int i=0;i<8;i++)base->ar[i]=next();
  }else{base->pc=0;budget=1000+(k%17)*137;}
  memcpy(a,base,sizeof *a);memcpy(b,base,sizeof *b);memset(poly,0,sizeof poly);
  c25_hook_pre=k%2?pre:NULL;c25_hook_post=k%2?post:NULL;c25_hook_iter=k%2?iter:NULL;
  callbacks=0;bool ra=reference(a,budget);unsigned ca=callbacks;memcpy(saved,poly,sizeof poly);memset(poly,0,sizeof poly);
  callbacks=0;bool rb=rr_c25_exec_run(b,budget);
  if(ra!=rb || ca!=callbacks || memcmp(a,b,sizeof *a) || memcmp(saved,poly,sizeof poly)){
   fprintf(stderr,"runner mismatch case=%u pc=%04x budget=%ld result=%d/%d end=%04x/%04x steps=%llu/%llu\n",k,base->pc,budget,ra,rb,a->pc,b->pc,(unsigned long long)a->steps,(unsigned long long)b->steps);return 1;
  }
  tests++;
 }
 printf("PASS: %u linked DSP runs match step execution, full memory, timer/IRQ/idle/RPT state, hooks and faults\n",tests);
 free(a);free(b);free(base);
}
'''.replace('LOOP',loop).replace('VARIANTS',','.join('{'+','.join(str(x) for x in row)+'}' for row in variants))
# Suppress expected fault messages from the reference's board wrapper.
test=re.sub(r'\s*fprintf\(stderr, "\[DSP\].*?;', '', test)
with tempfile.TemporaryDirectory(prefix='rr-c25-runner-') as td:
 d=Path(td);p=d/'test.c';p.write_text(test)
 subprocess.run(['cc','-O0','-fwrapv','-fsanitize=address,undefined','-fno-sanitize=shift',
 '-I'+str(root/'engine/c25'),str(p),str(root/'raverace/gen/rr_c25.c'),str(root/'raverace/gen/rr_c25_run.c'),
 str(root/'engine/c25/c25_core.c'),str(root/'engine/c25/c25_bus.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test'),str(root/'raverace/extracted/c71.bin')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
