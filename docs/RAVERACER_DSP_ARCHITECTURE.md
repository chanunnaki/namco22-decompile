# Native DSP execution stages

The master DSP now has a native implementation of its object-record assembly
and fixed-point matrix stages. The remaining command parser, boot/upload code,
interrupt handlers and uncommon modes retain the validated instruction runner.

`raverace/tools/gen/dsp_native.py` is a bounded semantic compiler with thirteen
reviewed entry points. It reads the user's game ROM and emits C kernels into the
build directory. Accumulator, product, carry and address-register work is
performed in local C variables, internal RAM is accessed directly, and the
resulting DSP state is committed at the routine boundary. Polygon writes use
the shared banked-memory implementation, preserving high-word latches and
write tracking. This preserves intermediate fixed-point truncation rather than
substituting floating-point transforms. It does not call the opcode executor
for covered instructions.

Covered stages:

- Object header construction and identity-matrix records.
- Translation/orientation inputs and output-list links.
- Six fixed-point rotation-matrix routines.
- Matrix scaling and copy routines.
- CPU completion-flag waits, accounted algebraically while the CPU is paused.

Each entry verifies the active program words and required arithmetic/addressing
mode. A kernel runs only if the current device slice and timer interval can
contain it without crossing an interrupt boundary. Otherwise the instruction
runner executes until a later eligible entry. Repeat state, debug hooks,
unsupported modes and changed uploaded programs also retain instruction
execution. No guest time, device events or frames are omitted.

## Validation

`python3 raverace/tests/test_dsp_native.py` generates the kernels from local
ROMs and checks their complete DSP state and polygon memory against the original
instruction semantics under ASan/UBSan. The current suite passes 1,721 native
kernel cases and 1,919 fallback cases, including bank boundaries, altered
programs, arithmetic modes, interrupt/timer boundaries and repeat state. A
captured Vita race can also be replayed:

```
python3 raverace/tests/test_dsp_native.py /tmp/raverace_dsp_state.bin
```

The capture replay compares all DSP memories, registers, timer/interrupt state,
program contents, bus latches and direct-render output over different execution
budgets, including boundaries inside native routines. All 12 captured replays
match; together they execute 34,452 guest instructions through native stages.
The shared-bus change also passes 4,202 original linked-runner comparisons.

Create `ux0:/data/raverace_dsp_legacy.enable` before launch for the original DSP
runner. Remove it for native stages. The native CPU remains independently
selectable; these switches do not change each other.

For deterministic hardware comparison, use the corrected 1,800-frame driving
script: coin at 180, fresh accelerator presses during selection at 240–599,
then full throttle from 600. The grid appears around frame 480 instead of 1020
with the old continuously-held pedal. Speed becomes nonzero at frame 720.
Measure the moving segment at frames 1200–1800; neutral steering makes this a
repeatable opening driving segment, not a completed lap. The comparator rejects
stationary or non-race checkpoints in that segment.

Use the CPU benchmark marker described
in `RAVERACER_CPU_ARCHITECTURE.md`, then compare the original/native DSP logs:

```
python3 raverace/tools/compare_cpu_runs.py --component dsp LEGACY_LOG NATIVE_LOG
```

For development captures only, `ux0:/data/raverace_dsp_profile.enable` installs
instruction hooks, profiles frames 1020–1080, and writes
`raverace_dsp_profile.csv` and `raverace_dsp_state.bin` in `ux0:/data/` at frame
1080. Profiling disables native kernels and capturing pauses execution; neither
mode should be active for performance measurements or normal play. Captures
contain game data and remain local, outside the source repository.

## Vita TV measurement (2026-09-29)

Same executable, CPU at 444 MHz, native CPU controller enabled in both runs.
Original DSP first, then native DSP, with identical corrected 1,800-frame
inputs. All 30 register/WRAM/polygon/shared-memory CRC and instruction-budget
checkpoints match, as do polygon counts and race mode/speed diagnostics.
Neither run reports a runtime fault.

| Workload | 60-frame windows | Original DSP median | Native DSP median | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Post-boot sequence, frames 180–1800 | 28 | 16.620 ms | 4.835 ms | 70.9% |
| Moving race, frames 1200–1800 | 11 | 19.790 ms | 4.870 ms | 75.4% |

These measure DSP execution, not total frame time or an equivalent FPS gain.
Rendering and sound still take time and overlap other work. The race sample
covers an opening driving segment with neutral steering, not a complete lap.
The earlier grid/countdown-only test showed 47.7% DSP reduction; it is a
different workload and should not be substituted for the driving measurement.

Per-window timings, speed and matching state signatures are in
`docs/RAVERACER_DSP_BENCHMARK.csv`. Local logs:
`/tmp/raverace-dsp-moving-baseline.log` and
`/tmp/raverace-dsp-moving-native.log`.

Installed executable SHA-256:
`94cd6a86109ed15d973b0c83cda8dc1113cb3d92eb10b72341f321d381baf0f8`.
Build identifier: `native-dsp-kernels-2`. Benchmark, legacy and profile markers
are removed after measurement; normal play uses both native CPU and DSP stages.
