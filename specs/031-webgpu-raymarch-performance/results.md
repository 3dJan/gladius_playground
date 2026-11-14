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
