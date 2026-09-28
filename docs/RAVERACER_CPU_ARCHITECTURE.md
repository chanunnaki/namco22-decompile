# Native CPU frame controller

The Vita build now separates the CPU's frame controller from its translated
instruction implementation. Boot and game-function semantics are retained;
the frame loop is native C in `raverace/src/rr_cpu.c`.

## Execution contract

The original running loop at 0x40EE increments D0, tests the frame gate at
A6-0x77FA, and branches until an interrupt clears it. D0 is later stored at
A6+0x2390 and is observable by the game. The service wait at 0x2B05C compares
the same gate with 1. Both gates reside in CPU WRAM; device workers do not
write that memory. Only the paused CPU's interrupt handlers can change them.

Rather than execute each repetition, the native controller computes the exact
number of iterations until the next existing scheduler boundary. It updates
D0 and flags, accounts for the same instruction budget including overshoot,
and yields to the existing DSP/sound/interrupt scheduler. It then re-reads the
gate and CPU registers. No device slices, game updates or rendered frames are
omitted, and neither the emulated clock nor instructions-per-frame is reduced.

The native main loop dispatches the game frame and housekeeping routines with
the existing guest and shadow stacks. Unusual returns transfer back to the
original continuation switch. Non-WRAM gates retain literal read ordering;
the checker and instruction-trace modes retain the original controller.

## Source and fallback

`raverace/gen/rr_lifted.c` remains unchanged. The build adapter
`raverace/tools/gen/cpu_native.py` attaches the native entry points to a build
copy, with SHA-256 guards on the replaced instruction blocks. If those blocks
change upstream, generation fails until the native contract is revalidated.

Create `ux0:/data/raverace_cpu_legacy.enable` before launching to select the
original controller. Remove it to restore the native controller. The boot log
reports `[CPU] event-driven controller=0` or `1`. `RR_CPU=legacy` also disables
it where environment variables are available.

## Verification and measurement

`python3 raverace/tests/test_cpu_events.py` extracts the actual original lifted
instructions and compares registers, complete WRAM, guest/shadow stack effects,
budgets and every scheduler/call boundary against the native controller. It
covers small budgets, the normal slice budget, initial expired budgets, counter
wraparound, signed gate values, RAM mirrors and interrupt register changes.

For an on-device A/B test, create `ux0:/data/raverace_cpu_bench.enable`. Both
controllers then receive identical inputs: coin at frame 180, gas at 240,
neutral steering, and stop after frame 1200. This mode overrides physical
controls. `[CPU_CHECK]` records register, WRAM, polygon and shared-RAM CRCs plus
the remaining budget every 60 frames. `[TIMING] cpu` measures the CPU portion;
it must not be confused with total frame time. Remove the benchmark marker for
normal play. `[CPU_EVENTS]` counts wait instructions accounted for without
executing their repetitions.

## Vita TV measurement (2026-09-29)

Same executable, 444 MHz CPU, identical 1200-frame scripted input. The legacy
controller ran first, followed by the native controller. All 20 checkpoints
matched CPU register, WRAM, polygon RAM and sound-shared RAM CRCs, instruction
budgets and rendered polygon counts. Neither run reported traps or unmapped
accesses.

| Workload | Samples (60-frame windows) | Legacy median CPU | Native median CPU | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Post-boot sequence (frames 180–1200) | 18 | 13.190 ms | 1.430 ms | 89.2% |
| Race geometry (frames 1020–1200, ~2057 quads) | 4 | 13.920 ms | 3.695 ms | 73.5% |

The race sample covers the initial grid/countdown portion, not a complete lap.
These are CPU-component timings, not an equivalent increase in total FPS;
DSP execution and rendering still consume frame time. The source controller
accounts for about 68,000 idle instructions per race frame without executing
each repetition. Boot still uses the original CPU implementation.

The per-window results and matching signatures are in
`docs/RAVERACER_CPU_BENCHMARK.csv`. Reproduce the comparison with
`python3 raverace/tools/compare_cpu_runs.py LEGACY_LOG NATIVE_LOG`.

Tested executable SHA-256:
`78763020e3276d379395966c114b206c6cc64f50575bac3cd4ff21a350ce2e79`.
