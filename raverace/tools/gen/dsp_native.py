"""Lower DSP object-record and matrix routines to native C kernels.

This is a deliberately bounded semantic compiler, not an opcode dispatch loop.
Only reviewed straight-line routines are accepted; arithmetic stays in host
locals, RAM accesses are direct and the scheduler accounts at routine boundaries.
Input is the user's own ROM; generated words and code remain build artifacts.
"""
from pathlib import Path
import argparse

# Half-open instruction ranges. Last instruction may branch or return.
KERNELS=[(0x441f,0x4438,'header'),(0x4438,0x444d,'identity'),
         (0x444d,0x446f,'orientation'),(0x4475,0x448b,'link'),
         (0x4476,0x448b,'link'),
         *[(a,a+0x3f,'matrix') for a in (0x4b1e,0x4b5d,0x4b9c,0x4bdb,0x4c1a,0x4c59)],
         (0x5599,0x55bc,'scale'),(0x55bc,0x55cf,'copy')]

def compile_kernel(prog,start,end,kind):
    code=[];pc=start;repeat=0;count=0;last=0
    def emit(s):code.append('    '+s)
    def addr(lo):
        if not lo&0x80:return f'0x{0x200+(lo&127):x}'
        assert (lo>>4)&7 in (0,1,2), hex(lo)
        emit('a = ar[arp];')
        mode=(lo>>4)&7
        if mode:emit('ar[arp] '+ ('++' if mode==2 else '--')+';')
        if lo&8:emit(f'arb=arp; arp={lo&7};')
        return 'a'
    def read(lo):
        a=addr(lo)
        return f'c25_poly_read16(d, (uint16_t)({a}-0x8000))' if lo&128 else f'd->ram[{a}]'
    def write(lo,value):
        # Materialize the source before auto-indexing (SAR can use the AR).
        emit(f'v = (uint16_t)({value});')
        a=addr(lo)
        emit(f'c25_poly_write16(d, (uint16_t)({a}-0x8000), v);' if lo&128 else f'd->ram[{a}] = v;')
    while pc<end:
        op=prog[pc];hi,lo=op>>8,op&255;last=pc;count+=1
        width=2 if hi in (0xd0,0xd2,0xf5,0xff) else 1
        nxt=pc+width
        for it in range(repeat+1):
            if hi==0x55:addr(lo)
            elif hi==0xca:emit(f'acc={lo}u;')
            elif hi==0xcc:emit(f'old=acc; acc+={lo}u; carry=old>acc;')
            elif hi in (0x20,0x21,0x2f):emit(f'acc=(uint32_t)(int32_t)(int16_t)({read(lo)}) << {hi&15};')
            elif hi==0x10:emit(f'v={read(lo)}; old=acc; acc-=(uint32_t)(int32_t)(int16_t)v; carry=!(old<acc);')
            elif hi==0x3c:emit(f't={read(lo)};')
            elif hi==0x38:emit(f'prod=(int32_t)(int16_t)t*(int32_t)(int16_t)({read(lo)});')
            elif hi==0x3e:
                emit(f't={read(lo)}; acc=pm ? (uint32_t)prod*2u : (uint32_t)prod;')
            elif hi==0x60:write(lo,'acc')
            elif hi in (0x68,0x69):write(lo,f'(acc << {hi&7}) >> 16')
            elif 0x70<=hi<=0x77:write(lo,f'ar[{hi&7}]')
            elif hi in (0x30,0x31):emit(f'ar[{hi&7}]={read(lo)};')
            elif hi==0x7e:emit(f'ar[arp]+={lo};')
            elif hi==0x7f:emit(f'ar[arp]-={lo};')
            elif hi==0xcb:
                assert it==0;repeat=lo
            elif hi==0xed:
                assert not lo&128;emit(f'd->bank=d->ram[0x{0x200+lo:x}]&3;')
            elif hi==0xd0:
                assert lo==1;emit(f'acc=(uint32_t)(int32_t)(int16_t)0x{prog[pc+1]:04x};')
            elif hi==0xd2:
                assert lo==0;emit(f'ar[2]=0x{prog[pc+1]:04x};')
            elif op==0xce14:emit('acc=pm ? (uint32_t)prod*2u : (uint32_t)prod;')
            elif op==0xce15:emit('old=acc; acc+=pm ? (uint32_t)prod*2u : (uint32_t)prod; carry=old>acc;')
            elif op in (0xce08,0xce09):emit(f'pm={lo&3};')
            elif op==0xce26:emit('next=d->stack[--d->sp];')
            elif hi==0xf5:
                assert lo==128 and nxt==end
                emit(f'next=acc ? 0x{prog[pc+1]:04x} : 0x{nxt:04x};')
            elif hi==0xff:
                assert lo==128 and nxt==end;emit(f'next=0x{prog[pc+1]:04x};')
            else:raise ValueError(f'unsupported {pc:04x}: {op:04x}')
        if hi!=0xcb:repeat=0
        pc=nxt
    assert pc==end and repeat==0
    guard=''
    if kind=='matrix' or kind in ('copy','scale'):
        guard='d->ar[d->arp] < 0x8000 || d->ar[d->arp] > 0xffe0'
    elif kind=='header':guard='d->ar[5] < 0x8000 || d->ar[5] > 0xffe0'
    elif kind in ('identity','orientation'):
        guard='d->arp != 5 || d->ar[5] < 0x8000 || d->ar[5] > 0xffe0 || d->ar[6] < 0x8000 || d->ar[6] > 0xffe0'
    elif kind=='link':guard='d->arp != 5 || d->ar[5] < 0x8000 || d->ar[5] > 0xffe0 || d->ram[0x246] < 0x8000'
    if prog[last]==0xce26:guard+=' || d->sp <= 0 || d->sp > 64'
    words=','.join(f'0x{x:04x}' for x in prog[start:end])
    body=f'''static int kernel_{start:x}(c71_t *d, long budget) {{
    static const uint16_t expected[]={{ {words} }};
    if (budget < {count} || d->tim <= {count} || {guard} ||
        memcmp(d->prog+0x{start:x},expected,sizeof expected)) return 0;
    uint16_t ar[8], t=d->t, a=0, v=0, next=0x{end:x};
    int arp=d->arp, arb=d->arb, carry=d->c, pm=d->pm;
    uint32_t acc=(uint32_t)d->acc;
    {"uint32_t old;" if any("old=" in line for line in code) else ""}
    int32_t prod=(int32_t)d->p;
    memcpy(ar,d->ar,sizeof ar);
'''+ '\n'.join(code)+f'''
    memcpy(d->ar,ar,sizeof ar); d->arp=arp; d->arb=arb;
    d->acc=(int32_t)acc; d->p=prod; d->t=t; d->c=carry; d->pm=pm;
    d->pc=next; d->cur_pc=0x{last:x}; d->tim-={count}; d->steps+={count};
    return {count};
}}
'''
    return body

def generate(prog):
    out=['#include <string.h>\n#include "c25.h"\n#include "c25_poly_bus.h"\n']
    out += [compile_kernel(prog,a,b,k) for a,b,k in KERNELS]
    out+=['''int rr_dsp_native_run(c71_t *d, long budget) {
    if (d->idle || d->rpt || d->ops || d->arp<0 || d->arp>7 ||
        ((d->imr&8) && d->tim>d->prd) ||
        (!d->intm && ((d->ifr & d->imr) || d->tint_pend)) ||
        c25_hook_pre || c25_hook_post || c25_hook_iter || c25_hook_acc) return 0;
    /* CPU doorbell: immutable during this DSP slice. Stop before timer IRQs. */
    if (d->pc==0x452d && d->dp==0x100 && d->bank==0 && d->sxm==1 &&
        d->prog[0x452d]==0x2000 && d->prog[0x452e]==0xf580 && d->prog[0x452f]==0x452d &&
        (d->poly[0]&0xffff)) {
        long n=budget/2; if(n>d->tim/2)n=d->tim/2;
        if(n>0) {d->acc=(int16_t)d->poly[0];d->cur_pc=0x452e;
            d->steps+=2*n;d->tim-=2*n;return (int)(2*n);}
    }
    /* Native matrix/record ABI. Unusual modes use the instruction reference. */
    if(d->dp!=4 || d->sxm!=1 || d->ovm || d->pm || (int64_t)(int32_t)d->p!=d->p) return 0;
    switch(d->pc) {
''']
    out += [f'    case 0x{a:x}: return kernel_{a:x}(d,budget);' for a,b,k in KERNELS]
    out+=['    default: return 0;\n    }\n}\n']
    return '\n'.join(out)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--roms',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    lanes=[(a.roms/n).read_bytes() for n in ('rv2_prguub.6d','rv2_prgumb.8d','rv2_prglmb.2d','rv2_prgllb.4d')]
    rom=bytearray(4*len(lanes[0]))
    for k,lane in enumerate(lanes):rom[k::4]=lane
    start=0x31a68;count=int.from_bytes(rom[start:start+2],'big')+1
    prog=[0]*65536
    for i in range(count):prog[0x4000+i]=int.from_bytes(rom[start+2+2*i:start+4+2*i],'big')
    a.out.write_text(generate(prog))
