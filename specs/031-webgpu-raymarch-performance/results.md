# WebGPU ray-march performance results

Protocol: every variant is measured with the full bench corpus (6 models x {256, 512, 1024})
and validated against the stored HQ references (`WebGPUHqQualityParity`) plus the LQ
recognizability gate (`WebGPULowQualityGate`). Baseline:
`baselines/performance.json` (radv / Dawn, `linux-webgpu-releaseWithDebug`).

## E1 - Workgroup tile shape sweep

Mechanism: `compute::WorkgroupShape` travels through `RenderSettingsSnapshot` ->
`FrameRequest`; `WebGPUFrameShaderComposer::applyWorkgroupShape` rewrites the
`@workgroup_size` declaration and `WebGPUDispatchPolicy::calculateSliceDispatchSize`
computes the matching dispatch. Non-default shapes get their own pipeline through the
shader-text cache key. Bench selection: `GLADIUS_BENCH_WORKGROUP=<w>x<h>`.

Median wall-time delta vs baseline across all 18 samples (negative = faster):

| shape | median | worst regression | median GPU pass | verdict |
|---|---|---|---|---|
| 1x256 | -12.3% | -1.6% | -12.3% | **accept candidate** |
| 2x128 | -11.0% | +0.4% | -12.0% | accept candidate |
| 1x128 | -10.3% | +2.7% | -9.1% | borderline |
| 4x64  | -10.1% | -1.5% | -10.0% | accept candidate |
| 2x64  | -9.6%  | -1.6% | -9.4% | ok |
| 2x32  | -9.0%  | +1.2% | -9.4% | ok |
| 1x64  | -8.8%  | +1.3% | -8.8% | ok |
| 4x32  | -7.9%  | -1.3% | -8.9% | ok |
| 4x16  | -7.8%  | +2.0% | -7.4% | ok |
| 8x32  | -1.2%  | +7.3% | -1.8% | ok |
| 16x16 | -0.7%  | +17.3% | -1.0% | current default |
| 32x8  | +1.3%  | +41.4% | +1.9% | reject |
| 64x4  | +3.0%  | +36.9% | +3.3% | reject |
| 128x2 | +3.2%  | +76.6% | +3.9% | reject |
| 256x1 | +4.6%  | +109.2% | +4.8% | reject |
| 16x1  | +23.0% | +99.3% | +24.4% | reject |
| 1x8   | +39.6% | +126.9% | +43.3% | reject |

Findings:

- Steps/ray, hits/ray and all counters are identical for every shape (max step delta 0.00):
  the tile shape does not change the algorithm, only scheduling and memory access patterns.
- HQ parity stays bit-exact (PSNR inf, SSIM 1, max channel diff 0) for all tested shapes,
  including the accepted candidates; the LQ gate passes unchanged.
- Tall tiles (height >> width) win: the row-band mapping makes `global_id.y` the row index,
  so a tall workgroup marches vertically adjacent pixels in one workgroup, which improves
  coherence for the empty-space skipping. Wide tiles lose badly at 1024 (few workgroups in
  the x direction and masked-out columns waste invocations).
- Repeat runs (3x per candidate) confirm the ranking; per-sample noise is ~2-4%.

Decision: `1x256` is the strongest single change (-12.3% median, no sample regressed).
`2x128` and `4x64` are close secondaries and may interact differently with later
experiments (E4/E7 tile culling prefers square-ish tiles), so E1 stays flag-selectable
and the default is re-evaluated after the combination run.

## E2 - Background-ray AABB skip

Mechanism: flag `RF_SKIP_BACKGROUND_RAYS` (bit 5). Before marching, a ray is tested
against a conservative skip box built in WGSL from the clipping box (model bounds), the
slice-height z range, the helper platform/axis box `(-2,-2,-22)-(402,402,402)`, and a
margin of `max(boundsErrorBound, 1.0)`. The error bound travels through
`RenderRequest::boundsErrorBound` -> `FrameRequest::boundsErrorBound` and is packed into
the previously unused `FrameUniforms::verticalScaleAndMaxSteps.z`. Rays proven to miss
the box write the background color directly and never march. Bench selection:
`GLADIUS_BENCH_FLAGS=<hex>`.

Validation: HQ parity is bit-exact (PSNR inf, SSIM 1, max channel diff 0) on all 12
reference frames with the flag on; the LQ gate passes unchanged. A headless behavior
test asserts a camera far outside the bounds marches zero steps (`totalSteps == 0`)
while the unflagged path marches, with identical background pixels.

Measured effect (full corpus A/B, flag off vs on):

- Steps reduced by only ~0.03-0.05% (e.g. webcam_003@1024: 65167211 -> 65146711 steps,
  hits identical). The skip almost never fires in the neutral HQ path.
- Wall-time deltas are dominated by run-to-run noise: three repeat flag-off runs of
  webcam_003@1024 spanned 8.7-12.2 ms median (the 8.7 ms value is the original A/B
  "off" run), so the +11.9% flagged by `bench_compare.py` on that sample is noise,
  not a real regression. Alternating off/on runs of SimpleGyroid@512 (3 pairs) show
  overlapping ranges (off 5.58-5.79 ms, on 5.63-5.75 ms), confirming the apparent
  +14% regression on that sample is noise too. GPU-pass deltas are within noise.

Findings:

- The skip box must cover everything `march_surface()` can hit: model bounds, plate,
  coordinate axes, field/stack overlays, plus the bounds-service error bound (harness
  probe 64^3 gives errorBound ~32.5, so the box spans ~ -34.5..434.5). The harness
  camera is framed tightly around the model with `maxTravelDistance` 100000, so nearly
  every ray enters the box and the AABB test is pure overhead for marching rays.
- The mechanism is correct and safe (bit-exact background, zero steps for far cameras)
  but gives negligible benefit on the neutral HQ path as designed.

Decision: keep the flag-gated mechanism (default off) as infrastructure for LQ/preview
paths where the camera can be outside content; do not adopt it as an HQ default. A
tile-level variant (E4/E7 macrocell culling) is the more promising form of the same
idea because it amortizes the AABB test per tile instead of per ray.

## E3 — Fast shadow / ambient-occlusion shading (LQ-only)

Mechanism: two new flag bits reduce the per-hit shading cost in `frame_compute.wgsl`.

- `RF_FAST_SHADOW` (bit 6, kernel `types.h`, ABI mirror `64u`): `calc_soft_shadow()`
  marches 10 taps instead of 32, clamps step size to 1.5..3.0 instead of 0.5..1.0, and
  exits early at shadow value < 0.02 instead of < 0.005.
- `RF_FAST_AO` (bit 7, ABI mirror `128u`): `calc_ambient_occlusion()` samples 4 taps
  instead of 8, spacing 0.06 instead of 0.03, decay 0.9 instead of 0.95. Metric 6 now
  records the actual tap count (previously hardcoded `8u`).

Both flags are ORed into `createLqSettings()` in `WebGPUQualityReferences.h`; HQ
settings never set them, so the HQ path is untouched by default.

Counters (median-of-3 A/B runs, flags 0 vs `c0`, instrumented counter pass):

- `shadowTaps` reduced 58.5-62.9% across all 18 samples (32 taps/ray -> ≤10 taps/ray,
  early exit cuts most of the remaining taps on lit surfaces).
- `aoTaps` halved exactly (8 -> 4 taps/hit).
- March `totalSteps` unchanged (shadow/AO taps are not march steps).

Wall time (median of 3 alternating off/fast corpus runs): geomean −0.8%, worst sample
+6.2% (RadialRadiator@512), no sample beyond the ±10% noise band established in E2.
GPU-pass timestamps: geomean −2.0%, worst +9.1% (same RadialRadiator@512 sample).
RadialRadiator and lampbase regress within noise; gyroid/sphere/webcam samples improve
up to −9.7% GPU.

Quality: HQ parity stays bit-exact (flags off by default). With fast shadow/AO active,
the LQ gate passes on all 12 frames: worst mask IoU 0.9956, worst SSIM 0.9943, worst
mean foreground color error 1.63, worst histogram overlap 0.961 — all comfortably above
the 0.90/0.90/≤12/≥0.95 thresholds.

Decision: keep both flags as LQ/preview knobs (default off for HQ). The measured wall
benefit is modest and noisy on this GPU, but the tap reduction is exact and the LQ
quality cost is small; revisit adoption in the combination run (E-combine) where
multiple LQ knobs stack.

## E4 — Macrocell grid empty-space skipping (rejected)

Mechanism tried: `RF_MACROCELL_GRID` (bit 8, ABI mirror `256u`). A compute prepass
(`macrocell_prepass.wgsl`, `@workgroup_size(4,4,4)`) writes a per-cell lower bound of the
distance field into a storage buffer (binding 11, shared bind group with the frame pipeline);
the march loop queries the grid and jumps to the nearest forward cell plane when the cell is
provably empty. Cells intersecting analytic overlay AABBs (build plate, axes, field slab,
stack slabs) are marked non-skippable since overlays are not part of `evaluateModel()`.

Counter effect (A/B, flags 0 vs `100`): march steps −27.6..−34.9%, refinement taps ~halved,
hits identical — the skip logic works.

Performance effect: **net slowdown**. Wall/GPU +5..+25% on most samples (median of 3
alternating runs; 8/18 samples regress >10%). The per-step grid lookup is a random-access
storage read that costs more than the sphere-tracing steps it removes; a `probe_distance`
gate (skip the lookup when the last sample is already within one cell of a surface) did not
change the outcome.

HQ parity: **fails** for every model containing mesh geometry (lampbase 68–70,
SphereInACage 188–196, RadialRadiator 116–119, webcam_003 212–222 max channel diff; gate ≤16).
Pure implicit gyroids pass (maxdiff 13). Root causes found along the way (see findings.md):
non-1-Lipschitz composed field (`omega = 1.6`), wrong AABB exit axis (`max` instead of
`min` plane distance), per-axis-max instead of gradient magnitude, mesh SDF sentinel
discontinuities. Even the most conservative bound settings (near-never-skip) leave mesh
models failing, so the mesh SDF path cannot be safely bounded with 7-point sampling.

Decision: **rejected** — violates both decision rules (no speed gain, HQ parity broken).
All E4 code reverted; the idea remains documented in findings.md as a potential for
implicit-only scenes if a provably-safe bound is ever found.

## E5 — Distance-init buffer (rejected)

Mechanism tried: `RF_DISTANCE_INIT` (bit 8, ABI mirror `256u`) + `RF_DISTANCE_INIT_WRITE`
(bit 9, `512u`). A low-res prepass (res/4, HQ camera framing) marches each ray from 0 and
stores the first-hit distance into a storage buffer (binding 11, always bound); the main
pass samples it (MIN of the 4 nearest texels, conservative) and starts the march at
`init_distance * 0.9` — the same 0.9 safety margin the OpenCL kernel uses
(`rendering.cl:185-215`, `AM_USE_DISTANCE_INIT`). First-sample validation reverts to a
march-from-0 fallback when the jump lands inside solid geometry.

HQ stays bit-exact by design: distance-init is masked out of HQ settings (only preview/LQ
may use it, mirroring OpenCL where only the preview pass sets `AM_USE_DISTANCE_INIT`).
HQ parity with flags `300`: 12/12 bit-exact, PASS.

LQ gate with E5 on: RadialRadiator fails the histogram-overlap criterion (0.916 @256,
0.949 @512 vs ≥0.95) — the quarter-res init field shifts shading on thin radial fins.
IoU/SSIM/color-error criteria pass.

Performance (LQ bench mode `GLADIUS_BENCH_LQ=1`, median of 3 alternating runs):
steps −8.0..−8.6% on RadialRadiator, −1.9..−2.2% on lampbase, ~0% on the procedural
gyroids (their rays already converge quickly); **wall +5..+50% everywhere** — geomean
0.846x (a slowdown). The extra prepass submission costs more than the steps it saves,
the same failure mode as E4. On the HQ path the effect is noise-level (geomean 1.02x).

Decision: **rejected** — net slowdown on the path it targets, and it degrades the LQ
recognizability gate on mesh-heavy models. All E5 code reverted. The idea stays viable
only if the init field comes from a *free* source (e.g. the previous frame's depth at the
same camera — see temporal reprojection in E8) rather than a dedicated prepass.
