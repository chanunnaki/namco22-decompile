/*
 * c25_core.c -- the master DSP's STEP (engine/c25/c25.h): interrupts at the
 * instruction boundary in TMS32025 priority order, IDLE, the timer, and then
 * the instruction at PC through the TRANSLATED program (d->xlat). Moved from
 * the validated interpreter; the fetch and decode it did are gone -- the
 * translation carries every opcode as a constant.
 */
#include <stdio.h>
#include <string.h>
#include "c25.h"

#include "c25_boundary.h"

bool c71_step(c71_t *d)
{
    int state = c25_step_begin(d);
    if (state <= 0) return state == 0;
    if (!d->xlat) { snprintf(d->error, sizeof d->error, "no program translation"); return false; }
    if (!d->xlat(d, d->cur_pc)) return false;
    if (c25_hook_post) c25_hook_post(d);
    return true;
}

void c71_reset(c71_t *d)
{
    d->bank = 0; d->latch = 0; d->pt_addr = 0; d->pt_data = 0; d->bioz = 1;
    d->pc = 0x4000; d->pfc = 0; d->t = 0; d->acc = 0; d->p = 0;
    memset(d->ar, 0, sizeof d->ar);
    d->arp = d->arb = d->dp = d->pm = 0;
    d->sxm = 1; d->ovm = 0; d->intm = 0; d->c = 0; d->tc = 0; d->cnf = 0;
    d->imr = 9; d->prd = 0xFFFF; d->tim = 0xFFFF; d->tint_pend = 0;
    d->idle = 0; d->ifr = 0;
    d->sp = 0; d->rpt = 0; d->steps = 0; d->error[0] = 0;
    memset(d->written, 0, sizeof d->written); d->n_written = 0;
}

static bool load_words(uint16_t *dst, size_t max_words, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[2]; size_t i = 0;
    while (i < max_words && fread(b, 1, 2, f) == 2) dst[i++] = (uint16_t)(b[0] << 8 | b[1]);
    fclose(f);
    return true;
}

bool c71_load(c71_t *d, const char *bios_path, const char *prog_path)
{
    bool ok = true;
    if (prog_path) ok = ok && load_words(d->prog + 0x4000, 0xC000, prog_path);
    if (bios_path) ok = ok && load_words(d->prog, 0x4000, bios_path);
    return ok;
}
