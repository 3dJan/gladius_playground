c# WebGPU Ray-March Performance — Running Findings

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

## E4 — macrocell grid prepass (in progress)

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
- E7 tile culling: build on E2's `ray_misses_bounds()` at tile granularity — biggest potential for
  background-heavy views.
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
