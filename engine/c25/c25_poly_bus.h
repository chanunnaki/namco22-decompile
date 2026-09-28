/* Shared banked polygon-RAM semantics, also used by native DSP stages. */
#ifndef C25_POLY_BUS_H
#define C25_POLY_BUS_H
#include "c25.h"
/* the 16-bit banked window onto polygon RAM (MAME namcos22_dspram16_r/w) */
static inline uint16_t c25_poly_read16(c71_t *d, uint32_t off)
{
    uint32_t v = d->poly[off & 0x7FFF];
    switch (d->bank & 3) {
    case 0: return v & 0xFFFF;
    case 1: return (v >> 16) & 0xFFFF;
    case 2: d->latch = (v >> 16) & 0xFFFF; return v & 0xFFFF;
    default: return 0;
    }
}

static inline void c25_poly_write16(c71_t *d, uint32_t off, uint16_t data)
{
    uint32_t v = d->poly[off & 0x7FFF];
    uint16_t lo = v & 0xFFFF, hi = (v >> 16) & 0xFFFF;
    switch (d->bank & 3) {
    case 0: lo = data; break;
    case 1: hi = data; break;
    case 2: lo = data; hi = d->latch; break;
    default: break;
    }
    d->poly[off & 0x7FFF] = ((uint32_t)hi << 16) | lo;
    if (!d->written[off & 0x7FFF]) { d->written[off & 0x7FFF] = 1; d->n_written++; }
}

#endif
