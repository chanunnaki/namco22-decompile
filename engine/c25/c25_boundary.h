/* Instruction-boundary state shared by single-step and linked execution. */
#ifndef C25_BOUNDARY_H
#define C25_BOUNDARY_H
#include <stdio.h>
#include "c25.h"
static inline bool c25_boundary_push(c71_t *d, uint16_t v)
{
    if (d->sp >= 64) { snprintf(d->error, sizeof d->error, "stack overflow"); return false; }
    d->stack[d->sp++] = v; return true;
}

static inline int c25_step_begin(c71_t *d)
{
    /* interrupts at the instruction boundary, in TMS32025 priority order:
     * INT0 2, INT1 4, INT2 6, TINT 0x18, RINT 0x1A, XINT 0x1C */
    if (!d->intm) {
        static const struct { uint16_t bit, imr, vec; } iv[] = {
            {1, 1, 2}, {2, 2, 4}, {4, 4, 6}, {8, 8, 0x18}, {0x10, 0x10, 0x1A}, {0x20, 0x20, 0x1C} };
        if (d->tint_pend) d->ifr |= 8;
        for (int k = 0; k < 6; k++)
            if ((d->ifr & iv[k].bit) && (d->imr & iv[k].imr)) {
                if (!c25_boundary_push(d, d->pc)) return -1;
                d->pc = iv[k].vec; d->intm = 1; d->ifr &= ~iv[k].bit; d->idle = 0;
                if (iv[k].bit == 8) d->tint_pend = 0;
                break;
            }
    }
    if (d->idle) {                        /* halted: nothing retires, the timer runs */
        d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
        if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
        return 0;
    }
    int pc = d->pc;
    if (c25_hook_pre) c25_hook_pre(d, pc);
    d->cur_pc = pc;
    d->steps++;
    /* TIM ticks once per retired instruction, reloading from PRD at 0 */
    d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
    if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
    return 1;
}

/* Preserve the board runner's existing idle/timer batching exactly. */
/* Keep the slow boundary helper out of thousands of translated labels. */
static __attribute__((noinline)) int c25_run_begin(c71_t *d, long *steps)
{
    while (*steps > 0) {
        if (d->idle) {
            /* If idling and no unmasked pending interrupt, fast-forward timer */
            if (d->intm || !(d->ifr & d->imr)) {
                uint32_t to_underflow = (d->tim == 0 ? d->prd : d->tim);
                if (to_underflow == 0) to_underflow = 1;
                long skip = (*steps);
                if (d->imr & 8) {
                    if (skip > (long)to_underflow) skip = (long)to_underflow;
                }
                if (skip > 0) {
                    if (d->tim >= skip) {
                        d->tim -= skip;
                    } else {
                        d->tim = d->prd - (skip - d->tim - 1);
                        if (d->imr & 8) d->tint_pend = 1;
                    }
                    (*steps) -= skip;
                    continue;
                }
            }
        }
        --*steps;
        int state = c25_step_begin(d);
        if (state) return state;
    }
    return 0;
}
#endif
