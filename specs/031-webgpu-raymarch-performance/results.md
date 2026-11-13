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
