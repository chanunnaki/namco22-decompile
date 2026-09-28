/*
 * rr_dsp.c -- the System 22 master DSP, the way MAME's namcos22.cpp wires it.
 *
 *  syscon 0x1A (syscon_dspcontrol): 0 holds the master and slave in reset,
 *    1 runs both with the DSP interrupts on, 0xFF runs the master alone
 *    (the 68K's code-upload mode). Leaving reset restarts the C71 at its
 *    BIOS reset vector; the program the 68K uploaded survives in extram.
 *  INT0 at every vblank and RINT/XINT from a 100 Hz serial pulse, both only
 *    while the DSP interrupts are on (HOLD_LINE: pending until taken).
 *  Point RAM at 0xF00000 (Super System 22 moved it to 0xF80000).
 *  The slave DSP is not run -- MAME does not run it either; its job
 *  (walking the master's display list) is done by the renderer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"
#include "c25.h"               /* the shared engine's master DSP (../engine/c25) */
#ifdef RR_ORACLE
#include "c25_oracle.h"
#endif
#include "rr_dsp.h"
#include "rr_video.h"
#include "rr_scene.h"
#include "rr_dsp_state.h"

int32_t  *g_pointrom;
uint32_t  g_pointrom_words;

static c71_t *m;
static uint8_t ctrl;          /* last syscon 0x1A value */
static bool running, irq_on, faulted, slave_on;
static uint16_t rbuf[0x1C];
static int rbuf_n, upload_state;            /* MAME m_RenderBufSize, m_dsp_upload_state */

static void render_w(uint16_t v)
{
    if (rbuf_n < 0x1C) { rbuf[rbuf_n++] = v; if (rbuf_n == 0x1C) rr_scene_direct_poly(rbuf); }
}
static void render_reset(void) { rbuf_n = 0; rr_scene_render_refresh(); }
static void port3_r(void) { upload_state = 0; }
extern bool g_rr_in_vblank;
static void pdp_begin(void) { rr_scene_pdp_begin(g_rr_in_vblank); }
static void slave_w(uint16_t v)          /* upload_code_to_slave_dsp_w: only the enables matter */
{
    if (upload_state == 0) {
        if (v == 0) slave_on = false;
        else if (v == 1) upload_state = 1;
        else if (v == 3 || v == 0x10) slave_on = true;
    } else if (upload_state == 1) upload_state = 2;   /* destination, then data until port 3 read */
}
static uint32_t seen_begins;

#ifdef __vita__
static int profile_on;
static int native_on=1;
static uint64_t native_instructions;
int rr_dsp_native_run(c71_t *, long);
static uint32_t profile_hits[0x10000], profile_iters[0x10000];
static unsigned profile_pc;
static void profile_pre(c71_t *d, int pc) { (void)d; profile_pc=(unsigned)pc; profile_hits[profile_pc]++; }
static void profile_iter(void) { profile_iters[profile_pc]++; }
#endif

void rr_dsp_capture_frame(unsigned frame)
{
#ifdef __vita__
    if (!profile_on || !m) return;
    if (frame == 1020) {
        memset(profile_hits,0,sizeof profile_hits); memset(profile_iters,0,sizeof profile_iters);
    }
    if (frame == 1080) {
        extern void vita_log(const char *, ...);
        FILE *f=fopen("ux0:/data/raverace_dsp_state.bin","wb");
        int ok=f && rr_dsp_state(f,m,1); if(f)fclose(f);
        vita_log("[DSP_CAPTURE] frame=%u saved=%d pc=%04x\n",frame,ok,m->pc);
        f=fopen("ux0:/data/raverace_dsp_profile.csv","w");
        if(f) {
            fprintf(f,"pc,op,next,hits,iterations\n");
            for(unsigned i=0;i<0x10000;i++)if(profile_hits[i])
                fprintf(f,"%04x,%04x,%04x,%u,%u\n",i,m->prog[i],m->prog[(i+1)&65535],profile_hits[i],profile_iters[i]);
            fclose(f);
        }
    }
#else
    (void)frame;
#endif
}


static bool load_pointrom(const char *dir)
{
    static const char *pl[3][4] = {
        {"rv1potl0.5b", "rv1potl1.4b", "rv1potl2.3b", "rv1potl3.2b"},
        {"rv1potm0.5c", "rv1potm1.4c", "rv1potm2.3c", "rv1potm3.2c"},
        {"rv1potu0.5d", "rv1potu1.4d", "rv1potu2.3d", "rv1potu3.2d"} };
    const uint32_t chip = 0x80000, n = 4 * chip;
    uint8_t *b = malloc(3 * n);
    g_pointrom = malloc(n * sizeof *g_pointrom);
    for (int p = 0; p < 3; p++)
        for (int c = 0; c < 4; c++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, pl[p][c]);
            FILE *f = fopen(path, "rb");
            if (!f || fread(b + p * n + c * chip, 1, chip, f) != chip) {
                fprintf(stderr, "[DSP] cannot read %s\n", path);
                if (f) fclose(f);
                free(b); return false;
            }
            fclose(f);
        }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)b[2 * n + i] << 16 | (uint32_t)b[n + i] << 8 | b[i];
        g_pointrom[i] = (int32_t)(v << 8) >> 8;
    }
    g_pointrom_words = n;
    free(b);
    return true;
}

bool rr_dsp_init(const char *dir)
{
    if (!load_pointrom(dir)) return false;
    m = calloc(1, sizeof *m);
    char bios[1024];
    snprintf(bios, sizeof bios, "%s/c71.bin", dir);
    if (!c71_load(m, bios, NULL)) { fprintf(stderr, "[DSP] no BIOS at %s\n", bios); return false; }
    m->poly = g_rr.poly;
    m->ptrom = (const uint32_t *)(const void *)g_pointrom;
    m->ptrom_words = g_pointrom_words;
    m->ptram_base = C71_PTRAM_S22;
    m->ss22 = 0;
    m->idle_halts = 1;                    /* TI IDLE: INTM 0, halt until an interrupt */
    m->port3_bioz = 1;
    /* THE PROGRAM: translated to C at build time (gen/rr_c25.c). The oracle
     * builds can run the interpreter instead (RR_C25=oracle) -- the gate. */
    { extern bool rr_c25_exec(c71_t *, int); m->xlat = rr_c25_exec; }
#ifdef RR_ORACLE
    { const char *e = getenv("RR_C25"); if (e && !strcmp(e, "oracle")) { c25_oracle_use(m); fprintf(stderr, "[DSP] master program: the interpreter ORACLE\n"); } }
#endif
    m->render_w = render_w; m->render_reset = render_reset; m->pdp_begin = pdp_begin; m->slave_w = slave_w; m->port3_r = port3_r;
#ifdef __vita__
    FILE *legacy=fopen("ux0:/data/raverace_dsp_legacy.enable","rb");
    if(legacy){fclose(legacy);native_on=0;}
    {extern void vita_log(const char *, ...);vita_log("[DSP_NATIVE] enabled=%d\n",native_on);}
    FILE *prof=fopen("ux0:/data/raverace_dsp_profile.enable","rb");
    if(prof) {fclose(prof);profile_on=1;c25_hook_pre=profile_pre;c25_hook_iter=profile_iter;}
#endif
    return true;
}

static void start(void)
{
    c71_reset(m);
    m->ptram_base = C71_PTRAM_S22;
    m->pc = 0x0000;                      /* the BIOS reset vector */
    m->render_w = render_w; m->render_reset = render_reset; m->pdp_begin = pdp_begin; m->slave_w = slave_w; m->port3_r = port3_r;
    faulted = false;
}

void rr_dsp_control(uint8_t v)
{
    if (!m || v == ctrl) return;         /* MAME: no-op when unchanged */
    ctrl = v;
    if (v == 0) { running = false; irq_on = false; slave_on = false; }
    else if (v == 1) { if (!running) start(); running = true; irq_on = true; slave_on = true; }
    else if (v == 0xFF) { if (!running) start(); running = true; irq_on = false; }
}

void rr_dsp_run(long steps)
{
    if (!m || !running || faulted) return;
#ifdef __vita__
    /* Retained linked-runner experiment; native kernels take precedence. */
    static int linked = -1;
    if (linked < 0) {
        FILE *f = fopen("ux0:/data/raverace_linked_dsp.enable", "rb");
        linked = f != NULL;
        if (f) fclose(f);
    }
    if (linked && !native_on) {
        extern bool rr_c25_exec_run(c71_t *, long);
        if (!rr_c25_exec_run(m, steps)) {
            fprintf(stderr, "[DSP] master stopped at %04X: %s\n", m->cur_pc, m->error);
            faulted = true;
        }
        return;
    }
#endif
    while (steps > 0) {
#ifdef __vita__
        int consumed = native_on ? rr_dsp_native_run(m, steps) : 0;
        if(consumed) {steps-=consumed;native_instructions+=(unsigned)consumed;continue;}
#endif
        if (m->idle) {
            /* If idling and no unmasked pending interrupt, fast-forward timer */
            if (m->intm || !(m->ifr & m->imr)) {
                uint32_t to_underflow = (m->tim == 0 ? m->prd : m->tim);
                if (to_underflow == 0) to_underflow = 1;
                long skip = steps;
                if (m->imr & 8) {
                    if (skip > (long)to_underflow) skip = (long)to_underflow;
                }
                if (skip > 0) {
                    if (m->tim >= skip) {
                        m->tim -= skip;
                    } else {
                        m->tim = m->prd - (skip - m->tim - 1);
                        if (m->imr & 8) m->tint_pend = 1;
                    }
                    steps -= skip;
                    continue;
                }
            }
        }
        steps--;
        if (!c71_step(m)) {
            fprintf(stderr, "[DSP] master stopped at %04X: %s\n", m->cur_pc, m->error);
            faulted = true; return;
        }
    }
}

void rr_dsp_vblank(void) { if (m && running && irq_on) c71_irq(m, 1); }
void rr_dsp_serial(void) { if (m && running && irq_on) c71_irq(m, 0x10 | 0x20); }
bool rr_dsp_render_done(void) { bool r = m && m->pdp_begins != seen_begins; if (m) seen_begins = m->pdp_begins; return r; }
uint16_t rr_dsp_pdp_base(void) { return m ? m->pdp_base : 0; }

bool rr_dsp_slave_active(void) { return slave_on; }
int32_t rr_dsp_pointram_read(uint32_t a)
{
    if (m && a >= C71_PTRAM_S22 && a < C71_PTRAM_S22 + C71_PTRAM_WORDS)
        return (int32_t)(m->ptram[a - C71_PTRAM_S22] << 8) >> 8;
    return -1;
}

void rr_dsp_copy_pointram(uint32_t *dst)
{
    if (m) memcpy(dst, m->ptram, sizeof m->ptram);
    else memset(dst, 0xff, C71_PTRAM_WORDS * sizeof *dst);
}

void rr_dsp_debug(char *buf, int n)
{
    if (!m) { snprintf(buf, n, "dsp: none"); return; }
#ifdef __vita__
    {extern void vita_log(const char *, ...);
     vita_log("[DSP_NATIVE] accounted instructions=%llu\n",(unsigned long long)native_instructions);
     native_instructions=0;}
#endif
    snprintf(buf, n, "dsp: ctrl %02X run %d irq %d slave %d fault %d pc %04X idle %d imr %X ifr %X intm %d pdp %u steps %llu",
             ctrl, running, irq_on, slave_on, faulted, m->pc, m->idle, m->imr, m->ifr, m->intm, m->pdp_begins,
             (unsigned long long)m->steps);
}
