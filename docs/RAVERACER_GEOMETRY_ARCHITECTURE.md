# Indexed geometry and worker-built triangles

The Vita renderer now compiles immutable point-ROM objects into indexed meshes and builds GPU triangles inside the geometry workers. Shared positions are transformed and projected once per object invocation. The renderer merges completed packets in original emission order, sorts lightweight references, and uploads the finished triangle stream.

## Ownership and compatibility

Each geometry lane owns its mesh cache (8 MiB maximum), transform generations and object output arenas. Mesh commands retain per-face UVs, flags and lighting operations. Per-face culling, lighting cursor advancement, fixed-point projection, clipping, fog and painter ordering preserve the reference implementation. Position sharing does not merge texture or brightness attributes.

Point-RAM object 5 remains dynamic and uses the original walker. Unsupported packets, non-ROM references, malformed bounds or cache/allocation limits fall back to the original object path. Changing the ROM pointer or size clears the cache; callers that mutate ROM contents in host tests must explicitly clear both lane caches.

Workers compile triangles using immutable frame palette, fog and scene extents. They never call GXM. Each job owns its vertex arena; references are assembled after jobs join and reallocations finish. The renderer copies sorted triangles into its CPU batch and then the fenced GPU slot, so the GPU never reads worker arenas. The full polygon intermediate and serial triangle construction remain available for comparison.

Create `ux0:/data/raverace_geometry_legacy.enable` before launch to select the original mesh/triangle path. Native CPU, DSP, GPU HUD and triple resource slots remain independent settings. Native packet construction requires the batch renderer and GPU HUD; otherwise the compatible serial path is used.

## Validation

- 200 synthetic scenes compare every polygon byte and geometry statistic against the original walker, then compare parallel triangle bytes and painter order.
- 30,000 polygons compare the extracted pure compiler with a frozen copy of the original serial builder, including clipping, fog, palette modes and capacity limits.
- Existing near-clip, division/projection and sorting checks pass. Host geometry/packet tests use ASan/UBSan; the mesh test exercises the Vita integer-math fallback.
- Hardware: two 1800-frame scripted runs of `indexed-geometry-packets-1`, switching only the geometry marker. All 30 state, budget, polygon-count, driving-speed and final GPU triangle-count/CRC checkpoints match. No capture requests during timing.

## Measured result

The moving-race comparison uses frames 1200–1800, eleven 60-frame windows, on Vita TV 192.168.100.158. This is the scripted opening straight, not a full lap. Medians:

| Measurement | Reference | Indexed packets |
| --- | ---: | ---: |
| Total measured frame | 40.420 ms | 40.400 ms |
| Render job | 36.510 ms | 35.540 ms |
| Geometry stage | 12.720 ms | 8.660 ms |
| Sort | 1.130 ms | 0.860 ms |

Geometry drops 31.9% even though its new stage also includes worker triangle construction. **There is no material overall FPS gain in this A/B run** (about 24.7 FPS from reciprocal median frame time). Do not describe this as a 32% frame-rate improvement. Component work overlaps, and its medians cannot be added to reconstruct total time. Total measured frame time is CPU + parallel devices + main prepare/join + main draw.

`BATCH_TIME` counts nonempty batches rather than global frames. Its windows are not aligned with the state checkpoints, so the comparison deliberately excludes those fields instead of pairing unrelated windows.

Evidence: `RAVERACER_GEOMETRY_BENCHMARK.csv`. Reproduce with:

```sh
python3 raverace/tests/test_gxm_packets.py
python3 raverace/tests/test_geometry_packets.py
python3 raverace/tools/compare_render_runs.py --geometry reference.log native.log --csv result.csv
```

Local A/B logs: `/tmp/raverace-geometry-baseline.log` and `/tmp/raverace-geometry-native-result.log`. Build 2 adds scene-begin, triangle-submission and presentation stage timing to locate the remaining limit without changing geometry behavior.

## Remaining limit and deployed build

`indexed-geometry-packets-2` adds timing only. Its complete 1800-frame run again matches all 30 reference state and final triangle checkpoints. Moving-window median total frame time is 40.26 ms. Render stages: preparation 13.43 ms, drawing 6.28 ms, presentation 15.72 ms. Presentation measures `swap_snapshot`, including `vita2d_swap_buffers`; no capture marker was present. Scene begin is approximately 0.05 ms in the sampled race windows. Explicit vita2d vblank waiting is already disabled.

This localizes a substantial remaining delay to presentation/GPU backpressure. It does not yet distinguish GPU completion from display queue/callback scheduling, so disabling more synchronization without tracing that dependency is not justified. A further presentation/GPU architecture change is needed to convert the reduced CPU geometry cost into throughput; 60 FPS is not established by these results.

Installed and FTP readback-verified SHA256: `4770e9a0edf07a196566bb7cde6f3ea1ce21de6bb099d95f390a815cb3e5f15d`. Direct Companion update, normal controls restored; benchmark and geometry fallback markers removed. Rollback executable: `/tmp/raverace-before-indexed-geometry.eboot.bin`. Diagnostic log: `/tmp/raverace-geometry-diagnostic.log`.
