/* Compare the wait shortcut with the real instruction semantics. */
#include <assert.h>
#include <stdio.h>
#include "snd_wait.h"
uint8_t g_snd_bios[0x4000], *g_snd_data;
static uint64_t due;
static uint8_t value;
uint64_t m37710_next_event(const m37710_t *c) { (void)c; return due; }
void m377_sfr_w(m37710_t *c, uint32_t a, uint8_t v) { (void)c;(void)a;(void)v;assert(0); }
static uint8_t read_mem(void *u, uint32_t a) { (void)u;assert(a==0x81);return value; }
static void run(m37710_t *c, uint64_t until, int fast) {
    while(c->cycles < until && (c->pc==0xc0dd || c->pc==0xc0df || c->pc==0xc0ff || c->pc==0xc101)) {
        if(c->cycles>=due) {value=1;due=UINT64_MAX;} /* event before this instruction */
        if(fast && rr_snd_skip_wait(c,until)) continue;
        c->ops=g_snd_bios+(c->pc-0xc000);
        uint8_t op=fetch8(c);c->cycles++;c->use_b=false;
        m377_exec(c,op);c->ops=NULL;
    }
}
static void run_chunks(m37710_t *c, uint64_t end, int fast) {
    while (c->cycles < end) {
        if (c->pc!=0xc0dd && c->pc!=0xc0df && c->pc!=0xc0ff && c->pc!=0xc101) break;
        if (fast && rr_snd_skip_wait_chunks(c,end)) continue;
        uint64_t remaining=end-c->cycles;
        run(c,c->cycles+(remaining<64?remaining:64),0);
    }
}
int main(void) {
    const uint8_t code[]={0xa5,0x81,0xf0,0xfc};
    memcpy(g_snd_bios+0xdd,code,4);memcpy(g_snd_bios+0xff,code,4);
    for(int pc=0;pc<2;pc++)for(int limit=1;limit<256;limit++)for(int event=0;event<256;event++) {
        m37710_t a={0},b;
        a.pc=pc?0xc0ff:0xc0dd;a.ps=M377_M|M377_N|M377_C;a.a=0xabcd;a.read8=read_mem;b=a;
        due=event;value=0;run(&a,limit,0);uint8_t av=value;uint64_t ad=due;
        due=event;value=0;run(&b,limit,1);
        assert(av==value && ad==due && memcmp(&a,&b,sizeof a)==0);
    }
    for (int pc=0;pc<2;pc++) for (int limit=1;limit<512;limit++) for (int event=0;event<512;event++) {
        m37710_t a={0},b;
        a.pc=pc?0xc0ff:0xc0dd;a.ps=M377_M|M377_N|M377_C;a.a=0xabcd;a.read8=read_mem;b=a;
        due=event;value=0;run_chunks(&a,limit,0);uint8_t av=value;uint64_t ad=due;
        due=event;value=0;run_chunks(&b,limit,1);
        assert(av==value && ad==due && memcmp(&a,&b,sizeof a)==0);
    }
    for (int trial=0;trial<1000;trial++) {
        uint64_t limit=10000+trial*79,event=trial*101;
        m37710_t a={0},b;
        a.cycles=trial;a.pc=0xc0dd;a.ps=M377_M;a.read8=read_mem;b=a;
        due=event;value=0;run_chunks(&a,limit,0);uint8_t av=value;uint64_t ad=due;
        due=event;value=0;run_chunks(&b,limit,1);
        assert(av==value && ad==due && memcmp(&a,&b,sizeof a)==0);
    }
    m37710_t c={0};c.pc=0xc0dd;c.ps=M377_M;c.read8=read_mem;
    due=UINT64_MAX;value=1;assert(!rr_snd_skip_wait(&c,100));
    value=0;c.dpr=1;assert(!rr_snd_skip_wait(&c,100));
    c.dpr=0;g_snd_bios[0xdd]=0;assert(!rr_snd_skip_wait(&c,100));
    memcpy(g_snd_bios+0xdd,code,4);
    c.pending=1;assert(!rr_snd_skip_wait_chunks(&c,1000));
    c.pending=0;c.stopped=1;assert(!rr_snd_skip_wait_chunks(&c,1000));
    c.stopped=0;c.unimpl_hit=1;assert(!rr_snd_skip_wait_chunks(&c,1000));
    puts("PASS: 524264 paired-chunk/event comparisons;  130560 wait/event boundaries match instruction execution; guards reject non-waits");
}
