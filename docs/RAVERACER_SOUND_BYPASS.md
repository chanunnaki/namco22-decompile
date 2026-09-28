# Full sound bypass diagnostic

Build `sound-bypass-1` reads `ux0:/data/raverace_sound_disable.enable` at startup. When present it skips sound ROM/MCU initialization, C352 mixer initialization, SDL audio subsystem/device opening, sound-worker creation/dispatch/join, and every sound/mix slice. SDL timer and driving I/O remain active. This removes processing and synchronization rather than merely muting output. Removing the marker and relaunching restores sound without reinstalling.

## Hardware comparison — 2026-09-29

Same executable and 1800-frame scripted inputs on Vita TV 192.168.100.158. Both runs exit cleanly with zero reported traps. All 30 final scene triangle-count/CRC and driving mode/speed checkpoints match. Full CPU/shared/WRAM state and budget do differ when the sound MCU is absent; this diagnostic is not an equivalent replacement for sound emulation. Gameplay validation is limited to the opening driving segment.

Medians across frames 1200–1800 (11 windows):

| Metric | Sound on | Sound fully bypassed |
| --- | ---: | ---: |
| Frame | 33.62 ms | 26.88 ms |
| Renderer job | 21.75 ms | 22.89 ms |
| Parallel device phase | 23.95 ms | 4.30 ms |
| CPU | 5.10 ms | 4.72 ms |
| DSP | 4.52 ms | 4.29 ms |
| Sound CPU | 8.17 ms | 0.00 ms |
| Mixer | 4.90 ms | 0.00 ms |
| Sound worker wake | 7.83 ms | 0.00 ms |
| Sound worker join bucket | 19.34 ms | 0.01 ms |
| Frame preparation, including renderer join and snapshot | 4.61 ms | 17.66 ms |

Frame time falls 20.0%, equivalent to about 29.7 -> 37.2 FPS (25.1% throughput increase). These overlapping stage times must not be added. Removing sound exposes renderer waits: frame preparation rises as the main thread reaches the renderer dependency earlier. Sound is a substantial cost, but its removal does not establish a 60 FPS frame budget.

A leftover capture request caused a single sound-on boot capture at frame 8, outside all measured windows. No measured-window capture occurred. The comparison rejects captures from frame 1140 onward, incomplete runs, probe runs, nonmoving race samples, differing scene/speed checkpoints, or nonzero bypass sound/mix/wake timings.

Evidence: `RAVERACER_SOUND_BYPASS_BENCHMARK.csv`, `raverace/tools/compare_sound_runs.py`; local logs `/tmp/raverace-shader-sound-on.log` and `/tmp/raverace-shader-sound-off.log`. Vita compilation and diff checks pass.

## Installed state

Readback-verified executable SHA256 `63671041c83812b58afcafbc8ff7702fb82cd5cd0fa0d4037800dd020f1ce8c0`. Sound bypass remains enabled for the user's test; scripted controls and GPU probe are disabled. Normal-play startup confirms bypass enabled, no sound worker or sound initialization, and zero sound/mix/wake timing. User FPS/gameplay feedback pending. Restore sound by removing only `raverace_sound_disable.enable` and relaunching through Companion.
