/* Portable diagnostic snapshot; host pointers/callbacks are supplied by the
 * replay harness. Fixed little-endian scalars, independent of struct padding. */
#ifndef RR_DSP_STATE_H
#define RR_DSP_STATE_H
#include <stdio.h>
#include <string.h>
#include "c25.h"
#define RR_DSP_SCALARS(X) \
 X(bank) X(latch) X(pt_addr) X(pt_data) X(bioz) X(pc) X(pfc) X(t) X(acc) X(p) \
 X(arp) X(arb) X(dp) X(pm) X(sxm) X(ovm) X(intm) X(c) X(tc) X(cnf) \
 X(imr) X(prd) X(tim) X(tint_pend) X(ptram_base) X(ss22) X(pdp_base) X(idle) \
 X(pdp_begins) X(ifr) X(sp) X(rpt) X(cur_pc) X(steps) X(n_written) \
 X(idle_halts) X(port3_bioz) X(ptrom_words)
static int rr_dsp_scalar(FILE *f, uint64_t *v, int write)
{
    uint8_t b[8];
    if (write) { for(int i=0;i<8;i++) b[i]=(uint8_t)(*v>>(i*8)); return fwrite(b,1,8,f)==8; }
    if(fread(b,1,8,f)!=8) return 0;
    *v=0;for(int i=0;i<8;i++)*v|=(uint64_t)b[i]<<(i*8);return 1;
}
static int rr_dsp_state(FILE *f, c71_t *d, int write)
{
    uint64_t v=0x315053445252u;
    if(!rr_dsp_scalar(f,&v,write)||v!=0x315053445252u) return 0;
#define FIELD(name) v=(uint64_t)d->name; if(!rr_dsp_scalar(f,&v,write))return 0; if(!write)d->name=v;
    RR_DSP_SCALARS(FIELD)
#undef FIELD
#define ARRAY(name, count) for(unsigned j=0;j<(count);j++){v=d->name[j];if(!rr_dsp_scalar(f,&v,write))return 0;if(!write)d->name[j]=v;}
    ARRAY(ar,8) ARRAY(stack,64) ARRAY(prog,0x10000) ARRAY(ram,0x10000)
    ARRAY(poly,C71_POLY_WORDS) ARRAY(ptram,C71_PTRAM_WORDS) ARRAY(written,C71_POLY_WORDS)
#undef ARRAY
    if(!write){d->ops=NULL;memset(d->error,0,sizeof d->error);}
    return 1;
}
#endif
