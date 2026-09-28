# GPU completion and presentation trace

The geometry rewrite reduced CPU work without a meaningful FPS improvement. This trace separates the blocking functions inside presentation while preserving the existing GPU and display dependencies.

## Instrumentation

Linker wrappers measure `sceGxmPadHeartbeat` and `sceGxmDisplayQueueAddEntry`. The initialization wrapper retains the original queue flags, pending limit and parameter buffer size, and wraps the existing callback. Its payload copies the original callback bytes together with the scene timestamp, enqueue timestamp, frame and fragment notification. The callback receives its unchanged original bytes. Payloads exceeding 64 bytes fall back to the original callback rather than being truncated.

`PRESENT` reports 60-frame average heartbeat, queue and optional probe waits, plus counts of frames whose fragment notification was complete before queue submission, after the optional wait and immediately after queue submission. `max_pending` counts submissions including the one currently entering the queue; it can be 3 with a configured limit of 2 because the submitting thread is blocked until an older entry completes.

`PRESENT_CALLBACK` records EndScene-to-callback and enqueue-to-callback latency, actual callback CPU time, notification readiness and callback core. Callback statistics have a single owner; only submitted/completed counters are atomic shared state. Ordinary tracing adds no GPU waits and never removes synchronization.

The opt-in `ux0:/data/raverace_gpu_probe.enable` marker waits for the current scene's fragment notification immediately after successful queue submission. This intentionally waits for GPU completion before the render worker can finish the frame. It is a diagnostic experiment, not a proposed performance mode, and changes overlap and queue depth. Its frame time must not be presented as an optimization comparison. Removing the marker and relaunching restores ordinary presentation.

The explicit vita2d vblank wait was already disabled before this work. No screen-resolution, shader, geometry or display dependency changes are part of this trace.

## Validation

The host sanitizer test checks 480 deferred callbacks, original payload lifetime, unchanged sync-object arguments and initialization parameters, and that only probe mode calls the notification wait. The hardware baseline is checked against the preceding geometry build for all 30 game-state and final triangle-stream checkpoints. The probe uses the same binary and the same 1800-frame driving input sequence.

```sh
python3 raverace/tests/test_present_trace.py
python3 raverace/tools/compare_present_runs.py baseline.log probe.log --csv result.csv
```

The installed SDK library was checked in the linked executable: its `vita2d_swap_buffers` calls the queue function directly and does not call `sceGxmPadHeartbeat`. The heartbeat total therefore stays zero because there are no calls. Build 1 placed the probe in that absent call path; those probe results are excluded. Build 2 tried waiting before enqueueing and stalled during boot, exposing a submission dependency. Build 3 waits after enqueueing, requires a valid pending fragment fence, and leaves all queue dependencies intact. The analysis requires every probe window to report 60 completed notifications after the wait.

The final frame's display callback may be cut off by the benchmark process exiting after GPU completion. Analysis still requires all 30 state, triangle and PRESENT checkpoints, but uses the fixed ten complete callback windows at frames 1200–1740 for all reported medians. It does not substitute missing callback data with zero.

## Results

On Vita TV 192.168.100.158, same-binary `presentation-trace-3` runs preserve all 30 CPU/register/WRAM/polygon/shared state, budget, geometry count, driving speed and final triangle count/CRC checkpoints. Ten moving-race window medians:

| Measurement | Normal pipeline | Completion probe |
| --- | ---: | ---: |
| Total measured frame | 40.565 ms | 66.200 ms |
| Explicit fragment-completion wait | 0.000 ms | 40.850 ms |
| Display queue submission | 14.330 ms | 0.040 ms |
| Scene-end to display callback | 92.210 ms | 41.175 ms |
| Callback CPU work | 0.520 ms | 0.440 ms |
| Maximum pending submissions, including blocked caller | 3 | 1 |

In normal mode the current frame's fragment notification is still incomplete when the queue call returns. In probe mode every wait completes it. The display callback always sees a completed fragment notification. Serializing completion removes the queue backlog, but makes total throughput worse (~15 FPS versus ~25 FPS), explaining the low CPU usage observed during the diagnostic. This is evidence of GPU-completion backpressure rather than an expensive display callback. The probe is disabled for normal play.

This measures host-visible completion waiting, not a timestamped isolated shader duration. It does not yet distinguish scene texture decoding, HUD shading, overdraw, GPU memory traffic and other GPU/driver costs. The next architectural target is GPU work, especially the per-pixel arcade texture/tilemap decoding in the scene and HUD fragment shaders. Enlarging the display queue would not remove that work and risks adding latency. Existing shaders are unchanged in this commit.

Evidence: `RAVERACER_PRESENTATION_BENCHMARK.csv`; local logs `/tmp/raverace-present-baseline-v3.log` and `/tmp/raverace-present-probe-v3.log`.

## Benchmark shutdown crash

The user correctly reported a Vita error with a core dump after the probe. `psp2core-1790630766-0x0006013abf-eboot.bin.psp2dmp` contains the stderr message `stop at frame 1800, 0 traps` before the fault. The render thread PC is 0x8167bc48; with the executable's 0x24000 load relocation this resolves to `_free_r` at 0x81657c48, faulting on the allocator's load from the freed/unmapped allocation header. Main had already entered process exit while render and display threads remained alive. The emulation log's zero traps was insufficient to rule out this native shutdown fault.

The bounded-run path now calls `rr_host_close()` before `exit()`. That joins the final render job and geometry worker. Renderer shutdown also drains `sceGxmDisplayQueueFinish()` after GPU completion, so the callback cannot outlive libc cleanup. `[RUN_END] ... host_closed=1` records successful orderly shutdown. Build 4 adds this fix without changing frame rendering or the trace experiment.

Build 4 completed the same full 1800-frame probe with all 30 state and final triangle checkpoints unchanged. The final frame-1800 display callback was logged, followed by `[RUN_END] frame=1800 traps=0 host_closed=1`. FTP inventory showed no new core dump after deployment. The existing 10000-handoff worker test, including pending shutdown, also passes. Log: `/tmp/raverace-present-fixed-exit.log`.

Installed/readback-verified SHA256: `5cb79707e9bb05df11abaffa34eecabb352327b5c0f1b1bbc967f5d6420ddc1b`. GPU probe and scripted benchmark markers removed for normal play. Geometry, native CPU/DSP, GPU HUD and buffered presentation remain enabled. Rollback before tracing: `/tmp/raverace-before-presentation-trace.eboot.bin`.
