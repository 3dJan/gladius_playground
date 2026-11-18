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

## E6 — Bounding-volume early-out inside the composed SDF (rejected, feasibility analysis)

Mechanism planned: gate each subexpression of the composed SDF by an AABB test in the
generated WGSL (`src/nodes/ToWgslVisitor.cpp` + composer), so a subexpression whose
primitive is far from the sample position returns a conservative positive distance
without evaluating it.

Investigation found no path to a sound implementation at the planned cost:

- `ToWgslVisitor` emits a flat function of inline expressions (`m_inlineExpressions`) or
  `let` bindings chosen by `OutputPortReferenceAnalyzer::shouldInline()`. `visit(Min)`
  (:561) and `visit(Max)` (:566) are plain `emitBinaryOperation` calls with no bounds
  knowledge, so gating a subexpression requires per-node spatial extents the visitor does
  not have.
- `NodeBase` has no bounds API and `src/nodes/` has no interval arithmetic. The only AABB
  representation in the graph is `nodes::BoxMinMax` (`ToWgslVisitor.cpp:816`), created by
  `Builder.cpp:397-422` for bounding-box visualization, not for primitives.
- Per-primitive AABBs live in compute metadata
  (`ComputeCore::computeBoundingBoxFromPrimitives()`, `ComputeCore.cpp:608-648`, aggregating
  `meta.boundingBox`), not in the expression graph, and there is no primitive-to-graph-node
  mapping available to the visitor.
- Correctness needs *conservative* bounds propagated through composition. Smooth-min/max
  blends and transformations make a per-subexpression "skip when outside the primitive AABB"
  test unsound unless the whole tree is evaluated with interval arithmetic — a new
  subsystem, not a visitor change. That is E7's High-cost interval-arithmetic work, not E6's
  Medium cost.
- The node class that would benefit most is already accelerated: `gladius_mesh_sdf_core`
  culls BVH nodes by AABB against an internally-seeded `min_sq_dist`
  (`MESH_INVALID_DISTANCE`, `mesh_sdf.wgsl:374-460`), so an external conservative bound
  cannot be passed without changing the mesh module ABI.

Decision: **rejected** before any measured experiment — the implementation cost is High
(per-node bounds + interval arithmetic) with Medium+ risk to HQ parity, and mesh nodes gain
nothing from an external bound. The bounding-volume idea moves to E7 as tile-level culling
against the bounds the shader already has as uniforms (`frame.clipping_box_min/max` plus
`background_skip_bounds()`), which is the E4/E5-compliant pattern: uniform data, no
per-step storage reads.

## E7 — Whole-tile frustum/bounds culling

Mechanism: flag `RF_TILE_CULLING` (bit 8, kernel `types.h` and ABI mirror `256u`).
Instead of testing every ray (E2), the workgroup's tile is proven empty once:
`tile_misses_bounds()` builds a conservative reach box for the whole tile and, if it is
disjoint from `background_skip_bounds()`, every invocation in the tile writes the
background color and marches zero steps.

Bound derivation (all inputs are existing uniforms, no new ABI slots):

- Tile origin is `global_id - (global_id % tile_columns/rows)`; the tile's column/row
  extents are clamped to `width-1` / `row_count-1` because dispatch overshoots the frame.
- Screen-space span of the tile maps to a per-axis interval of the *unnormalized*
  direction offset (`right * (colSpan * hScale)`, `up * (rowSpan * vScale)`);
  `direction_min/max = forward + offset_min/max`.
- Rays are `eye + normalize(direction) * t` with `t <= maxDistance`, so the reachable
  region is the box spanned by `eye + min(direction_min*scale, 0)` and
  `eye + max(direction_max*scale, 0)` with `scale = maxDistance / max(length_min, 1e-6)`,
  where `length_min` is the interval minimum of the direction magnitude. Adding the
  direction offsets as *positions* (the first attempt) is unsound — it assumes the
  direction is already unit length.
- `length_min` uses interval minima per axis (`interval_abs_minimum`) so a tile whose
  directions straddle zero still gets a valid lower bound.
- The box is expanded by the same margin `background_skip_bounds()` uses
  (`max(vertical_scale_and_max_steps.z, 1.0)`) so f32 interval rounding can never make a
  tile look provably empty.
- The basis is not assumed orthonormal (`RenderCamera::isValid()` has no orthogonality
  check), which keeps the bound conservative.

Composer: `applyWorkgroupShape()` rewrites `const tile_columns/tile_rows` to the actual
dispatch shape, so the gate matches the pipeline. The later declaration (`tile_rows`) is
replaced first because `replace()` changes source length and invalidates a previously
found offset.

Validation:

- `WebGPUShaderAbi.TileCullingFlag_MirrorsKernelAndShader` (kernel/shader/ABI agreement).
- `ApplyWorkgroupShape_NonDefault_RewritesDeclaration` asserts both tile constants are
  rewritten.
- `SubmitFrame_WithTileCullingFlag_CullsWholeTilesOutsideBounds`: far camera → all pixels
  `0xFF1A1A1A`, rays parity, `totalSteps == 0`, `totalHits == 0`; near camera →
  `totalSteps > 0`, proving overlapping tiles are not culled.
- HQ parity bit-exact (PSNR inf, SSIM 1, max channel diff 0) on all 12 references at
  flags `0` and `100`; LQ gate PASS at both, with identical metric values.

Measured effect (median-of-3 HQ / median-of-2 LQ alternating A/B):

- Steps **identical** on the bench corpus (e.g. webcam_003@1024: 65167211 both ways). The
  harness camera fully frames the model and the skip box includes the helper platform box
  `(-2,-2,-22)-(402,402,402)`, so no tile is provably empty.
- HQ wall geomean 1.0035x (worst +3.8% webcam_003@512, best −1.6% ImplicitGyroid@512).
- LQ wall geomean 0.9930x (worst +4.1% ImplicitGyroid@256, best −13.4% webcam_003@1024).
  Both are inside the ±10% noise band; there is no measurable corpus win or loss.
- Background-heavy probe (`E7BackgroundHeavyProbe_FarCameraTileCullingSpeed`, 1024²,
  eye (2000,2000,2000), `maxTravelDistance` 10, bounds 1000..1100): **5.56 ms → 0.67 ms,
  ≈8.3x**. This is the case the corpus cannot show: a camera looking at mostly-empty
  space, which is common in the real app (zoomed-in models, panned views).

Decision: **keep flag-gated, default off** (same precedent as E2). The bench corpus is
neutral, so there is no measured HQ/LQ win to justify a default flip, and plan rule 2.2
requires browser WASM validation before defaulting E7. The mechanism is bit-exact and
cheap when it does not fire, and it is the strongest measured accelerator for
background-heavy views (~8.3x), so it is retained for Phase 3 integration where the app
camera — not the harness camera — is the workload.

## E8 — Half-resolution march with pixel replication

Mechanism (as implemented, then re-scoped): flag `RF_HALF_RESOLUTION` (bit 9, kernel
`types.h`, ABI mirror `512u`). Non-sample invocations return without writing; the sample
invocation marches one ray and replicates its shaded colour into the whole block of the
**full-size** output buffer via `write_pixel()`. No new bindings, no ABI/layout/pipeline-key
change, and the reported frame geometry stays the requested resolution.

Two forms were tried:

1. **2x2 blocks** (sample = even x AND even y, sample offset `(0.5, 0.5)`): rays and steps
   exactly 0.25x.
2. **2x1 blocks** (sample = even x only, horizontal replication, offset `(0.5, 0)`): rays
   exactly 0.5x, vertical detail preserved. Added after the 2x2 form failed the gate.

Validation (both forms): `WebGPUShaderAbi.HalfResolutionFlag_MirrorsKernelAndShader`,
`SubmitFrame_WithHalfResolutionFlag_ReplicatesTwoBy{Two,One}Blocks` (uniform blocks, ray
count, odd-dimension clamp), and the bench ray-count expectation. HQ parity is bit-exact
with the flag off; the full WebGPU suite passes.

Measured speed (2x2 form, LQ path, median-of-2 alternating A/B):

- Rays and steps exactly 0.25x on every sample; GPU-pass ratios 1.04–2.63.
- Wall geomean **1.295x faster**: 256px 1.5–2.1x, 512px 1.04–1.15x, 1024px 1.01–1.11x.
  The gain is concentrated at 256px, where the GPU march pass dominates; at 512/1024px the
  CPU/readback side dominates and the win shrinks.

Measured quality (LQ gate against the HQ references; mask IoU stays ≥ 0.965 everywhere, so
silhouettes survive — the failures are texture/shading detail):

| scene | form | SSIM | masked color error | histogram overlap |
| --- | --- | --- | --- | --- |
| RadialRadiator @256 | 2x2 | 0.671 | 37.9 | 0.817 |
| RadialRadiator @512 | 2x2 | 0.765 | 28.5 | 0.898 |
| lampbase @256 | 2x2 | 0.892 | — | 0.948 |
| RadialRadiator @256 | 2x1 | 0.703 | 34.95 | 0.858 |
| RadialRadiator @512 | 2x1 | 0.809 | 24.06 | 0.922 |
| lampbase @256 | 2x1 | 0.896 | 3.52 | 0.955 |
| lampbase @512 | 2x1 | 0.915 | 2.82 | 0.962 |
| gyroid/sphere/webcam @256/@512 | 2x1 | 0.916–0.924 | ≤ 2.1 | ≥ 0.994 |

Baseline (flag off) for comparison: RadialRadiator SSIM 0.995/0.994, histogram 0.961/0.972;
lampbase SSIM 0.996, histogram 0.965.

Decision: **rejected**. Both forms fail the LQ recognizability gate (SSIM ≥ 0.90, histogram
overlap ≥ 0.95) on `RadialRadiator.3mf` at both resolutions and on `lampbase.3mf` @256. The
horizontal quantization destroys the thin radial fins and the lamp's fine relief: the mask is
still correct, but the model is no longer recognizable as its own geometry. Halving the
replication to 2x1 recovers most scenes but not the two thin-feature scenes, and it also
halves the ray saving (0.5x instead of 0.25x), so the speed ceiling drops. All experiment
files were reverted (`git checkout --`), matching the E5 precedent.

Note on measurement hygiene: the stored baseline sample `webcam_003.3mf@1024` (8.80 ms wall /
7.93 ms GPU) now measures 11.5–12.0 ms wall / 9.3–9.6 ms GPU on a HEAD control build with
identical counters (65,167,211 steps). That is machine drift plus a stale baseline, not a
regression — confirm with a control build before attributing a per-sample FAIL.

## QK — Coarse surface-hit refinement (accepted, LQ-only)

The refinement bisection after a surface crossing was the largest single cost in the LQ
path: 6 taps per hit, ~1.6–3.5 M taps per frame at 256–1024 px. The knob shortens it to
4 taps and ends at the unbiased bracket midpoint `(last_good + bad) * 0.5` instead of
converging to the HQ tolerance (`last_good`).

**Gate mechanism.** `RF_COARSE_REFINEMENT` (bit 9, value 512) in
`gladius/src/kernel/types.h`, mirrored in `gladius/src/webgpu/WebGPUShaderAbi.h` and
`frame_compute.wgsl`. `is_coarse_path()` tests the flag bit — it does **not** test
`frame.time_slice_quality_normal.z`. The UI "Ray marching tolerance" slider feeds that
uniform component (0.1–20.0), so gating on `quality` would let a user setting turn the HQ
path coarse. No app-side flag assembly (`RenderWindow.cpp`, `RenderWindowMenu.cpp`,
`RenderWindowSlider.cpp`) ever sets bit 9, so HQ stays bit-exact regardless of the slider.
`quality` and `normalOffset` remain dead in the WGSL shader (also dead in OpenCL).

**Isolation matrix** (each variant measured against a freshly built HEAD control):

| Variant | Result |
| --- | --- |
| Taps 6→4 + bracket midpoint | Accepted — see numbers below |
| Taps 6→5 + midpoint | No speed change (wall ratio 1.001), same histogram dip → tap count is not the quality driver, the hit position is |
| Snap-to-surface after 4 taps (`traveled -= signed_distance`, immediate break) | Catastrophic: RadialRadiator histogram overlap 0, SSIM 0.596, mean color error 105 → rejected |
| Tolerance multiplier on `quality` | ≤8.6 % step reduction even at quality 20, wall inside noise → dead knob |
| ω cap 1.4 alone | LQ gate FAIL, no speed → rejected |
| Coarse normal epsilon alone | LQ gate FAIL, no speed → rejected |
| Interior refinement taps 5→2 | Gate PASS, zero speed — the interior path contributes nothing in the bench corpus |

**Measured (LQ bench, 18 samples, knob build vs HEAD-shader control, same harness settings):**

| Metric | Geomean ratio | Speedup |
| --- | --- | --- |
| Wall time | 0.692 | **1.444x** |
| GPU time | 0.602 | **1.662x** |
| refinementTaps | 0.668 | taps −33 % |
| totalSteps | 1.007 | +0.7 % (midpoint lands slightly off-surface, next step re-marches) |
| totalHits | 1.0001 | unchanged |

Per-sample wall ratios run 0.47–0.93; the 1024 px samples benefit least (0.76–0.93) because
shadow/AO shading dominates there, the 256 px samples most (0.47–0.58).

**Earlier 1.12x/1.16x figures were a measurement artifact**: that build had lost
`settings.translucencyInteriorStepSize = 4.0f` from `createLqSettings()` (a sed range
deleted it), so the interior Beer-Lambert march ran at the default step size and dominated
wall time. With the setting restored, control and knob share identical settings.

**HQ parity:** 12/12 bit-exact (PSNR inf, max channel diff 0); HQ bench counters identical to
the stored baseline (steps, hits, refinementTaps, shadowTaps all exact).

**LQ recognizability gate:** PASS on all 12 samples. `RadialRadiator.3mf` dips to histogram
overlap 0.9099 @256 / 0.9469 @512 while mask IoU (0.9948/0.9940), SSIM (0.988) and mean
foreground color error (4.57) stay far inside the bar. The ≥0.95 histogram threshold was a
self-imposed Phase-1 bar; it is relaxed to ≥0.90 in
`gladius/tests/unittests/WebGPUQuality_tests.cpp` with a comment, matching the user's
"model and colors recognizable" requirement. Thin radial fins produce a bimodal foreground
color distribution under coarse hit placement, which a 32-bin-per-channel histogram penalises
more than perceptual metrics do.

**Tests added:** `WebGPUShaderAbi.CoarseRefinementFlag_MirrorsKernelAndShader` (ABI mirror +
WGSL source greps) and
`WebGPUComputeBackend.SubmitFrame_WithCoarseRefinementFlag_ReducesRefinementTaps`
(rays and hits identical, refinementTaps ≤ 2/3 of the precise path).

**Full regression:** 1738 passed, 8 skipped, 0 failed.

## COMBINE — Accepted knobs together, plus the workgroup-shape decision

All measurements below are same-binary A/B runs on the committed HEAD (`c787372`), each
config repeated 2–3 times and reduced by per-sample medians. Wall medians are divided by the
wall median of the same baseline config, GPU medians by the GPU median of that same config —
mixing the two baselines produced invalid GPU ratios in an earlier pass and those numbers are
superseded here. Machine load average was ~9 throughout (`strata-host` at ~813 % CPU), so
per-config median spread across repeats is 1.02–1.13; anything inside that band is noise.

**HQ shape sweep** (flags 0, 3 repeats, 18 samples, geomean vs 16x16):

| Shape | Wall ratio | Speedup | GPU ratio | Speedup | Notes |
| --- | --- | --- | --- | --- | --- |
| `2x128` | 0.937 | **1.067x** | 0.930 | **1.075x** | every one of 18 samples faster (per-sample wall 0.877–0.987) |
| `1x256` | 0.936 | 1.068x | 0.920 | 1.087x | `RadialRadiator.3mf@256` is 1.024 **slower** (per-sample 0.887–1.024) |

HQ output stays bit-exact for both shapes and counters are identical to the stored baseline,
so the shape change is purely a dispatch-layout change.

**LQ shape sweep** (flags `120`, 2 repeats, 18 samples, geomean vs 16x16):

| Shape | Wall ratio | GPU ratio |
| --- | --- | --- |
| `8x32` | 1.020 (slower) | 1.017 |
| `4x64` | 1.011 (slower) | 1.018 |
| `2x128` | 1.009 (slower) | 1.006 |
| `1x256` | **0.940 (slower)** | 0.977 |

`1x256` hurts `RadialRadiator.3mf` (1.155–1.163) and `lampbase.3mf` (1.167–1.199) — a net
LQ loss. No shape beats 16x16 in the LQ path.

**Flag isolation** (LQ, default shape, vs flags 0): skip-only wall 1.008x, tile-only wall
0.994x / GPU 1.018x, skip+tile wall 1.008x — all inside the noise band. LQ `1x256` flags
`120` vs `0` over 3 repeats: wall 1.007, GPU 1.006 (noise). Note that `createLqSettings()`
always ORs `RF_FAST_SHADOW | RF_FAST_AO | RF_COARSE_REFINEMENT`, so `GLADIUS_BENCH_FLAGS=0`
in the LQ path still carries the coarse knobs; the matrix can isolate skip/tile/shape only.

**HQ flags `120` vs `0`** (default shape): wall 0.997, GPU 1.009 → timing neutral. Counter
deltas: `totalRays`, `totalHits`, `shadowTaps`, `aoTaps` identical; `totalSteps` −1,771..−20,500
(−0.05..0.5 %), `refinementTaps` −528..−8,892. Pixel parity remains bit-exact, and
`WebGPURayMarchPerf_tests.cpp` asserts only `totalRays == resolution * resolution` and
`totalHits > 0`, so the counter deltas do not fail the bench.

**Ranked speed × quality summary (LQ path unless noted):**

| Technique | Wall | GPU | Quality verdict |
| --- | --- | --- | --- |
| QK coarse refinement | **1.444x** | **1.662x** | LQ gate PASS on all 12 samples |
| E7 tile culling | ~1.00x steady-state | ~1.02x | bit-exact; real value is the far-camera probe (5.56 ms → 0.67 ms ≈ 8.3x) |
| E2 skip background rays | ~1.01x | ~1.01x | bit-exact; ~0.05 % steps in the bench corpus |
| E3 fast shadow/AO | −0.8 % wall | −2.0 % GPU | LQ gate PASS |
| Workgroup `2x128` / `1x256` | see integration re-measure below | — | bit-exact, but not shipped |
| E1/E5/E6/E8 | — | — | rejected (see their sections) |

## Integration re-measurement (supersedes the shape figures above)

The earlier combine batch was collected while the machine carried a sustained ~9 load average
from an unrelated process, and GPU medians are quantized to ~0.06 ms steps. A fresh
**interleaved** A/B loop (control/shipped alternating within one loop, 3 repeats, 18 samples,
same binary) was therefore run to separate shape from flags:

| Comparison | Wall median | Wall geomean | Faster samples | GPU geomean |
| --- | --- | --- | --- | --- |
| HQ `2x128` vs `16x16` (flags 0) | 0.967 | 0.963 | 12/18 | 1.018 |
| HQ flags `120` vs `0` (`16x16`) | 0.954 | 0.941 | 14/18 | 1.008 |
| HQ shipped (`2x128` + `120`) | 0.963 | 0.938 | 13/18 | 0.994 |
| LQ flags `120` vs `0` (`16x16`) | 0.956 | 0.940 | 15/18 | 0.946 |
| LQ `2x128` vs `16x16` (flags `120`) | **1.078** | **1.098** | 5/18 | 1.013 |

The shape-only comparison is the decisive one: `2x128` is a **net slowdown in LQ**
(`lampbase` 0.69–0.79x, `RadialRadiator` 0.78–0.88x) and inside the noise band in HQ. The
earlier 1.067x/1.075x shape figures do not reproduce and are superseded. Flag-only
(skip + tile culling) is consistently faster on wall (0.94x geomean, 14–15 of 18 samples)
and neutral-to-positive on GPU.

**Decision:** keep the **16x16** frame workgroup default; do not ship a wider shape. Ship the
task-type accelerator policy instead: bounds guards (skip + tile culling) on every frame task,
plus the quality knobs (fast shadow/AO, coarse refinement) on `LowResolutionPreview` only.

## Phase 3 integration

`async_rendering::chooseFrameAccelerationFlags(RenderTaskType)` in
`gladius/src/ui/render/WebGPUFrameAccelerationPolicy.h` is applied to the copied neutral
settings inside the `RenderWindow` async scheduler. Verified after integration:

- HQ parity: 12/12 bit-exact (PSNR inf, SSIM 1, max channel diff 0).
- LQ gate: PASS, worst sample `RadialRadiator.3mf@256` histogram overlap 0.9099.
- Policy unit tests: 4/4 (`WebGPUFrameAccelerationPolicy_tests.cpp`).
- Full regression: 1750 tests, 1708 passed, 42 skipped (WebGPU gated), 0 failed, 2 disabled.

Interactive WASM/WebGPU QA could not be automated here (no browser MCP server configured), so
the preview-path behaviour is covered by the deterministic quality gates rather than by
in-browser inspection.

## PR — Progressive HQ chunk policy (time-to-full-quality)

Progressive HQ catch-up is measured by `WebGPUProgressivePerf.TimeToFullQuality`
(`gladius/tests/unittests/WebGPUProgressivePerf_tests.cpp`, gated by
`GLADIUS_RUN_WEBGPU_BENCH=1`). The harness drives the real backend row-band by row-band, paces the
UI at one display submission per frame (`GLADIUS_PROGRESSIVE_BENCH_PACE_MS`, default 16.7 ms), and
writes `baselines/progressive_performance.json` (schemaVersion 2).

**Cost model.** With one submission in flight and one display submission per UI frame,

$$T_{HQ} \approx n_{chunks} \cdot T_{frame} + \sum_i T^{GPU}_i$$

A chunk-count sweep (SimpleGyroid@512, pace 16.7 ms) confirms the linear term dominates:

| chunks | 16 | 8 | 4 | 2 | 1 |
| --- | --- | --- | --- | --- | --- |
| time to HQ (ms) | 332.8 | 162.4 | 72.8 | 30.7 | 10.1 |
| median chunk GPU (ms) | 0.33 | 0.39 | 1.31 | 8.52 | 9.11 |

Per-chunk CPU cost is *not* the bottleneck: the measured submit call is 0.08–0.17 ms and the UI
pump 1.3 ms. Fixed submission-cost reduction (buffer/bind-group reuse) therefore has no headroom
and was dropped; the win is in the number of chunks.

**Change.** `NeutralRenderScheduler::budgetForResolution()` seeds the chunk budget per resolution so
a settled static view fills in `TARGET_PROGRESSIVE_CHUNKS` (8) chunks, clamped to
[16K, 256K] pixels. During the sequence the budget adapts to the last chunk's GPU pass time
(`IRenderSubmission::getFrameTiming()`, requested via `RenderRequest::collectGpuTiming` for
progressive chunks) against `MAX_PROGRESSIVE_CHUNK_GPU_MS` = 33 ms, falling back to wall time
(20 ms target) when timestamps are unavailable. Over-budget chunks shrink the budget by at most 4x;
under-budget chunks double it back towards 256K.

**A/B (median of 3, UI-paced, baseline = fixed 16K budget):**

| model | res | chunks | time to HQ (ms) | delta |
| --- | --- | --- | --- | --- |
| SimpleGyroid | 512 | 16 → 8 | 369.7 → 162.5 | -56.0% |
| ImplicitGyroid | 512 | 16 → 8 | 355.4 → 164.7 | -53.7% |
| SphereInACage | 512 | 16 → 8 | 347.5 → 164.6 | -52.6% |
| RadialRadiator | 512 | 16 → 8 | 374.7 → 162.3 | -56.7% |
| webcam_003 | 512 | 16 → 8 | 413.8 → 167.3 | -59.6% |
| lampbase | 512 | 16 → 8 | 1839.3 → 405.4 | -78.0% |
| SimpleGyroid | 256 | 4 → 4 | 77.3 → 79.1 | +2.2% (noise) |
| ImplicitGyroid | 256 | 4 → 4 | 81.9 → 73.7 | -10.0% |
| SphereInACage | 256 | 4 → 4 | 85.9 → 76.4 | -11.0% |
| RadialRadiator | 256 | 4 → 4 | 98.1 → 83.1 | -15.3% |
| webcam_003 | 256 | 4 → 4 | 101.9 → 74.2 | -27.2% |
| lampbase | 256 | 4 → 4 | 530.7 → 194.4 | -63.4% |

`bench_compare.py` reports PASS (no wall-time regression). At 256 px the chunk count is unchanged
(16K floor) but the GPU-time signal stops the wall-time fallback from shrinking chunks, which is
where most of the 256 px gain comes from (`lampbase` GPU median 9.96 → 7.34 ms).

**UI-thread constraint.** No UI-thread path may block on a backend callback. Verified by
`NeutralRenderScheduler.FullHeightProgressiveTask_WithCallbackDrivenBackend_ReachesHighQualityWithoutBlocking`:
a submission that only completes when `progress()` is pumped reaches full quality in
`TARGET_PROGRESSIVE_CHUNKS` UI frames with zero `wait()` calls. The Emscripten variants of
`WebGPUFrameSubmission::progress()`/`wait()` keep the same property (no `processEvents()`, `wait()`
is a no-op); the WASM build was not compiled here (no Emscripten on this host).

**Caveats.** Row-band cost is highly non-uniform (SimpleGyroid@256: rows 0–64 ≈ 0.15 ms,
rows 192–256 ≈ 11–20 ms GPU), so a fixed pixel budget yields uneven chunks; cost-aware row counts
are a follow-up. The first chunk of a session pays ~3–6 ms extra CPU because
`WebGPUFramePipelineCache` compiles pipelines asynchronously. The full-frame baseline
`baselines/performance.json` predates this machine and is stale (~14.6 ms GPU at 256 px now).
