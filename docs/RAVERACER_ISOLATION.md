# Simulation and renderer isolation

Build `isolation-1` provides opt-in experiments rather than a new gameplay architecture. Target is approximately 59.9 simulation ticks and rendered frames per second, or 16.7 ms per tick/frame.

## Method

- `ux0:/data/raverace_sim_only.enable`: retain main CPU, DSP, sound CPU, C352 mixing, SDL audio output, input updates and display-list bookkeeping. Skip frame-memory copies, render/geometry workers, GPU scene submission and swaps. Main thread is pinned to USER_0, as in normal play; sound worker remains USER_2. The screen is blank. GXM/assets still initialize before timing; this is execution isolation, not a memory-footprint comparison.
- `ux0:/data/raverace_render_replay.enable`: normal scripted race to frames 600, 1200 and 1740. At each point capture the same immutable snapshot as normal, dispatch a replay job to the existing USER_1 renderer, and immediately join it on main. Main simulation, DSP and sound execution remain paused. The SDL output callback remains alive and may underrun while no samples are produced.
- Each snapshot runs two experiments: rebuild geometry and submit/present; then reuse the already-built geometry while still performing uploads, HUD handling, drawing and presentation. Each has 60 warm-up frames followed by 180 timed frames. GPU work and display callbacks drain at the timing boundaries; final drain is included in the measured average. Existing triple GPU resource slots and asynchronous presentation are retained.
- This is an isolated renderer harness inside the existing executable, not a separate minimal renderer application. Repeated scenes have warmer caches than a changing race; no claim of full-race throughput follows from these numbers.
- Snapshot CRC is checked before/after, all emitted frozen triangle streams are checked against normal source frames, and engine frame numbering is restored before gameplay continues. No game ticks are skipped.
- `[ISOLATION_TICKS]` reports actual wall time between 60-tick checkpoints, including intervening diagnostics. Normal/simulation summaries use 11 windows ending at frames 1200–1800. Replay-run game tick timings are intentionally invalid for throughput because replay pauses are included.

## Results

Simulation with sound, rendering entirely absent: median **20.212 ms/tick, 49.5 ticks/s** (window range 17.220–20.695 ms). Normal combined execution is **33.978 ms/tick, 29.4 ticks/s** (30.445–35.544 ms). This is below the full-speed target. Sound worker wake overhead falls to about 0.34 ms/tick without competing geometry work, but sound execution (~7.34 ms), mixing (~4.83 ms), main CPU (~3.81 ms), DSP (~3.89 ms) and synchronization still exceed the budget collectively. These overlapping component medians must not be added as independent savings.

| Frozen source frame | Quads | Rebuild + draw/present | Cached geometry + draw/present |
| --- | ---: | ---: | ---: |
| 600, grid/start | 2057 | 29.367 ms | 28.749 ms |
| 1200, moving race | 1349 | 24.345 ms | 23.461 ms |
| 1740, moving race | 1119 | 27.740 ms | 27.343 ms |

Removing geometry preparation barely changes sustained throughput. For example, frame 600 preparation falls from 18.550 ms to effectively zero, but the presentation bucket rises from 2.417 to 21.372 ms. Frame 1740 cached draw submission is 4.215 ms while presentation takes 22.828 ms. CPU geometry overlaps the GPU; removing it lets submission reach queue backpressure sooner. These are end-to-end presentation measurements, not isolated GPU shader timestamps.

There are two independently demonstrated constraints: simulation with sound cannot yet sustain 60 ticks, and current scene drawing/presentation cannot sustain 60 frames even without simulation or geometry rebuilding. Extra snapshot buffering alone cannot remove either sustained cost. This does not establish a Vita hardware limit or prove that a clean rewrite would succeed. The next rendering experiment should distinguish GPU workload from presentation limits; the simulation side needs sound/device scheduling and execution changes.

## Reproduction and validation

Use identical scripted inputs (`raverace_cpu_bench.enable`) for normal, simulation and replay modes. Sound bypass and GPU completion probe must be absent. Remove capture requests before timed runs. All three runs should complete 1800 frames with clean shutdown and identical guest registers/WRAM/polygon/shared RAM/budget and race mode/speed checkpoints.

Run `python3 raverace/tools/compare_isolation_runs.py NORMAL.log SIM.log REPLAY.log --csv docs/RAVERACER_ISOLATION_BENCHMARK.csv`. Local logs: `/tmp/rr-isolation-baseline.log`, `/tmp/rr-isolation-sim.log`, `/tmp/rr-isolation-replay.log`.

Executable SHA256: `0f964b09514937200b9190410a0a6de719e6a4c4e3b243091241637a7d3a2779`. Prior silent build backup: `/tmp/raverace-before-isolation.eboot.bin`. Remove both isolation markers and the scripted benchmark marker, then relaunch to restore normal play. Remove sound-bypass marker to restore audio.


All three 1800-tick hardware runs passed: 30 complete guest-state/budget and driving checkpoints match, frozen triangle streams match the normal source frames, snapshots remain unchanged, and shutdown is clean. Vita build and diff checks pass. Final installation has both isolation markers, scripted benchmark, GPU probe and sound bypass removed; normal rendering, controls and audio restored.
