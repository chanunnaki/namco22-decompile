## Presentation trace and shutdown crash fix — 2026-09-29

- Traced actual linked vita2d queue function and callback. Same-binary normal/probe runs match all30 state/budget/geometry/speed/final triangle CRC checkpoints. Moving-window medians (frames1200–1740): normal frame40.565ms, queue14.330ms, callback0.520ms; explicit post-enqueue GPU wait40.850ms, queue0.040ms, callback0.440ms, total66.200ms. Probe intentionally serializes GPU completion, giving~15FPS and low CPU usage. **No FPS improvement claimed.** Normal presentation remains~25FPS.
- Queue backpressure follows unfinished GPU work; the callback itself is inexpensive. Next target: GPU shading/texture work, separating scene texture decoding, HUD, overdraw and memory costs. Completion wait is not an isolated shader timestamp. The linked SDK does not call PadHeartbeat during swapping; waiting before queue submission stalled, so the diagnostic waits only after enqueueing. Never leave `raverace_gpu_probe.enable` active for normal play.
- User correctly reported a native crash with core dump at benchmark exit. Dump `psp2core-1790630766-0x0006013abf-eboot.bin.psp2dmp` shows render thread in `_free_r` after main's frame1800 stop/exit. Bounded-run path now calls host shutdown to join renderer/geometry workers; GPU shutdown drains the display queue before libc exit. Fixed build repeats all1800frames/30checks, logs final callback and `[RUN_END] ... host_closed=1`, and creates no new dump. Host callback480cases and worker10000handoff tests pass; Vita build/diffchecks pass.
- Installed `presentation-trace-4` on Vita TV .158, readback SHA256 `5cb79707e9bb05df11abaffa34eecabb352327b5c0f1b1bbc967f5d6420ddc1b`; normal controls and asynchronous presentation restored. Details/CSV: `docs/RAVERACER_PRESENTATION_TRACE.md`, `docs/RAVERACER_PRESENTATION_BENCHMARK.csv`. Rollback `/tmp/raverace-before-presentation-trace.eboot.bin`.

## Indexed geometry and parallel triangle packets — 2026-09-29

- ROM objects now compile to per-lane indexed meshes; shared positions transform/project once per object invocation. Workers construct finished triangle packets and the renderer sorts lightweight references before uploading. Dynamic point RAM and unsupported objects retain the original walker; original geometry path selectable with `raverace_geometry_legacy.enable`.
- Same-binary 1800-frame hardware A/B: all 30 state/budget/geometry/speed checkpoints and final triangle counts/CRC match. Geometry stage 12.720 -> 8.660 ms (31.9% reduction), now including worker triangle construction; sort 1.130 -> 0.860 ms. **Overall frame time is effectively unchanged: 40.420 -> 40.400 ms**, roughly 25 FPS. Do not claim geometry savings as an FPS improvement.
- Final timing build also passes all 30 checkpoints. Preparation 13.43 ms, drawing 6.28 ms, presentation 15.72 ms medians; total frame40.26 ms. Scene begin ~0.05 ms. Remaining delay is localized to presentation/GPU backpressure, not yet distinguished between GPU completion and display queue scheduling. Explicit vita2d vblank wait already disabled. This is the next architectural investigation.
- Host validation: 200 full scenes match polygon bytes/statistics and parallel triangle bytes/order; 30000 triangles/polygons compiler cases match frozen original; existing clip/math checks pass under sanitizers. Vita build and diff checks pass.
- Installed `indexed-geometry-packets-2` on Vita TV .158, SHA256 `4770e9a0edf07a196566bb7cde6f3ea1ce21de6bb099d95f390a815cb3e5f15d`; normal controls restored. User visual/FPS feedback pending. Rollback `/tmp/raverace-before-indexed-geometry.eboot.bin`. Evidence in `docs/RAVERACER_GEOMETRY_ARCHITECTURE.md` and `docs/RAVERACER_GEOMETRY_BENCHMARK.csv`.

## GPU HUD and buffered rendering — 2026-09-29

- Replaced CPU RGBA HUD expansion/upload and its worker dependency with a GPU tilemap decoder. Three independently owned sets of batch vertices, palettes and HUD RAM are protected by fragment-completion notifications; ordinary rendering waits only when reusing its own slot instead of draining the GPU each frame. Existing decoded HUD/serial path remains selectable. No screen-resolution reduction.
- Same-binary corrected driving comparison (1800 frames, native CPU/DSP in both modes): all30 state/budget/polygon-count checkpoints and mode/speed values match. Moving-window median total measured frame time **52.140 -> 40.280 ms, 22.7% reduction**; render job48.300 ->36.320ms (24.8%). Reciprocal frame-time estimate19.2 ->24.8FPS; user FPS may vary. Resource wait10.390 ->0.010ms; HUD upload/draw bucket10.790 ->0.020ms, while layer upload increases1.500 ->2.310ms. These are overlapping/substage timings, not additive independent savings.
- Captured race `/tmp/raverace-gpu-hud.png` shows road/buildings/mirror/HUD/speedometer intact, overlay25FPS. Capture was excluded from both final timing runs. Native shader640000 samples, existing scene shader200000 samples,3000 GPU ownership handoffs and600 dirty per-slot HUD updates pass host ASan/UBSan. Vita build and diff checks pass. Linker fragment reserves space for SCE ELF metadata to prevent text/data overlap.
- Installed/readback-verified on Vita TV192.168.100.158: `gpu-buffered-hud-2`, SHA256 `b48e7404c8bce55e3d18704d09fba1865ae5998951226d0777bc141ff1b045df`. Benchmark/HUD legacy/GPU serial markers removed, native CPU/DSP retained, normal controls restored. User confirms race FPS around25 and everything looks normal.
- Main remaining costs: CPU geometry, sorting and triangle construction; sound/geometry worker contention; snapshot copies. Total40ms is still well above16.7ms required for60FPS. Details and evidence: `docs/RAVERACER_RENDER_PIPELINE.md`, `docs/RAVERACER_RENDER_BENCHMARK.csv`. Logs `/tmp/raverace-render-baseline-v2.log`, `/tmp/raverace-render-native-v2.log`. Prior rollback `/tmp/raverace-before-gpu-hud.eboot.bin`.

## Native DSP stages — 2026-09-29

- Native fixed-point matrix and object-record stages replace instruction dispatch at thirteen reviewed entry points; CPU doorbell waits are accounted algebraically. Guarded fallback preserves unsupported program/mode/timer/IRQ behavior. Shared inline banked polygon bus retains original latches and write tracking. Original instruction runner remains available.
- User identified wasted track-selection time in the test. Corrected script releases and presses the accelerator through selection, reaches grid around frame480 rather than1020, holds throttle from600, and stops at1800. Speed is nonzero from720. Final comparison uses moving frames1200–1800; do not compare these inputs directly with the old CPU/grid benchmark.
- Same-binary original/native DSP A/B: **19.790 -> 4.870 ms/frame, 75.4% reduction** across11 moving-race windows. All30 state CRC/budget/geometry checkpoints and mode/speed diagnostics match; no runtime fault. Post-boot sequence16.620 ->4.835ms (70.9%). This is DSP component time, not total FPS; rendering/sound remain. Opening driving segment, not a complete lap.
- Host ASan/UBSan:1721 native cases +1919 guard fallbacks match complete DSP state/polygon RAM;12 captured-grid replays match full DSP memories/registers/direct-render output,34452 guest instructions native. Shared bus also passes4202 original linked-runner comparisons. Vita build and diff check pass.
- Installed and readback-verified on Vita TV192.168.100.158 via Companion: `native-dsp-kernels-2`, SHA256 `94cd6a86109ed15d973b0c83cda8dc1113cb3d92eb10b72341f321d381baf0f8`. Native CPU remains enabled. Benchmark/legacy/profile markers removed; normal play restored. No manual install needed.
- Details: `docs/RAVERACER_DSP_ARCHITECTURE.md`, `docs/RAVERACER_DSP_BENCHMARK.csv`. Logs `/tmp/raverace-dsp-moving-baseline.log`, `/tmp/raverace-dsp-moving-native.log`. Native-CPU-only rollback `/tmp/raverace-native-cpu-good.eboot.bin`. DSP fallback marker `ux0:/data/raverace_dsp_legacy.enable`; benchmark/profile markers must stay absent for normal play.

## Native CPU controller — 2026-09-29

- Fork: `chanunnaki/namco22-decompile`, branch `codex/vita-native`; origin is the fork, upstream push disabled. Fork baseline commit `d12fe1d`.
- User requests architectural CPU replacement targeting at least 50% CPU-time reduction. Implemented native event-driven frame controller (`rr_cpu.c`), replacing the running frame loop and service wait. Original CPU translation remains unchanged, attached by a guarded build adapter. Guest instruction budgets, idle counter, flags, scheduler events and game calls are preserved.
- `native-cpu-events-1` installed and verified on Vita TV .158 via Companion. SHA256 `78763020e3276d379395966c114b206c6cc64f50575bac3cd4ff21a350ce2e79`. Original-controller and native-controller A/B runs: 1200 identical scripted frames each, all 20 state CRC/budget/geometry checkpoints match. No runtime faults. Race grid/countdown CPU median 13.920 -> 3.695 ms (73.5% reduction); post-boot sequence median 13.190 -> 1.430 ms (89.2%). Full-lap timing not yet measured. Total FPS is still constrained by DSP/rendering.
- 360 host comparisons against extracted original instructions pass ASan/UBSan, checking all registers, WRAM, stack effects, budgets and every scheduler/call boundary. Details and per-window evidence: `docs/RAVERACER_CPU_ARCHITECTURE.md`, `docs/RAVERACER_CPU_BENCHMARK.csv`.
- Benchmark and legacy markers removed for normal play; native enabled by default. Fallback marker `ux0:/data/raverace_cpu_legacy.enable`. Opt-in scripted benchmark marker `ux0:/data/raverace_cpu_bench.enable` overrides controls and stops at frame1200; do not leave it enabled for the user.
- Local rollback `/tmp/raverace-before-native-cpu.eboot.bin` (atlas-3). Logs `/tmp/raverace-cpu-baseline.log`, `/tmp/raverace-cpu-native-result.log`. No need for manual VPK installation.

## Atlas build and current results

- User confirms pipeline2 peaks23FPS (pipeline1 mostly18–20/peak22). Screenshot `/tmp/raverace-pipeline-2.png` inspected: road/buildings/mirror/HUD intact, overlay19FPS. User confirms countdown road jerk is also present on old Blue Vita2000 build, so it predates this work.
- `frame-pipeline-atlas-3` installed and launched directly via Companion; uploaded executable SHA256 verified `bfea8ad927e87f94a4bbe38d14e0896337caff6d585c73cbc640fd57f5c6011e`. Startup confirms atlas packing, batch fragment shader1568bytes (previous1648), CDRAM, pipeline and background HUD worker. Clean log `/tmp/raverace-vitatv-frame-pipeline-atlas-3.log`, no startupfaults. Race FPS/visual validation pending. All builds/uploads completed. Changes: immutable pen ROM is repacked losslessly as256x256 spatial16x16tiles in4096x4096U8 atlas. Previously each tile occupied a256x1 strip, causing poor spatial texture-cache use. Shader now uses low/high tile-ID bytes directly as atlas X/Y: fewer address calculations, same pixels/nearest sampling and full resolution. Reuse64KB of tilemap staging during initialization, then fill full tilemap normally; no extra persistent memory. All16MB bytes checked,200000 actual shader samples matchCPUoracle underASan/UBSan. Build/diffcheck pass.
- `vita_log` shared FILE had visibly interleaved/duplicated messages with concurrent render/main writes. Added sleeping kernel mutex around open/vfprintf/fflush; initialized by first main-thread boot log before workers exist. No spinlock/priority inversion busy wait.
- Pipeline2 remains prior rollback `/tmp/raverace-before-atlas-3.eboot.bin`. Initial pipeline rollback `/tmp/raverace-before-pipeline-2.eboot.bin`; pre-pipeline rollback `/tmp/raverace-before-frame-pipeline.eboot.bin`.

## Latest hardware result and scheduler refinement

User confirmed frame-pipeline-1: **mostly18–20FPS, peaks22**. Old linked build~15, earlier peak16. Pipeline logs confirm core1 render overlapping main emulation, no faults. Stationary2057quad scene: render~61ms, main CPU13.5+devices33.7+join/snapshot18.1~65ms (~15FPS), versus earlier~85–92ms. Moving race user18–22FPS. Core2 geometry delays sound wake (~12ms/frame), while main core has spare wait intervals.

`frame-pipeline-2` built: existing HUD/palette worker runs on USER_0 at lower priority0x10000110, using immutable snapshot data during main thread waits. Renderer USER_1 prepares geometry concurrently and joins HUD before drawing; former~8ms synchronous HUD work removed from renderer's serial path. Fixed affinity initialization so render thread is not accidentally moved to USER_0, and do not delete successful worker semaphores in pipeline mode. Direct deployment underway. No new shader/geometry semantics changes. Need verify worker starts and race timing/user FPS.

## Frame pipeline — latest candidate

- Linked-DSP candidate gave only a small improvement (user15FPS). User reported the road/cars jumping upward ~one wheel height after countdown. Rolled back to idle-pairs-vfp-1 via Companion; user confirmed the same whole-road jerk persists. Thus not introduced by linked DSP; unresolved visual/camera issue, do not claim fixed. Linked runner now opt-in using `ux0:/data/raverace_linked_dsp.enable`; ordinary path is previous single-step DSP.
- New `frame-pipeline-1`: frame preparation captures immutable renderer input (polygon RAM, point RAM, palette, mixer, fog, HUD RAM/attrs and direct commands), then dispatches complete prepare/draw/swap to USER_1. Main USER_0 emulates next frame concurrently; its next capture joins prior render before overwriting snapshot. Sound/mix and geometry share USER_2. Palette/HUD work synchronous inside renderer in this mode. GXM accesses remain serialized on renderer thread after initialization. Adds at most one frame in flight.
- Serial fallback if worker creation fails or marker `ux0:/data/raverace_pipeline.disable` exists. All scene state/queue consumption stays main thread; rendering never reads live g_rr or DSP point RAM. Completed quad count atomic. Shutdown joins renderer before stopping geometry/HUD workers.
- Tests passed:40 full-frame snapshot/mutation cases,10000 worker handoffs,448 HUD frames,200 parallel geometry scenes; Vita build and diff check passed. Deploying directly via Companion. New [PIPELINE] reports render work/core and main wait/snapshot time; main [TIMING] prepare now means join+snapshot, draw mainly input handling. Don't add renderer time to main time because they overlap.

## Foundational performance work — linked DSP execution

User says “up to16 now” on idle-pairs-vfp-1 and wants a more foundational change. Log inspected: `/tmp/raverace-vitatv-idle-pairs-vfp-1.log`. Moving race frames1080–1440: sound8–9ms (previous14ms), DSP16–19ms, combined devices25–29ms, CPU12–13ms, prepare13–16ms, draw10–13ms when HUD is mostly static. Dynamic HUD windows raise draw to18–20ms. Typical total62–70ms agrees with peak16FPS. User clarified “cop the log” means inspect it.

In development: `tools/gen/c25_translate.py --runner-out` emits a linked, budgeted DSP runner that stays inside generated code between instructions and jumps directly to translated fallthrough labels. Shared `engine/c25/c25_boundary.h` retains original IRQ/timer/idle behavior; all program-word guards, repeats, bus operations, hooks and faults remain. Vita board calls bulk runner instead of repeated c71_step -> function pointer -> address switch. The reference single-instruction executor remains available. Equivalence passed: 4202 full linked runs vs single-step board loop under ASan/UBSan; 10000 boundary states also matched the pre-refactor core. Host harness uses -O0 because optimizing all generated cases in Clang took excessive time. Initial Vita build hit Thumb conditional branch range after boundary duplication; helper now explicitly noinline to retain one compact shared boundary implementation. Rebuild succeeded; directly installed and launched using Companion quit/launch. Uploaded executable SHA256 verified:4500dae433a8e18b0815361a01fdae312e6ebf8c688ea77d8815c914d2a4bf37. Rollback `/tmp/raverace-before-linked-dsp.eboot.bin`. Startup ID linked-dsp-1. User asked to enter race/report FPS and behavior; pending. New runner text size0xd57a0 vs single-step0x99a34; boundary helper0x220 bytes, no Thumb branch-range issue.

## Latest measured state — 2026-09-29

Current target: Vita TV `192.168.100.158`, FTP1337 and VitaCompanion1338. User clarified manual installation was ONLY for first deployment to this device. Subsequent updates: Companion `quit RAVERACER`, FTP replace `ux0:/app/RAVERACER/eboot.bin`, verify bytes/hash, Companion `launch RAVERACER`. Do not ask the user to reinstall each VPK. This supersedes all chronological deployment notes below.

- User confirmed FPS is unchanged after the second batch package. FTP log `/tmp/raverace-vitatv-current.log` confirms the latest build: `[CLOCK] CPU=444 bus=222 GPU=222 crossbar=166`, `[BATCH] memory=CDRAM`, `[BATCH_TIME]`. One scene draw contains ~2580 triangles; this is not an old build or renderer fallback.
- Second batch includes C25 opcode specialization (2240 state/memory/fault comparisons), viewport trivial accept/reject, release of unused baked texture pool before batch allocation, and detailed batch timings. C25 compiler experiment completed; specialization is in source and installed.
- Stationary race frame600: CPU12.55ms, DSP10.25ms, sound14.36ms, mix6.49ms, combined devices29.81ms, prepare28.24ms, draw21.11ms. Batch build11.34ms, upload2.29ms, submit0.03ms. Total remains ~92ms: no meaningful overall gain. Moving race frame840: CPU12.17, devices33.74, prepare17.76, draw11.80ms. DSP/sound/mix overlap; do not add their individual times.
- First batch framebuffer `/tmp/raverace-vitatv-batch.png` was inspected: scenery, cars, road, mirror, and aligned HUD present. Do not claim the current speed is acceptable.
- Candidate built and uploaded as `ux0:/data/raverace-batch.vpk` (17,727,262 bytes, SHA256 `5712c9e779f9b7d957b28fcfef6bcf66e1ccdf86499e12ba40d829a0fa3f9e84`). Startup identifier `[BUILD] idle-pairs-vfp-1`. Direct deployment completed: executable read-back SHA256 matched, Companion launched the app, and startup log confirms `[BUILD] idle-pairs-vfp-1` and batch renderer in CDRAM. Awaiting race FPS. This TV uses `quit RAVERACER` (old `destroy` returns unknown command). No build/upload sessions running. Changes: sound skips whole pairs of idle executor chunks (135 cycles), bounded by next peripheral event and original slice endpoint, refusing pending interrupts/stopped/faulted CPUs. 524264 paired-chunk/event comparisons plus original130560 wait cases pass ASan/UBSan.
- This candidate also replaces common ARM geometry software 64-bit division with VFP double quotient and integer remainder correction, retaining fallback for large quotients/denominators. 400000 projection-coordinate comparisons, 20000 complete polygons and 200 parallel scenes pass. Hardware improvement not yet measured.

## Target change explicitly authorized by user — 2026-09-29

User: “let's go back to 192.168.100.158 1337. deploy there, my vita tv on ethernet. i will install”. **Current target is Vita TV 192.168.100.158, FTP 1337.** The old prohibition below is superseded by this explicit instruction. Upload the VPK to `ux0:/data/raverace.vpk`; user will install. Do not assume companion/restart authorization is needed for this manual installation request.

Current performance work after the prior 10 FPS report:
- Generic semaphore/atomic worker in `raverace/include/rr_vita_worker.h`, sound + C352 mixing on USER_2 concurrently with each DSP slice, joined before the 68K resumes. DSP and sound use disjoint board memory during the pause. `test_vita_worker.py` validates 10000 handoffs, visibility, shutdown and fallback using host pthread primitives. User reported 11–12 FPS, sometimes 14–15; core 3 active and core 1 ~80%.
- Texture reclamation tracks the retired unit range rather than scanning the entire bitmap every frame. Allocator regression passes; reclamation stage dropped ~3.9 ms to ~0.2 ms on hardware.
- Parallel geometry: immutable per-object view jobs collected by `eng_walk_objects`, dynamic queue shared by main and a USER_2 geometry worker (sound worker idle during frame rendering). `engine/geo_hw_lane.c` compiles the same geometry implementation with private state symbols, avoiding shared camera/lighting state. Results are reassembled in original emission order before sorting. `test_geometry_jobs.py`: 200 full parallel scenes byte-identical to serial including lighting/object flags. Built and installed on Blue Vita; runtime confirms objects split between lanes, no fault reported. At ~2057 quads, prepare ~25.5 ms, draw ~21.5 ms; only modest geometry gain due copying/synchronization or imbalance, needs investigation.
- Latest local VPK is the above build; uploaded and size-verified on Vita TV at ux0:/data/raverace.vpk (17,716,323 bytes). A further VPK was subsequently uploaded as `ux0:/data/raverace-batch.vpk` (17,723,001 bytes), containing the new batched renderer and dirty HUD spans; user asked to install/launch and report FPS + visuals, response pending.
- Batched renderer implementation: `engine/quad_gxm_batch.inc` included from `quad_gxm.c`. One immutable 4096x4096 U8 pen-ROM texture, 256x4096 RGBA tilemap with decoded attribute nibble, 256x128 RGBA palette updated by generation, CPU cached triangle staging and one ordered draw. Fragment shader performs original tile flips/transpose, bank/cmode/palette lookup, brightness and fog. Perspective UV and per-viewport clipping retained; HUD uses existing destination-alpha priority. This removes baked per-quad textures and individual scene draw calls. Note: fog is now one shader pass (small quantization differences possible), raw ROM sampling has full texture resolution instead of 256-texel bake cap. No hardware visual validation yet.
- `raverace/tests/test_gxm_batch.py` transpiles the actual Cg pixel formula to host vector C++, executes 200000 samples against original CPU texture lookup (all banks/cmodes/flips/wrapping), ASan/UBSan passes.
- Batch shader initialization failure logs and falls back to prior baked renderer. To force fallback, create `ux0:/data/raverace_batch.disable` before launching. Memory partial-failure cleanup present. Need inspect hardware shader compiler and `[BATCH]` logs.
- HUD uploads now use dirty 64-byte spans within changed rows. `test_gxm_hud_upload.py` checks 100 frames, sparse edits, full frames, unchanged frames and padded strides against full-copy reference. Main decode test still passes 448 frames.
- Worker timing fields added under __vita__: dispatch/start/finish timestamps and actual CPU ID. `[DEVICES]` adds wake delay and core to diagnose incomplete overlap.
- Background experiment compiling generated C25 with an always-inline c25_exec using a TEMPORARY header under `/tmp/rr-c25-inline`, exec session 64486. This is not a source change, not installed. Check completion before using results. It aims to remove runtime opcode decoding still present despite the generated constant opcodes. All ninja builds finished; no concurrent ninja.

## Current state — 2026-09-29 multicore and geometry builds

This section supersedes the chronological notes below.

- Visuals and speedometer are user-accepted. Keep texture cap 256 and current projection/fog/HUD behavior.
- Sound shortcut, projection fast paths, index sorting and changed-row HUD uploads were installed. Sound CPU time dropped from ~31 ms to ~13 ms, but user still reported 7 FPS and one busy core.
- Latest binary **installed and launched successfully on Blue Vita 192.168.100.42**: palette conversion and HUD decoding execute on a semaphore/atomic-synchronized worker pinned to USER_1. Main thread assigned USER_0. Hardware worker ID 1073873217; affinity change returned previous mask 0x70000. Board emulation is paused until worker completion; GPU work stays on main thread.
- HUD input snapshots skip decoding when text RAM, character RAM, scroll attributes and selected palette are unchanged. `python3 raverace/tests/test_gxm_hud.py`: 448 frames/input mutations compared to original per-pixel decoder under ASan/UBSan, with unchanged-frame reuse checked.
- Runtime log `/tmp/raverace-worker.log`, race frame 600, qn=2057: cpu=11.12, dsp=11.13, sound=13.11, mix=5.89, prepare=46.44, draw=25.13 ms/frame. Worker work=7.84, join=0.01 ms. Rendering: palette/wait/reclaim=3.98, quads=16.47, text/upload=4.24. Cache hits=77131 / misses=19 per 60 frames.
- Previous race frame 600 `/tmp/raverace-speed-race.log`: cpu=10.78, dsp=11.10, sound=13.18, mix=5.43, prepare=45.44, draw=48.87. Approximate total improves 135 to 113 ms (~7.4 to ~8.9 FPS), not full speed. User confirmed 9 FPS, second core 4–18%, first core 100%.
- Follow-up profiling measured geometry=36.2 ms and sort=9.5 ms at ~2056 race polygons.
- **Latest installed build** adds: reuse raw projected vertices when all four vertices are in front of the near plane (avoids duplicate divisions); compact depth/order sort keys and indirect drawing (avoids permutation-copying 1044-byte polygon structs). Near-plane/guard clipping and sort tie behavior are preserved. `test_gxm_geometry.py` compares 20,000 complete output polygons against the old path under ASan/UBSan with the ARM fallback forced. `test_gxm_math.py` checks indirect and physical ordering against original qsort.
- New build installed and tested; user confirmed **10 FPS** (did not explicitly answer the visual-regression question). `/tmp/raverace-geometry-speed.log`, frame 600, qn=2057: geometry=26.58 ms, sort=1.93 ms, total prepare=28.78 ms, draw=25.10 ms, cpu=11.36, dsp=11.17, sound=13.01, mix=5.88. Total ~95 ms (~10.5 FPS). Compared with the original ~135 ms, ~40 ms removed. Worker join=0.01 ms. Worker shutdown now joins and deletes its thread/semaphores on normal close. No build or deployment pending.
- Geometry (now ~26.6 ms), quad submission (~16.5 ms), sound (~13 ms), main CPU (~11 ms), and DSP (~11–22 ms depending on driving) remain significant. Texture retirement scans all 262144 units each frame even without retirements; consider a bounded retired range/list or dirty flag (palette/wait/reclaim currently ~3.9 ms). Main CPU / DSP / sound are still serial; do not imply all emulator work is parallel now. Avoid arbitrary CPU budget reductions.
- Last build completed successfully; no build session running. No subagents used. Preserve existing dirty work.

# Rave Racer Vita diagnostic status — 2026-09-29

Target: Blue Vita 2000, 192.168.100.42 (FTP 1337, companion 1338).
Never contact/deploy to 192.168.100.158. User explicitly authorized installation and testing on the Blue Vita.

The original handover is at `/Users/chan/.gemini/antigravity-cli/brain/f67b0136-0a11-4477-929a-50c0d7e340dc/HANDOVER_RAVERACER_VITA.md`.
Its “black screen resolved” statement did not establish working 3D rendering.

## Confirmed on hardware

- Shader attributes must use reflected resource indices. The shader reports position=0, UV=4, color=8; the original code used 0,1,2.
- After this fix, the user confirmed models in attract mode, but reported broken polygons and a lime/green screen in races. Do not describe the game as visually fixed.
- No CPU traps or DSP faults appeared in the first diagnostic run.
- At frames 420–480, CPU ~12 ms, DSP ~10–11 ms, sound MCU ~31 ms, mixing ~1 ms, scene preparation ~39–44 ms, rendering ~40–44 ms. Roughly 7–8 FPS. Audio was missing ~41k–43k of 48k output samples/sec.
- CPU budget remains unchanged at 83,000 polls/frame. Increasing audio buffering alone cannot correct this throughput deficit.

## Latest build installed; race appearance still needs verification

- Reflected vertex attribute bindings, per-draw projection uniform reservation, initial draw error logging.
- Perspective-correct texture interpolation: carry u/z, v/z, 1/z to fragment shader; divide there.
- Texture pool no longer wraps over live allocations or the white texture. Uses the actual allocation size, including the 16MB fallback. Orphans are retired until GPU work completes.
- Handle exhaustion returns failure rather than stealing texture handle 1. Failed uploads are not cached.
- Vita texture cache budget 8MB, 4096 entries, 8192 texture handles. Freed entries release pool allocations instead of retaining a desktop-sized free list.
- Wait for prior GPU work before reuse of vertex/text/retired texture memory. Vertex overflow skips rather than overwriting current-scene vertices.
- Nearest texture sampling, padded linear row stride.
- HUD decoding hoists tile-row/palette work out of the pixel loop. 64 randomized frames matched the prior implementation for visible RGB and alpha, including scrolling/flips/transparency.
- Optional capture: create `ux0:/data/raverace_capture.request`; every 8 emulated frames the renderer checks it, removes the marker, waits for rendering and writes `ux0:/data/raverace_capture.ppm`. Ordinary companion PS+Start capture produced no screenshot.

## Validation

`VITASDK=/Users/chan/vitasdk PATH=/Users/chan/vitasdk/bin:$PATH ninja -C raverace/build-vita`

`python3 raverace/tests/test_gxm_allocator.py`

Host allocator checks run with ASan/UBSan: 16MB fallback bounds, exhaustion, deferred reclamation, preservation of live texture data, white texture reservation, row stride, handle exhaustion.

Latest runtime startup confirms both updated shaders compile and quad_gxm_init succeeds. Performance and racing geometry remain unverified for the latest build.

The working tree already contained substantial uncommitted Vita work before this session. Do not reset it wholesale.

## Follow-up findings

User confirmed improved polygons in attract after perspective/memory changes, but races remained mostly lime green. A framebuffer capture confirmed the overlay with faint mirror geometry. Two further discrepancies from the desktop renderer were found:

- Per-quad viewport clipping was calculated but disabled. CPU clipping now uses each quad's exact rectangle (mirror included); invisible polygons are rejected before texture baking. Intersection positions are snapped to the clip boundary to avoid floating-point excursions.
- `s22_fog_quad` used incorrect mixer RGB offsets and inverted fog strength. Replaced with the desktop function: RGB at 0x100/0x180/0x200 + cz_color, no pass for ff=0, alpha_const=255-ff. Fog color also receives the vertex shade when fog_before_shade is set.

A diagnostic 64-texel cap eliminated some baking pressure but was visibly blocky; it was reverted to 256 before the latest installation. Pool exhaustion occurred in the pre-clipping 256 build and must still be monitored.

The ARM projection divider now uses native 64-bit division when the high numerator word is zero. 200,000 cases compared exactly with host unsigned __int128. Host viewport tests verify clipping bounds and interpolants. Run `python3 raverace/tests/test_gxm_math.py`.

The latest installed build includes the fog fix, clipping, 256-texel cap, and projection fast path. Awaiting race confirmation for that exact build. Prior captures can add several seconds of storage I/O to timing samples; disregard those windows for steady-state performance.

## Latest hardware feedback and pending build

- Fog fix verified by framebuffer: road, cars, buildings and scenery visible. User confirmed everything looked good except the needle, and clarified countdown numbers are fine.
- User noted the needle is 3D and the HUD itself is stretched. Confirmed in code: HUD was stretched to all 960 pixels while geometry uses Hor+ scene extents.
- **Built but NOT yet installed**: HUD now drawn through the same projection as geometry, with desktop-style destination-alpha priority; clear alpha is zero and fog preserves alpha. This aligns the dial and needle without moving the needle. Text filtering uses nearest sampling like desktop.
- **Built but NOT yet installed**: pool now requests 64MB CDRAM (fallback 48/32/24/16); cache budget uses 75% of actual pool. Previous 8MB cache caused ~35k misses across a 60-frame race window, ~410ms/frame in quads. 256-texel detail retained.
- **Built but NOT yet installed**: sound PC sampling at slice boundaries, `[SND_PROFILE]`, to find hot loops.
- Last deployment attempt timed out before the destroy command connected; currently installed build is still the fog/clipping/projection-fast-path version.

## Speed-only phase

The HUD/cache build was subsequently installed successfully. Hardware allocated pool=64MB/cache=48MB. User confirmed speedometer fixed and reports about 7 FPS, one core at 100%, two idle. Treat visuals as user-accepted, countdown was already fine.

Pending speed build:
- Vita-only generated sound executor calls `rr_snd_skip_wait` from `src/snd/snd_wait.h`. Two BIOS loops (C0DD, C0FF) are LDA $81 / BEQ -4. Shortcut checks actual BIOS bytes, bank, direct page, M/X, and zero internal RAM byte. Skips complete 9-cycle iterations only up to the next timer/ADC event or current chunk deadline. Preserves A, flags, fetch count, PC, and cycle count.
- 130,560 deadline/event-boundary comparisons passed against actual m377 instruction semantics under ASan/UBSan. Test: `cc -O2 -fsanitize=address,undefined -Iengine/snd -Iraverace/src/snd raverace/tests/test_snd_wait.c -o /tmp/rr_snd_wait_test && UBSAN_OPTIONS=halt_on_error=1 /tmp/rr_snd_wait_test`.
- Projection fast path bypasses emulated 128-bit multiply/shift for int32 coordinates and int16 DSP mantissas; wide fallback retained for clipped extremes. 200k randomized cases matched the native desktop formula, added to `test_gxm_math.py`.
- Network timed out again before installing these speed changes. User was asked to restore connectivity. Latest installed version is HUD/cache; new sound/projection build remains local until successful deployment.

Additional local speed changes: cached CPU copy of last uploaded HUD, transfer only changed rows (100-frame test matches full uploads with padded strides); polygon sorting now sorts uint32 indices then applies permutation cycles, preserving full-struct qsort output for both tie modes across ~270 randomized scenes. geo_quad is 1044 bytes; avoiding repeated struct swaps should reduce scene-preparation traffic. No FPS claims until hardware measurement.

Build caution: overlapping ninja invocations during the long sound recompilation damaged dependency metadata. Do not run concurrent builds. Let active build complete, then run `ninja -C raverace/build-vita -t recompact` to repair metadata before another build if needed. Current sound recompilation can take several minutes. Connection often drops during gameplay; ask user to keep VitaShell FTP screen open when ready to deploy.
