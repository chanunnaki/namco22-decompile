"""Native DSP kernels vs the original instruction semantics, full state/memory."""
from pathlib import Path
import subprocess,tempfile,os,sys,importlib.util
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('dsp_native',root/'raverace/tools/gen/dsp_native.py');gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
source=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c25.h"
#include "c25_sem.h"
#include "c25_boundary.h"
#include "rr_dsp_state.h"
int rr_dsp_native_run(c71_t*,long);
static unsigned rng=0x225588;
static unsigned random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static bool exec(c71_t*d,int pc){int op=d->prog[pc];d->pc=pc+1;int n=d->rpt+1;d->rpt=0;for(int i=0;i<n;i++)if(!c25_exec(d,pc,op,i))return false;return true;}
static void compare(c71_t*a,c71_t*b,uint32_t*poly_a,uint32_t*poly_b,unsigned entry){
 uint32_t *p=b->poly;b->poly=a->poly;
 if(memcmp(a,b,sizeof *a)||memcmp(poly_a,poly_b,C71_POLY_WORDS*4)){
  fprintf(stderr,"mismatch entry=%04x end=%04x/%04x acc=%08x/%08x p=%llx/%llx bank=%d/%d arp=%d/%d arb=%d/%d c=%d/%d tim=%d/%d steps=%llu/%llu\n",entry,a->pc,b->pc,a->acc,b->acc,(unsigned long long)a->p,(unsigned long long)b->p,a->bank,b->bank,a->arp,b->arp,a->arb,b->arb,a->c,b->c,a->tim,b->tim,(unsigned long long)a->steps,(unsigned long long)b->steps);
  for(unsigned i=0;i<sizeof *a;i++)if(((unsigned char*)a)[i]!=((unsigned char*)b)[i]){fprintf(stderr,"first state byte mismatch offset=%u %02x/%02x\n",i,((unsigned char*)a)[i],((unsigned char*)b)[i]);break;}
  for(int i=0;i<C71_POLY_WORDS;i++)if(poly_a[i]!=poly_b[i]){fprintf(stderr,"first poly mismatch %04x: %08x/%08x\n",i,poly_a[i],poly_b[i]);break;}
  abort();
 }
 b->poly=p;
}
static unsigned render_hash;
static void render(uint16_t v){render_hash=render_hash*33+v;}
static unsigned run(c71_t*d,long budget,int native){
 unsigned covered=0;
 while(budget>0){
  int n=native?rr_dsp_native_run(d,budget):0;
  if(n){budget-=n;covered+=n;continue;}
  int state=c25_run_begin(d,&budget);assert(state>=0);if(!state)continue;
  assert(exec(d,d->cur_pc));
 }
 return covered;
}
int main(int argc,char**argv){
 c71_t *base=calloc(1,sizeof *base),*a=malloc(sizeof *a),*b=malloc(sizeof *b);
 uint32_t *pa=calloc(C71_POLY_WORDS,4),*pb=calloc(C71_POLY_WORDS,4);
 base->poly=pa;base->xlat=exec;
 assert(argc>=2 && c71_load(base,NULL,argv[1]));
 static const unsigned entries[]={ENTRIES,0x452d};
 unsigned accepted=0,rejected=0;
 for(unsigned k=0;k<sizeof entries/sizeof *entries;k++)for(int trial=0;trial<260;trial++){
  c71_reset(base);base->pc=entries[k];base->dp=4;base->sxm=1;base->ovm=base->pm=0;
  base->intm=trial%2;base->tim=trial<80?trial:60000;base->prd=65535;
  base->arp=5;base->arb=random32()%8;base->sp=2;base->stack[1]=0x1234;
  base->acc=random32();base->p=(int32_t)random32();base->t=random32();base->c=trial%2;
  for(int i=0;i<8;i++)base->ar[i]=0x8100+random32()%0x7000;
  for(int i=0;i<65536;i++)base->ram[i]=random32();
  for(int i=0;i<C71_POLY_WORDS;i++)pa[i]=random32();
  base->ram[0x246]=0x8400;base->bank=trial%4;
  if(entries[k]==0x452d){base->dp=0x100;base->bank=0;pa[0]=trial%3?0x8001:0;}
  if(trial%17==0)base->ifr=base->imr=1;
  if(trial%19==0){base->tim=500;base->prd=499;}
  if(trial%23==0)base->rpt=1;
  if(trial%29==0)base->tint_pend=1;
  uint16_t saved_op=base->prog[entries[k]];
  if(trial>=200) switch(trial%12){
   case 0:base->dp=3;break;
   case 1:base->sxm=0;break;
   case 2:base->ovm=1;break;
   case 3:base->pm=1;break;
   case 4:base->arp=8;break;
   case 5:base->imr=8;base->tim=500;base->prd=499;break;
   case 6:base->ar[5]=0x7fff;break;
   case 7:base->ar[5]=0xffe0;break;
   case 8:base->ar[5]=0xffff;break;
   case 9:base->ar[6]=0x7fff;break;
   case 10:base->sp=0;break;
   case 11:base->prog[entries[k]]^=1;break;
  }
  memcpy(a,base,sizeof *a);memcpy(b,base,sizeof *b);a->poly=pa;b->poly=pb;memcpy(pb,pa,C71_POLY_WORDS*4);
  int n=rr_dsp_native_run(b,trial%7?512:1);
  if(n){for(int i=0;i<n;i++)assert(c71_step(a));accepted++;}else rejected++;
  compare(a,b,pa,pb,entries[k]);
  base->prog[entries[k]]=saved_op;
 }
 printf("PASS: %u native kernel cases and %u guard fallbacks match every DSP field and polygon word\n",accepted,rejected);
 if(argc>=4){
  FILE*f=fopen(argv[2],"rb");assert(f&&rr_dsp_state(f,base,0));fclose(f);
  uint32_t *rom=calloc(base->ptrom_words,4);base->ptrom=rom;
  for(int layer=0;layer<3;layer++)for(int chip=0;chip<4;chip++){
   char p[1024];snprintf(p,sizeof p,"%s/rv1pot%c%d.%d%c",argv[3],"lmu"[layer],chip,5-chip,"bcd"[layer]);f=fopen(p,"rb");assert(f);
   for(int i=0;i<0x80000;i++){int x=fgetc(f);assert(x>=0);rom[chip*0x80000+i]|=(unsigned)x<<(layer*8);}fclose(f);
  }
  for(unsigned i=0;i<base->ptrom_words;i++)rom[i]=(uint32_t)((int32_t)(rom[i]<<8)>>8);
  base->render_w=render;
  unsigned reference_hash=0,covered=0;
  /* Different slice sizes force entry/exit in the middle of every native routine. */
  for(int sample=0;sample<12;sample++){
   memcpy(a,base,sizeof *a);memcpy(b,base,sizeof *b);a->poly=pa;b->poly=pb;memcpy(pb,pa,C71_POLY_WORDS*4);
   long budget=sample==11?166667:1000+sample*701;
   render_hash=0;run(a,budget,0);reference_hash=render_hash;
   render_hash=0;covered+=run(b,budget,1);assert(reference_hash==render_hash);
   compare(a,b,pa,pb,base->pc);
   /* Reload initial polygon RAM before the next independent replay. */
   f=fopen(argv[2],"rb");assert(f&&rr_dsp_state(f,base,0));fclose(f);
  }
  printf("PASS: 12 captured race replays match entire DSP state, all RAM and direct render output; %u instructions handled natively\n",covered);
  free(rom);
 }
 free(base);free(a);free(b);free(pa);free(pb);
}
'''.replace('ENTRIES',','.join(hex(a) for a,b,k in gen.KERNELS))
with tempfile.TemporaryDirectory(prefix='rr-dsp-native-') as td:
 p=Path(td);(p/'test.c').write_text(source)
 subprocess.run([sys.executable,str(root/'raverace/tools/gen/dsp_native.py'),'--roms',str(root/'raverace/extracted'),'--out',str(p/'native.c')],check=True)
 lanes=[(root/'raverace/extracted'/n).read_bytes() for n in ('rv2_prguub.6d','rv2_prgumb.8d','rv2_prglmb.2d','rv2_prgllb.4d')];rom=bytearray(4*len(lanes[0]))
 for k,lane in enumerate(lanes):rom[k::4]=lane
 n=int.from_bytes(rom[0x31a68:0x31a6a],'big')+1;(p/'program.bin').write_bytes(rom[0x31a6a:0x31a6a+2*n])
 subprocess.run(['cc','-O2','-fwrapv','-fsanitize=address,undefined','-fno-sanitize=shift','-I'+str(root/'engine/c25'),'-I'+str(root/'raverace/include'),str(p/'test.c'),str(p/'native.c'),str(root/'engine/c25/c25_core.c'),str(root/'engine/c25/c25_bus.c'),'-o',str(p/'test')],check=True)
 args=[str(p/'test'),str(p/'program.bin')]
 if len(sys.argv)>1:args += [sys.argv[1],str(root/'raverace/extracted')]
 subprocess.run(args,check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
