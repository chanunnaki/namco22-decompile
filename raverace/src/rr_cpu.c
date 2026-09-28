/* Event-driven main CPU controller for Rave Racer.
 *
 * Boot and game functions retain their translated/readable implementations.
 * The running CPU has two hardware waits: the main frame gate at 40EE and
 * the service/test gate at 2B05C. Both observe WRAM written only by this CPU's
 * interrupt handlers. Between scheduler boundaries the value cannot change.
 * Account for those intervals algebraically, then run the SAME device slice
 * and interrupt delivery as the instruction runner. No frames or work skipped.
 *
 * D0 counts idle iterations and is game-visible (A6+2390); preserve it, flags,
 * budget overshoot, call stack and every interrupt boundary exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd.h"
#include "rr_cpu.h"
#include "rr_lifted.h"

int rr_cpu_native;
uint64_t rr_cpu_wait_instructions;

void rr_cpu_init(void)
{
    const char *rd = getenv("RR_RD"), *cpu = getenv("RR_CPU");
    rr_cpu_native = (!rd || !strcmp(rd, "1")) && (!cpu || strcmp(cpu, "legacy"));
#ifdef RR_TRACE
    rr_cpu_native = 0;
#endif
#ifdef __vita__
    FILE *f = fopen("ux0:/data/raverace_cpu_legacy.enable", "rb");
    if (f) { fclose(f); rr_cpu_native = 0; }
#endif
}

/* Only ordinary WRAM is eligible: unknown addresses keep instruction-sized
 * execution so an I/O read can never be collapsed. Re-evaluate after each IRQ. */
static int ordinary_wram(uint32_t address)
{
    uint32_t off = address & 0x07ffffffu, bank = address & 0xf8000000u;
    return (bank == 0x10000000u || bank == 0x18000000u) && off <= RR_WRAM_SIZE - 2;
}

static int wait_iterations(uint32_t address, int width, int waiting)
{
    if (!waiting || !ordinary_wram(address) || rr_budget <= 0 || rd_poll_rec)
        return 1;
    return 1 + (rr_budget - 1) / width;
}

void rr_cpu_wait_frame(void)
{
    for (;;) {
        uint32_t address = a_reg(6) - 0x77fau;
        if (!ordinary_wram(address)) {
            /* Literal ordering for an unexpected device-backed gate. The
             * upstream TST performs two reads; either may have side effects. */
            charge(1);
            uint32_t d0 = d_reg(0) + 1;
            set_d(0, d0); RS1(RR_XF, d0 == 0);
            rd_flags_nzvc((int32_t)d0 < 0, d0 == 0, d0 == 0x80000000u, d0 == 0);
            charge(1); RS1(0x46, 0); RS1(0x47, 0);
            RS1(0x44, (int16_t)vrd16(address) < 0);
            RS1(0x45, vrd16(address) == 0);
            charge(1);
            if (RG1(0x45)) return;
            poll();
            continue;
        }
        uint16_t gate = (uint16_t)vrd16(address);
        unsigned n = (unsigned)wait_iterations(address, 3, gate != 0);
        uint32_t d0 = d_reg(0) + n;
        set_d(0, d0);
        /* ADDQ's X survives TST; N/Z/V/C are TST's result. */
        RS1(RR_XF, d0 == 0);
        rd_flags_nzvc((gate & 0x8000) != 0, gate == 0, 0, 0);
        rr_budget = (int32_t)((int64_t)rr_budget - (int64_t)n * 3);
        rr_cpu_wait_instructions += (uint64_t)(n - 1) * 3;
        if (!gate) return;
        poll();
    }
}

void rr_cpu_wait_service(void)
{
    for (;;) {
        uint32_t address = a_reg(6) - 0x77fau;
        /* Charge CMP before a potentially side-effecting read, like the oracle. */
        int literal = !ordinary_wram(address);
        if (literal) charge(1);
        uint16_t gate = (uint16_t)vrd16(address);
        unsigned n = (unsigned)wait_iterations(address, 2, gate != 1);
        rd_flags_cmp16(gate, 1);
        /* Preserve the lifted program's CMP X behavior as well as NZVC. */
        RS1(RR_XF, gate < 1);
        rr_budget = (int32_t)((int64_t)rr_budget - (int64_t)n * 2 + literal);
        rr_cpu_wait_instructions += (uint64_t)(n - 1) * 2;
        if (gate == 1) return;
        poll();
    }
}

/* Return an unusual continuation to the original function's resume switch.
 * RD_UNWIND propagates its existing shadow-stack unwind without another call. */
uint32_t rr_cpu_frame_loop(void)
{
    for (;;) {
        rr_cpu_wait_frame();
        uint32_t d0 = d_reg(0), a6 = a_reg(6);
        vwr32(a6 + 0x2390, d0);
        rd_flags_nzvc((int32_t)d0 < 0, d0 == 0, 0, 0);
        charge(1);
        uint32_t flags = a6 - 0x77f3u;
        uint8_t b = (uint8_t)vrd8(flags);
        RS1(0x45, !(b & 0x80)); vwr8(flags, b | 0x80); charge(1);
        b = (uint8_t)vrd8(flags);
        RS1(0x45, !(b & 0x20)); vwr8(flags, b | 0x20); charge(1);
        charge(1);
        uint32_t r = rd_call(L_4120, 0x410a);
        if (r) return r;
        /* Callbacks/interrupts can change A6: never retain it across a poll. */
        a6 = a_reg(6);
        vwr16(a6 - 0x77fau, 1); rd_flags_nzvc(0, 0, 0, 0); charge(1);
        flags = a6 - 0x77f3u; b = (uint8_t)vrd8(flags);
        RS1(0x45, !(b & 0x80)); vwr8(flags, b & ~0x80u); charge(1);
        charge(1);
        r = rd_call(L_4D52, 0x411c);
        if (r) return r;
        set_d(0, 0); rd_flags_nzvc(0, 1, 0, 0); charge(2);
        poll();
    }
}
