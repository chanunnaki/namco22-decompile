# GPU tilemap HUD and fenced render resources

The renderer now sends arcade character RAM, tile RAM and a small palette to
the GPU. The HUD fragment shader performs tile selection, flips, scrolling,
nibble decoding and palette lookup. Pen 15 remains transparent; destination
alpha still controls polygon-over-HUD priority. It uses the same arcade screen
coordinates as the previous HUD, retaining the accepted speedometer alignment.
Character codes 960–1023 alias tile RAM exactly as in the original decoder.

The normal path no longer expands a 640x480 RGBA HUD on a CPU worker, waits for
that worker, or uploads that full image. Dirty 128-byte input blocks are copied
to GPU memory. The palette/HUD worker and old decoded texture remain as an
explicit fallback. Palette conversion for the polygon renderer now runs on the
render thread before geometry, so consumers see a completed palette without a
worker dependency.

Three resource slots own separate batch vertices, polygon palettes, HUD
character/tile/palette textures and HUD vertices. ROM textures and indices are
immutable and shared. Each submitted scene records a fragment-completion
notification. A slot can be reused only after its own notification completes;
the renderer no longer calls a global GPU finish before every normal frame.
GXM still manages the framebuffer/display queue. A linker wrapper supplies
notifications to vita2d's EndScene call while retaining vita2d's bookkeeping.
Serial and baked-renderer fallback paths retain the global completion wait.

The existing immutable board snapshot remains owned by the render CPU job until
submission finishes. GPU reads are from the slot copies, so the CPU can then
capture another emulated frame safely. Shutdown drains outstanding GPU work.
This adds about 9.5 MB of batch storage plus 0.5 MB of extra HUD GPU storage compared
with a single slot. A linker script reserves a full page for Vita ELF module
metadata, avoiding code/data segment overlap as the executable grows.

## Validation

- `python3 raverace/tests/test_gxm_tile_hud.py`: 640,000 samples execute the
  actual shader formula against the original arcade decoder under ASan/UBSan.
- `python3 raverace/tests/test_gxm_frame_slots.py`: 3,000 actual acquisition and
  notification handoffs with delayed GPU completion; 600 dirty HUD updates
  verify independent slot contents and RAM aliases.
- `python3 raverace/tests/test_gxm_batch.py`: 200,000 existing scene texture
  samples still match the CPU oracle.
- Hardware capture `/tmp/raverace-gpu-hud.png` shows road, buildings, mirror,
  minimap, timer and speedometer. Its overlay reads 25 FPS at that instant; this
  single capture is not an average performance measurement.

The driving benchmark uses the corrected selection inputs described in
`RAVERACER_CPU_ARCHITECTURE.md`. Both modes run with native CPU and DSP enabled.
`raverace/tools/compare_render_runs.py` rejects mismatched build/input sequences,
state checks, budgets, polygon counts or driving speed, and rejects logs that
contain screenshot capture stalls. It reports frames 1200–1800 only.

`frame` in the report sums main-thread CPU, parallel device wall time,
prepare/wait/snapshot and host time. It does not add overlapping render or DSP
work again. `render` measures the complete render CPU job including GPU waits
and display submission, not pure GPU execution. Reciprocal frame time is a
throughput estimate, not an independently sampled FPS counter.

## Diagnostic switches

Create `ux0:/data/raverace_hud_legacy.enable` before launch for the original HUD
and serial GPU-resource reuse. Create `ux0:/data/raverace_gpu_serial.enable` to
keep the native GPU HUD but force serial resource reuse. Shader/allocation
failure falls back to the existing HUD. Native resource buffering requires both
the batch renderer and GPU HUD to be active.

For normal play remove those markers and `raverace_cpu_bench.enable`.
`[HUD_GPU] tilemap renderer enabled` and
`[GPU_FENCE] buffered=1 notifications=1` confirm the new path. `[SYNC]` separates
resource acquisition, layer uploads, HUD draw submission and scene-end calls.

## Vita TV result (2026-09-29)

Same executable, original HUD/serial reuse versus GPU HUD/fenced slots, both
with native CPU and DSP. Identical 1,800-frame corrected driving inputs; all
30 state/budget/geometry checkpoints and mode/speed values match. No runtime
faults or screenshots during either measured run. Medians below use eleven
60-frame windows ending at frames 1200–1800.

| Measurement | Original | New | Reduction |
| --- | ---: | ---: | ---: |
| Total measured main-loop frame time | 52.140 ms | 40.280 ms | 22.7% |
| Complete render job | 48.300 ms | 36.320 ms | 24.8% |
| GPU resource-reuse wait | 10.390 ms | 0.010 ms | 99.9% |
| HUD upload/draw submission | 10.790 ms | 0.020 ms | 99.8% |
| Layer/texture upload stage | 1.500 ms | 2.310 ms | increased |

The HUD upload now occurs in the layer-upload stage, so its old bucket is not
a measure of all new HUD costs. GPU tile decoding also takes GPU time. The
complete render and total-frame measurements include these consequences.
Reciprocal median frame times imply approximately 19.2 -> 24.8 FPS; user play
can vary by scene. Rendering remains above the 16.7 ms budget for 60 FPS.
CPU geometry, sorting and triangle construction remain significant, with sound
and geometry sharing a worker core. Removing waits does not eliminate that work.

Per-window evidence: `docs/RAVERACER_RENDER_BENCHMARK.csv`.

```
python3 raverace/tools/compare_render_runs.py \
  /tmp/raverace-render-baseline-v2.log /tmp/raverace-render-native-v2.log
```

Build: `gpu-buffered-hud-2`. Installed/readback-verified SHA-256:
`b48e7404c8bce55e3d18704d09fba1865ae5998951226d0777bc141ff1b045df`.
Normal-play markers are removed after testing. Prior native CPU/DSP rollback:
`/tmp/raverace-before-gpu-hud.eboot.bin`.

User hardware confirmation: race FPS is around 25 and everything looks normal.
