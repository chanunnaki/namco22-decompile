/* C74 BIOS timer waits: skip only complete, side-effect-free iterations. */
#ifndef RR_SND_WAIT_H
#define RR_SND_WAIT_H
#include <string.h>
#include "m377_sem.h"
#include "snd_board.h"

static inline bool rr_snd_skip_wait(m37710_t *c, uint64_t until)
{
    /* C0DD / C0FF: LDA $81; BEQ -4, in 8-bit accumulator mode.
     * The host cannot change internal sound RAM during this slice. */
    if (c->pg || c->dpr || ((c->ps >> 4) & 3) != 2 ||
        (c->pc != 0xc0dd && c->pc != 0xc0ff)) return false;
    static const uint8_t wait_code[4] = {0xa5, 0x81, 0xf0, 0xfc};
    if (memcmp(g_snd_bios + (c->pc - 0xc000), wait_code, 4) ||
        c->read8(c->user, 0x81) != 0) return false;
    uint64_t event = m37710_next_event(c);
    if (until > event) until = event;
    if (until <= c->cycles) return false;
    uint64_t loops = (until - c->cycles) / 9; /* LDA: 4; taken BEQ: 5 */
    if (!loops) return false;
    c->cycles += loops * 9;
    c->fetches += (uint32_t)(loops * 4);
    c->a &= 0xff00;
    c->ps = (uint16_t)((c->ps | M377_Z) & ~M377_N);
    c->use_b = false;
    c->ops = NULL;
    return true;
}
/* At the LDA boundary, two complete 64-cycle executor chunks consume 135
 * cycles: the first ends after LDA at 67, the second after BEQ at 135.
 * Skip whole pairs only, so the next run_chunk has exactly the same phase.
 * Never pass a peripheral event or an unserviced interrupt. */
static inline bool rr_snd_skip_wait_chunks(m37710_t *c, uint64_t end)
{
    if (c->pg || (c->pc != 0xc0dd && c->pc != 0xc0ff) ||
        c->pending || c->stopped || c->unimpl_hit || end <= c->cycles)
        return false;
    uint64_t event = m37710_next_event(c);
    if (end > event) end = event;
    if (end <= c->cycles) return false;
    uint64_t pairs = (end - c->cycles) / 135;
    if (!pairs) return false;
    return rr_snd_skip_wait(c, c->cycles + pairs * 135);
}
#endif
