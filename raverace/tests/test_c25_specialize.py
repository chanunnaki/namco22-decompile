"""Compare constant-opcode specialization with runtime decoding for every used DSP opcode."""
from pathlib import Path
import re, subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
gen=(root/'raverace/gen/rr_c25.c').read_text()
ops=sorted(set(int(x,16) for x in re.findall(r'RUN\(0x[0-9A-F]+, 0x([0-9A-F]+)',gen)))
reference='#include "c25_sem.h"\nbool reference_run(c71_t*d,int pc,int op,int it){return c25_exec(d,pc,op,it);}\n'
optimized='#define C25_SPECIALIZE 1\n#include "c25_sem.h"\nbool specialized_run(c71_t*d,int pc,int op,int it){switch(op){\n'
optimized+=''.join(f'case 0x{x:04x}:return c25_exec(d,pc,0x{x:04x},it);\n' for x in ops)+'default:return false;}}\n'
test=r'''
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "c25.h"
bool reference_run(c71_t*,int,int,int);bool specialized_run(c71_t*,int,int,int);
static uint32_t state=22;
static uint32_t next(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
static const int opcodes[]={OPS};
int main(void){
 c71_t *base=calloc(1,sizeof *base),*a=malloc(sizeof *a),*b=malloc(sizeof *b);
 static uint32_t poly[C71_POLY_WORDS],saved[C71_POLY_WORDS],rom[256];
 for(int i=0;i<256;i++)rom[i]=next()&0xffffff;
 base->poly=poly;base->ptrom=rom;base->ptrom_words=256;base->ptram_base=C71_PTRAM_S22;
 base->idle_halts=base->port3_bioz=1;
 for(int i=0;i<0x10000;i++)base->prog[i]=base->ram[i]=next();
 for(unsigned k=0;k<sizeof opcodes/sizeof *opcodes;k++)for(int trial=0;trial<4;trial++){
  base->pc=0x4001;base->acc=(int32_t)next();base->p=(int16_t)next();base->t=next();
  base->arp=next()%8;base->arb=next()%8;base->dp=next()%512;base->pm=next()%4;
  base->sxm=next()%2;base->ovm=next()%2;base->c=next()%2;base->tc=next()%2;base->cnf=next()%2;
  base->sp=8;base->rpt=0;base->bank=next()%3;base->bioz=next()%2;
  for(int i=0;i<8;i++)base->ar[i]=next();
  memset(poly,0,sizeof poly);memcpy(a,base,sizeof *a);memcpy(b,base,sizeof *b);
  bool ra=reference_run(a,0x4000,opcodes[k],trial%2);
  memcpy(saved,poly,sizeof poly);memset(poly,0,sizeof poly);
  bool rb=specialized_run(b,0x4000,opcodes[k],trial%2);
  if(ra!=rb || memcmp(a,b,sizeof *a) || memcmp(poly,saved,sizeof poly)){
   fprintf(stderr,"DSP specialization mismatch op=%04x trial=%d\n",opcodes[k],trial);return 1;
  }
 }
 printf("PASS: %zu DSP opcode/state cases preserve registers, memory, bus writes and fault results\n",4*sizeof opcodes/sizeof *opcodes);
 free(a);free(b);free(base);
}
'''.replace('OPS',','.join(hex(x) for x in ops))
with tempfile.TemporaryDirectory(prefix='rr-c25-test-') as d:
 d=Path(d)
 paths=[]
 for name,code in [('reference',reference),('specialized',optimized),('test',test)]:
  p=d/(name+'.c');p.write_text(code);paths.append(str(p))
 subprocess.run(['cc','-O1','-fwrapv','-fsanitize=address,undefined','-fno-sanitize=shift',
 '-I'+str(root/'engine/c25'),*paths,str(root/'engine/c25/c25_bus.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
