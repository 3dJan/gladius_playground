# WebGPU Ray-March Benchmark

Headless benchmark for the WebGPU ray-march frame path (`src/webgpu/WebGPUComputeBackend.cpp`).
It measures end-to-end latency of a full HQ frame render plus GPU-internal compute-pass
duration and shader ray-march counters.

## Running the benchmark

The benchmark is a gated gtest (`WebGPURayMarchPerf.RunBenchmarkCorpus`) that runs without a UI:

```bash
cmake --build --preset linux-webgpu-releaseWithDebug --target gladius_test --parallel 8
cd out/build/linux-webgpu-releaseWithDebug/tests/unittests
GLADIUS_RUN_WEBGPU_TESTS=1 GLADIUS_RUN_WEBGPU_BENCH=1 \
  GLADIUS_BENCH_OUTPUT=webgpu_bench/baseline.json \
  ./gladius_test --gtest_filter='WebGPURayMarchPerf*'
```

Environment variables:

| Variable | Effect |
| --- | --- |
| `GLADIUS_RUN_WEBGPU_BENCH=1` | Enables the benchmark test (otherwise it skips). |
| `GLADIUS_BENCH_MODEL` | Restricts the corpus to one model file (smoke runs). |
| `GLADIUS_BENCH_RESOLUTION` | Restricts the corpus to one resolution. |
| `GLADIUS_BENCH_OUTPUT` | JSON output path (default `webgpu_bench/baseline.json`). |

The scene setup mirrors the authoritative app render path: document load,
`updateFlatAssemblyParameterValues()`, runtime flat-scene materialization under a
`RenderBackendSession`, and `ui::OrbitalCamera` framing derived from model bounds
(same as `WebGPUThumbnailRenderer`). Render settings are the app's neutral HQ settings.

## Report schema (`schemaVersion: 1`)

Each sample is one `(model, resolution)` configuration:

| Key | Meaning |
| --- | --- |
| `medianWallMs` | **Primary metric.** CPU wall time around `renderFrame()` including readback; scales with resolution. |
| `p5WallMs`, `p95WallMs`, `minWallMs`, `maxWallMs` | Wall-time distribution over the iterations. |
| `medianWorkDoneMs` | Queue submission → `OnSubmittedWorkDone`. CPU-side queue latency only; Dawn/radv does not reflect GPU completion here. |
| `medianGpuMs` | Compute-pass duration from Dawn timestamp queries (`FrameRequest::collectGpuTiming`). Requires the `timestamp-query` feature; zero when unavailable (e.g. WASM). |
| `totalRays`, `totalSteps`, `totalHits`, `nonConverged`, `refinementTaps`, `shadowTaps`, `aoTaps`, `backtracks`, `interiorSteps` | Shader counters from `RF_DEBUG_METRICS` (layout mirrors the OpenCL `RayMarchMetrics`). |
| `stepsPerRay`, `hitsPerRay` | Derived ratios used to attribute speed changes to workload reduction vs. shader efficiency. |

## Regression gate

```bash
python3 scripts/bench_compare.py baseline.json current.json --tolerance 0.10
```

Compares `medianWallMs` per `(model, resolution)`; fails (exit 1) when any sample is
more than 10% slower than baseline, or when the sample sets differ (exit 2).

## Baseline

The Phase 0 baseline is stored in `specs/031-webgpu-raymarch-performance/baselines/performance.json`.
Re-measure on the same hardware before comparing; GPU timings are hardware-dependent.

## Known limitations

- `radv` prints a non-conformance warning; timings are valid for relative comparison on this machine.
- GPU timestamps are wired only in the native path; the Emscripten/WASM build reports `gpuTimestampsAvailable = false`.
- First frames of a model include pipeline/materialization warm-up; medians over repeated iterations exclude most of it, but `maxWallMs` can show outliers.
