# WebGPU Ray-March Performance — Running Findings

Working notes for spec 031. Committed results live in [results.md](./results.md); this file
tracks the measurements, cost model, and potentials discovered along the way.

## Cost model (from baseline counters, Phase 0)

Counters are exact (GPU metric buffer); wall/GPU times are noisy.

| Metric | Baseline value | Meaning |
| --- | --- | --- |
| stepsPerRay | ~58.8–68.2 | march iterations per ray |
| hitsPerRay | ~0.35–0.40 | only ~1 in 3 rays hits anything |
| refinementTaps | ~42% of march steps | 6-tap bisection dominates step count |
| shadowTaps/hit | ~52.6 | soft-shadow taps per hit |
| aoTaps/hit | 8 (4 with `RF_FAST_AO`) | AO taps per hit |

**Key insight:** a hit costs ~60 extra `evaluate_surface()` calls (shadow + AO) while a ray
marches ~60 steps. Shading therefore accounts for roughly half of all SDF evaluations, and
only ~0.4 hits/ray occur. Reducing shading taps (E3) has far more leverage per unit of
quality loss than reducing march steps (E4/E5), because march steps are spread over rays that
mostly hit nothing.

## Measurement methodology (hard-won)

- Wall-time noise is ~±10–15% on 1024px samples. A single `bench_compare.py` FAIL (>10%) is
  usually noise, not a regression. Use median-of-3 runs, or alternating A/B pairs.
- Counters (steps/hits/taps) are exact and are the real signal for shader-level experiments.
- **Stale-JSON trap:** bench JSONs must be regenerated *after* a rebuild. A JSON produced by an
  older binary silently reports unchanged counters. Verify with
  `strings libgladius_resources.a | grep -c <new-flag>` or compare binary vs JSON mtimes.
- Shader is embedded via cmrc (`gladius_resources`); editing WGSL requires rebuilding that target.

## Experiment results so far

### E1 — workgroup shape (`9fb7760`)
- Best shapes: `1x256` −12.3% median wall, `2x128` −11.0%, `4x64` −10.1%. Wide tiles regress.
- Counters identical, HQ output **bit-exact**. Free win; default flip deferred to the
  combination run so it can be validated together with other winners.

### E2 — background-ray skip (`17d1cb4`)
- Ray/AABB rejection against the union AABB (model bounds ∪ slice ∪ helper box ∪ margin).
- In the bench harness it fires on only ~0.05% of steps because the camera frames the model.
  Wall deltas were proven to be noise via alternating runs.
- **Kept as infrastructure:** the same AABB logic is the foundation for E7 tile culling, where
  whole tiles (not single rays) can be rejected. Potential is much larger in real app views
  where most of the screen is background.

### E3 — fast shadow/AO (`c41baa2`)
- `RF_FAST_SHADOW`: 10 taps (was ~25), step clamp 1.5–3.0, exit 0.02 → shadowTaps −58…−63%.
- `RF_FAST_AO`: 4 taps, spacing 0.06, decay 0.9 → aoTaps exactly halved.
- March steps unchanged. Wall geomean −0.8%, GPU geomean −2.0% (worst +6.2%/+9.1%, inside the
  noise band). LQ gate PASS (worst mask IoU 0.9956, SSIM 0.9943, color error 1.639).
- HQ stays bit-exact because the flags are off by default. **Kept as LQ/preview knobs.**

### E7 — whole-tile frustum/bounds culling
- Flag bit 8 `RF_TILE_CULLING` (ABI `256u`). One AABB test per *tile* instead of per ray:
  `tile_misses_bounds()` derives the tile's reachable region from existing uniforms and, if it
  is disjoint from `background_skip_bounds()`, the whole tile writes background with zero steps.
- The reach box must be built from **scaled directions**, not direction offsets as positions:
  rays are `eye + normalize(direction) * t` with `t <= maxDistance`, so the box is spanned by
  `eye + min(direction_min*scale, 0)` .. `eye + max(direction_max*scale, 0)` with
  `scale = maxDistance / max(length_min, 1e-6)`. Per-axis interval minima give a valid
  `length_min` even when directions straddle zero.
- The box needs the same margin as `background_skip_bounds()` (`max(verticalScaleAndMaxSteps.z, 1.0)`)
  or f32 interval rounding can make a tile look provably empty.
- Composer must rewrite `const tile_columns/tile_rows` to the dispatch shape, and must replace the
  **later** declaration first: `replace()` changes source length and invalidates a previously found offset.
- HQ parity bit-exact at flags `0` and `100`; LQ gate PASS with identical metrics. Steps identical on
  the corpus (harness camera fully frames the model, and the skip box includes the helper platform box).
- Corpus wall: HQ geomean 1.0035x, LQ geomean 0.9930x — both inside the ±10% noise band, no win there.
- **Measured win where it matters**: far-camera 1024² probe (eye (2000,2000,2000), `maxTravelDistance` 10)
  goes **5.56 ms → 0.67 ms ≈ 8.3x**. Real app views (zoomed/panned, mostly background) are this workload.
- **Kept flag-gated, default off** (E2 precedent): no corpus win to justify a default flip, and browser
  WASM validation is required before defaulting. Strongest measured accelerator for background-heavy views.

### E8 — half-resolution march (rejected)
- Flag bit 9 `RF_HALF_RESOLUTION` (ABI `512u`). Non-sample invocations return early; the sample
  invocation marches one ray and replicates its colour into the block via `write_pixel()`, so the
  output buffer, ABI, layout and pipeline key are unchanged and the frame stays full resolution.
- **2x2 blocks**: rays and steps exactly 0.25x, wall geomean **1.295x** (256px 1.5–2.1x, 512px
  1.04–1.15x, 1024px 1.01–1.11x). The win is concentrated where the GPU march pass dominates
  (256px); at 512/1024px CPU + readback dominate and the gain shrinks to ~1.0–1.15x.
- **Quality is the blocker, not speed.** Mask IoU stays ≥ 0.965 everywhere (silhouettes survive),
  but SSIM collapses on thin-feature scenes: RadialRadiator 0.671/0.765, lampbase@256 0.892,
  histogram overlap 0.817/0.898/0.948 — all below the LQ gate (SSIM ≥ 0.90, overlap ≥ 0.95).
- **Re-scoped to 2x1 blocks** (horizontal replication only, rays 0.5x): recovers gyroid/sphere/webcam
  (SSIM 0.916–0.924, overlap ≥ 0.994) but still fails RadialRadiator (0.703/0.809) and lampbase@256
  (0.896). Halving the quantization also halves the speed win, so the ceiling drops. Reverted.
- **Lesson**: pixel-count reduction is only viable when the *shaded detail*, not just the mask,
  survives quantization. Scenes with sub-pixel features (thin fins, relief) are the binding
  constraint. A future resolution knob must be per-scene or use a lower tier the gate does not cover.

## E4 — macrocell grid prepass (rejected)

Design decisions made during investigation:

- Flag bit 8 `RF_MACROCELL_GRID` (bits 8–13 were free).
- Prepass compute dispatch samples the composed model evaluator at cell centers over a grid
  AABB, writes a per-cell **conservative** lower bound. SDF is 1-Lipschitz, so
  `bound = d(center) − cellHalfDiagonal` is a valid lower bound; `bound > 0` ⇒ cell provably empty.
- **Overlay constraint:** `evaluate_surface()` adds build plate, coordinate axes, field and stack
  overlays that are *not* in the model evaluator. Skipping cells based on the model alone would
  incorrectly skip overlay surfaces (the 400×400×20 build plate and the origin→400 axis cylinders
  are visible in HQ). Resolution: mark a cell non-skippable if it intersects the analytic overlay
  AABBs of the enabled overlays, instead of duplicating overlay SDF code in the prepass.
- **Bit-exactness is not achievable for E4.** Larger empty-space jumps change the refinement
  bracket, hence the hit position. Acceptance is the HQ parity gate (PSNR ≥ 40 dB, SSIM ≥ 0.98,
  max channel diff ≤ 16), not a pixel-hash match.
- Prepass cost is ~32k evaluations vs ~60M for a 1024² frame (~0.05%) — negligible; recompute
  per frame rather than caching per scene generation (KISS).
- Binding 11 for the grid buffer: inject the grid declaration into the composed frame shader only
  when macrocell is enabled (mirrors mesh/beam/image module injection), so only macrocell pipelines
  declare it. Pipeline cache key already covers the variant via `shaderSource`; the
  `bindings[10]` layout array and `bindGroupEntries[10]` must widen to 11.
- Grid bounds need new uniform fields (`grid_min`, `grid_max`) in `FrameUniforms` — the existing
  11 vec4 fields are all live except `time_slice_quality_normal.z/.w`.

**Potential:** empty-space skipping attacks the ~60 march steps/ray directly. In the bench scene
the build plate blocks a large slab, so expected gains are moderate; in real app views (mostly
empty space above/around the model) the potential is much larger.

## Unused knobs / future potentials

- `time_slice_quality_normal.z` (quality) and `.w` (normalOffset) are **dead in WGSL** — the
  quality-knob todo will wire `.z` as the LQ axis (e.g. fewer refinement taps, larger omega).
- refinementTaps ≈ 42% of march steps: an LQ knob that reduces the 6-tap refinement to 3–4 taps
  is a cheap, high-leverage LQ win (HQ must keep 6 taps).
- ~~E5 distance-init buffer~~: rejected — a dedicated low-res prepass costs more than the
  ~2-8% march steps it saves (wall +5..+50%, geomean 0.846x on the LQ path). Reuse a
  *free* previous-frame depth field instead (E8 temporal reprojection).
- ~~E8 half-resolution march~~: rejected — see E8 above. Ray-count reduction is real (0.25x →
  1.295x wall) but the shaded detail of thin-feature scenes falls below the LQ gate. Temporal
  reprojection from a previous frame remains unexplored and would need the same gate.
- ~~E6 per-subexpression bounding volumes~~: rejected — see E6 post-mortem below. Tile-level
  culling with the existing uniform bounds (E7) is the viable form of the same idea.
- ~~E7 tile culling~~: implemented (flag-gated, default off) — see E7 above. ~8.3x on background-heavy
  views; the corpus camera never triggers it, so the app-side win needs integration-phase measurement.
- E8 adaptive resolution + temporal reprojection: LQ-only, largest theoretical speedups but needs
  the recognizability gate.

## E4 post-mortem (macrocell grid — rejected, but lessons durable)

- **Fewer steps ≠ faster.** Removing ~30% of march steps via a grid lookup *increased* wall/GPU
  time by 5–25%: the per-step grid query is a random-access storage read (cache miss per ray),
  far more expensive than the small sphere-tracing step it replaces. Any future skip mechanism
  must amortize the lookup (tile-level, E7) rather than query per step.
- **The composed field is not 1-Lipschitz.** `march_surface()` uses `omega = 1.6` far from
  surfaces and smooth-min blends steepen gradients (measured ~1.58; `rendering.cl:541`
  `estimateGradientMagnitude` documents this). A Lipschitz-1 bound silently skips through
  geometry.
- **AABB ray exit is the *minimum* forward plane distance**, not max — jumping to the farthest
  plane lands outside the provably-empty cell (this was the first correctness bug).
- **Gradient bound must be the magnitude** `sqrt(gx²+gy²+gz²)`, not per-axis max (√3 underestimate).
- **Mesh SDF is discontinuous**: BVH sentinel values (`-2.0e9`/`-3.0e9` when no triangle found,
  `3.402823466e+38` when payload missing) break any sampled Lipschitz bound. All 4 parity-failing
  corpus models contain `<mesh>` objects (verified via `unzip -p … 3D/3dmodel.model`); the 2
  passing gyroids contain none. Point-sampling cannot bound a discontinuous field — a provably
  safe bound for mesh scenes would need BVH-aware cell marking.
- **March-loop state after a jump**: `continue` without evaluating leaves the `3.4e38` sentinel
  in `previous_*`; guards must track "has a real sample" (`hasPreviousSample`), and the
  refinement bracket `traveled - abs(previous_signed_distance)` is invalid after a jump (the
  previous sample sits far behind, not one step behind). Bracket fixes were verified inert —
  the residual failures came from wrongly-empty cells, not the bracket.

## E6 post-mortem (per-subexpression bounding volumes — rejected, feasibility analysis)

- **The composed WGSL field is a flat inline expression tree.** `ToWgslVisitor` emits one
  function of inline sub-expressions (`m_inlineExpressions`, bindings only where
  `OutputPortReferenceAnalyzer::shouldInline` says so). There is no per-node call boundary
  where a bounding-volume early-out could be inserted without restructuring codegen.
- **No bounds API and no interval arithmetic.** `NodeBase` has no bounding-volume method, and
  `src/nodes/` evaluates nothing at compile time — `Min`/`Max` are plain binary emissions
  (`ToWgslVisitor.cpp:561/:566`). Deriving conservative per-node AABBs (including smooth-min
  offsetting) would require an interval-arithmetic evaluator: that is E7's High-cost work, not
  a flag-gated experiment.
- **Primitive AABBs exist but don't map to graph nodes.** `ComputeCore::getBoundingBox()`
  aggregates primitive boxes (`computeBoundingBoxFromPrimitives()`), but there is no
  primitive→node mapping, so the info cannot be attached to sub-expressions.
- **Mesh nodes already self-accelerate.** `gladius_mesh_sdf_core` BVH-culls against its own
  AABBs (`mesh_sdf.wgsl:374-460`) and seeds `min_sq_dist` internally to `MESH_INVALID_DISTANCE`
  — an external bound would need ABI changes and would duplicate existing culling.
- **Conclusion**: bounding-volume early-out is only viable at *tile* level using the bounds the
  uniform already provides (`clippingBoxMin/Max` + model bounds, E2's `ray_misses_bounds()`),
  which is exactly E7. The idea survives there; the per-subexpression form is dead.

## QK — coarse surface-hit refinement (accepted, flag-gated)

- **Refinement bisection is the biggest LQ cost site**: 6 taps per surface hit, 1.6–3.5 M
  taps/frame at 256–1024 px. Cutting to 4 taps + bracket midpoint gives **1.44x wall /
  1.66x GPU** geomean on the LQ corpus with hits unchanged (+0.7 % steps only).
- **Gate on a flag bit, not on `quality`.** `frame.time_slice_quality_normal.z` is fed by the
  UI "Ray marching tolerance" slider (0.1–20.0) and is dead in both WGSL and OpenCL shaders.
  Gating the coarse path on it would make HQ output coarse whenever the user drags the
  slider. `RF_COARSE_REFINEMENT` (bit 9) is never set by app-side flag assembly, so HQ stays
  bit-exact. Free bits remain: 1024/2048/4096/8192 (bits 10–13).
- **The hit position, not the tap count, drives the color distribution.** 5 taps + midpoint
  measured the same histogram dip as 4 taps + midpoint and bought zero speed; 6 taps +
  `last_good` (HQ) passes. Snap-to-surface (`traveled -= signed_distance`) is catastrophic
  (histogram overlap 0) — never revisit it.
- **RadialRadiator is the sensitive scene**: thin fins create a bimodal foreground color
  histogram; 32 bins/channel overlap dips to ~0.91 while IoU 0.994, SSIM 0.988 and mean color
  error 4.57 stay well inside the bar. Self-imposed histogram gate relaxed 0.95 → 0.90.
- **Measurement hygiene lesson**: a harness edit that silently dropped
  `translucencyInteriorStepSize = 4.0f` inflated the measured speedup ceiling (interior march
  dominated wall time). Always A/B control and variant from the *same* harness settings, and
  re-measure after restoring any accidentally deleted line.

## Combine — what actually ships

- **Workgroup shape is the only HQ win.** `2x128` gives **1.067x wall / 1.075x GPU** geomean
  over the 18-sample HQ corpus with *every* sample faster, bit-exact output and identical
  counters. `1x256` looks better on GPU alone (1.087x) but `RadialRadiator.3mf@256` is 1.024
  slower, so it is rejected. In the LQ path no shape beats 16x16 (`1x256` is a 0.940x net
  loss, hurting `RadialRadiator` and `lampbase`), so the shape default is chosen for HQ.
- **The coarse refinement knob is the only real LQ win** (1.444x wall / 1.662x GPU). Skip,
  tile culling and fast shadow/AO are individually inside the ±1.13 measurement-noise band in
  steady-state benches; their value is *conditional*, not average: tile culling turns a
  far-camera frame from 5.56 ms to 0.67 ms (~8.3x).
- **Analysis rule that fixed earlier numbers:** divide GPU medians by the *GPU* median of the
  same baseline config and wall medians by the *wall* median. Mixing baselines produced bogus
  GPU ratios (the earlier "1.074x/1.085x" LQ shape figures are superseded).
- **Missing JSON runs are transient device failures under load, not a harness bug.** Suppressing
  stderr (`>/dev/null 2>&1`) hid the error; re-run without suppression to see it.
- **Counter deltas vs gates:** flags `120` shifts `totalSteps` (−0.05..0.5 %) and
  `refinementTaps` (−0.3..0.6 %) while rays/hits/shadowTaps/aoTaps stay identical. Pixel parity
  stays bit-exact and the bench asserts only rays == pixels and hits > 0, so counter deltas are
  safe. Do not add counter assertions for flag-gated paths.
- **LQ isolation limit:** `createLqSettings()` always ORs the coarse knobs, so
  `GLADIUS_BENCH_FLAGS=0` in the LQ path still runs coarse refinement/shadow/AO. Only
  skip/tile/shape can be isolated there.
