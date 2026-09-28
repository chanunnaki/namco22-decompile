# Cached glyphs and native scene texture banks

## HUD architecture

The old GPU HUD interpreted the character map, packed nibbles, flips and palette at every pixel of a full-screen quad. The replacement resolves visible tilemap cells on the CPU and draws a single triangle batch sampling an RGBA glyph atlas. Fully transparent glyphs emit no geometry. Cached glyphs are keyed by character code and palette bank; flipped cells share the same pixels and change only UV orientation. Scrolling is handled by cell positions and clipped UVs.

The cache holds 2048 glyphs and a direct 16384-key lookup. At most 1271 tilemap cells can intersect the 640x480 logical HUD, so current-frame references cannot exhaust the cache. Cache entries compare the 128 character bytes and 64 palette bytes when first referenced in a frame. Codes 960–1023 still alias tile RAM. Pen 15 becomes transparent; RGB-only destination-alpha blending preserves the speedometer/scene priority behavior.

Three 2 MiB GPU atlas copies have independent version stamps. Only dirty referenced glyphs are copied into the selected, already-fenced frame slot. Indices are immutable; HUD triangle vertices use the existing per-slot vertex ring. No worker or GPU reads an atlas while the CPU overwrites that slot. Logical HUD placement and display resolution are unchanged.

Marker `ux0:/data/raverace_hud_decoder.enable` selects the preceding GPU tilemap decoder. The older CPU-expanded HUD fallback remains separately selectable with `raverace_hud_legacy.enable`.

## Scene architecture

One 4096x4096 U8 pen texture is decoded from the arcade ROM tilemap into a 16 MiB CDRAM cache. The bank is chosen by summed projected area of visible textured triangles. A four-to-one hysteresis keeps the current bank unless another is substantially more important. Allocation failure retains the reference path.

Cached triangles use a dedicated small fragment program with native wrapped texture sampling and palette lookup. Tilemap lookup, attribute decoding, flips and transpose are absent from that program. Palette/color mode, lighting, fog and alpha priority remain dynamic and unchanged. Uncached triangles use the original fragment program. Contiguous native/fallback runs preserve exact painter order without sorting by material or changing triangle data.

Bank planning occurs before BeginScene. Before replacing cached pixels, the renderer drains all preceding GPU reads; the cache is not overwritten while queued frames reference it. First-use expansion and bank changes can pause a frame. The comparison separates warm-up from steady-state driving. This cache does not promise to make every bank resident or eliminate all transition hitches.

Marker `ux0:/data/raverace_scene_decoder.enable` selects the original scene fragment shader. The initial attempt at three permanent banks was limited to one bank by available CDRAM and retained a startup bank covering only ~3% of the race; the final design replaces that with coverage-based selection and a specialized program.

## Validation and initial HUD result

- 30,720,000 HUD pixels match independent full-screen decoding across scrolling, flips, palette/character mutations, transparent glyphs, RAM aliases and cache eviction. Rebuilding an unchanged frame decodes zero glyphs.
- 50,331,648 predecoded scene texels and 600000 samples of the actual native shader match the original texture oracle. Original 200000 scene-shader and 640000 HUD-shader samples still pass. Tests use ASan/UBSan.
- HUD-only same-binary 1800-frame test: all 30 state/budget/geometry/speed and scene triangle-count/CRC checkpoints match; both runs record clean shutdown. Frame median 40.430 -> 33.640 ms (16.8% reduction), render job 35.600 -> 20.840 ms. User observed a 33 FPS peak. HUD pixel equivalence is established by the host decoder test; the scene triangle CRC does not validate GPU HUD pixels.

Evidence: `RAVERACER_HUD_ATLAS_BENCHMARK.csv`; local logs `/tmp/raverace-shader-baseline.log`, `/tmp/raverace-shader-hud-native.log`.

The separate draw-run test validates 3,600,000 submitted vertices against the original CPU stream, including fallback selection and stream/index offsets across run boundaries. Both shaders retain the original triangle order.


## Combined hardware result — 2026-09-29

Same-binary original-decoder/native-decoder runs each complete 1800 scripted frames, selecting a track and accelerating into the race. All 30 CPU state/budget, geometry, speed and final scene triangle-count/CRC checkpoints match. Both runs terminate cleanly. Medians below cover 11 moving-race windows, frames 1200–1800; overlapping timings must not be added.

| Metric | Original decoders | Native glyphs and scene bank |
| --- | ---: | ---: |
| Total frame | 40.630 ms | 33.510 ms |
| Renderer job | 35.760 ms | 21.450 ms |
| Layer upload | 2.320 ms | 1.180 ms |
| HUD CPU submission | 0.020 ms | 0.380 ms |
| Display queue wait | 14.240 ms | 0.040 ms |

Total frame time falls 17.5%, equivalent to about 24.6 -> 29.8 FPS. The user observed a 33 FPS peak during the HUD-only test. The scene cache adds little total-frame improvement compared with the HUD-only 33.640 ms result; the HUD is the main measured throughput win. Queue/callback latency is not an isolated GPU shader duration.

The resident race bank covers about 25–30% of projected triangle area. Four initial/menu/grid bank loads take 80.97–102.19 ms; no replacement occurs from race frame 480 through 1800. Other tracks or later sections can still incur replacement stalls. This is an opening driving segment, not a complete-lap stability test.

The parallel device phase now takes roughly 20–26 ms, with sound-worker wake delays around 6.6–8.9 ms, CPU around 5 ms and snapshot around 3.8 ms. This points to sound/device work and scheduling as the next investigation; the component timings overlap.

Evidence: `RAVERACER_NATIVE_TEXTURES_BENCHMARK.csv`; local logs `/tmp/raverace-shader-baseline-v3.log` and `/tmp/raverace-shader-native-v3.log`. The comparison tool rejects mismatched inputs/checkpoints, capture stalls and completion-probe runs. Tests also verify 80 mutating triple-atlas frames, 3000 GPU fence handoffs and 600 reference HUD slot updates. Vita compilation and diff checks pass.

## Installed build

`gpu-native-textures-3` is installed on Vita TV 192.168.100.158 through FTP/Companion, with readback-verified SHA256 `572158fc88df11b737650a6a7bca63199794a485b495a161b4c33ec4365dbb92`. This differs from the timed binary only by removing the obsolete `draws=1` log field. Normal controls are restored; scripted benchmark, reference-decoder and GPU completion-probe markers are absent. Startup confirms both new paths active and normal presentation.

Post-benchmark capture `/tmp/raverace-native-textures.png` shows road, buildings, mirror and HUD intact (stationary capture overlay 25 FPS). It is excluded from benchmark measurements. Final combined-build user visual/FPS feedback remains pending. Prior executable rollback: `/tmp/raverace-before-glyph-atlas.eboot.bin`. Either decoder can also be selected independently using the markers above.
