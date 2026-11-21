# Implicit Modeling Guide — Cost, Structure, Anti-patterns

Read this before authoring or refactoring any non-trivial Gladius model, and
whenever a model renders slowly. All cost claims below are for the **WebGPU**
path (`src/webgpu/shaders/frame_compute.wgsl`), which is the only render path on
Apple Silicon builds.

---

## 1. How the graph becomes shader code

Understanding the pipeline is what makes the rest of this guide predictable.

| Stage | Source | Effect |
|-------|--------|--------|
| `LowerFunctionGradient`, `LowerNormalizeDistanceField`, `OptimizeOutputs` | `src/Document.cpp:837-848` | Feature lowering + dead-output pruning |
| `GraphFlattener::flatten()` | `src/Document.cpp:850-853`, `src/nodes/GraphFlattener.cpp` | **Every used function is inlined into one flat model** |
| `ToWgslVisitor` | `src/nodes/ToWgslVisitor.cpp` | Emits WGSL; single-use outputs are folded into one expression |

Two consequences that trip agents up:

1. **Splitting a model into functions does not make rendering faster.**
   `GraphFlattener` walks `main` and clones the body of every called function
   into the caller. The dedup key is the *call site*
   (`GraphFlattener.cpp:55-61`, `m_integratedFunctionCalls.find(&functionCall)`),
   so `tree_12(p1)`, `tree_12(p2)`, `tree_12(p3)` each get their own copy of
   `tree`'s nodes. Refactoring a monolith into ten functions produces the
   identical flat graph and identical frame time.
2. **There is no per-subobject early-out.** The flattened graph is one
   straight-line expression. `min(a, b)` unions do not skip `b`; every branch is
   evaluated at every sample. `OutputPortReferenceAnalyzer::shouldInline()`
   (`src/nodes/OutputPortReferenceAnalyzer.h:56`) inlines any output referenced
   exactly once, which is why a flat union in `main` becomes one enormous
   emitted expression.

So the two goals are separate:

- **Modularity** buys maintainability, reuse, reviewability, and library growth.
- **Render cost** is determined *only* by the total node count reachable from
  `main` after flattening, times the number of times the evaluator is called.

Do both. Never claim a speedup from a refactor alone.

---

## 2. WebGPU cost model

The evaluator is called once per ray-march step and once per shading tap. Tap
counts from `src/webgpu/shaders/frame_compute.wgsl`:

| Cost site | Line | Taps (default) | Taps (preview/coarse) |
|-----------|------|----------------|-----------------------|
| `estimate_normal()` | 458-468 | 4 | 4 |
| `surface_refinement_taps()` | 459 | 6 | 4 (`RF_COARSE_REFINEMENT`) |
| `calc_soft_shadow()` | 474-503 | ≤32, called **twice** (light + reflection, :554/:555) | 0 (`RF_DISABLE_SHADOWS`) |
| `calc_ambient_occlusion()` | 506-523 | 8 | 0 (`RF_DISABLE_AO`) |
| color / translucency | 529+ | 1 | 1 |

**Per hit pixel: ≈ 83 full-graph evaluations at full quality, ≈ 13 in preview.**

Marching adds more. `close_enough = 0.001 + traveled * 1e-6` (:662-784), one
backtrack allowed, `max_ray_steps` up to `MAX_FRAME_RAY_STEPS = 2048`
(`src/webgpu/WebGPUComputeBackend.cpp:1176-1179`), capped to 300 in preview.

**Rule of thumb:** `frame cost ≈ flat_node_count × (march_steps + 83)` per hit
pixel. Halving the flat node count roughly halves frame time.

### Accelerators that are already on

`src/ui/render/WebGPUFrameAccelerationPolicy.h:24-46` enables
`RF_SKIP_BACKGROUND_RAYS | RF_TILE_CULLING | RF_USE_PRECOMPUTED_SDF` for all
frame task kinds. Preview additionally sets `RF_DISABLE_SHADOWS |
RF_DISABLE_AO | RF_COARSE_REFINEMENT`.

The precomputed SDF grid (`src/ui/RenderWindowWebGPU.cpp:782-789`) is `64³` or
`128³` over the model bounds padded by `max(span)*0.05 + 2.0`
(`WebGPUComputeBackend.cpp:1530-1580`), with `tolerance = maxVoxelSize * 2.5`.
`evaluate_model_cached()` (`frame_compute.wgsl:366-385`) short-circuits outside
that box and when `|cached| > tolerance`. **Near the surface the full analytic
graph runs anyway** — so the accelerators help far-field steps only. Your op
count still dominates.

Non-frame task kinds (including `SdfPrecomputation`) get **no** accelerators.

---

## 3. Measuring graph cost

`get_program_snippet()` gives you the whole program. Count nodes per function,
then compute the *inlined* total for `main` — that is the number that matters.

The catch when parsing a saved `.3mf`: a `<i:functioncall>`'s `functionID` input
is a `<i:resourceref ref="Resource_N.value">` pointing at a sibling
`<i:constresourceid identifier="Resource_N" value="fnId">`. Resolve that
indirection and do **not** skip `constresourceid` nodes, or every call count
comes out as zero. Namespace:
`http://schemas.3mf.io/3dmanufacturing/implicit/2023/12`.

Analyzer (save as `graph_cost.py`, run `python3 graph_cost.py model.3mf`). It
prints, per function: own op count, inlined total, call count, and `main`'s
direct calls.

```python
import xml.etree.ElementTree as ET, sys
I = '{http://schemas.3mf.io/3dmanufacturing/implicit/2023/12}'
root = ET.parse(sys.argv[1]).getroot()
funcs = {int(f.get('id')): {'name': f.get('displayname'), 'body': f}
         for f in root.iter(I + 'implicitfunction')}
SKIP = {'implicitfunction', 'in', 'out', 'scalar', 'vector', 'scalarref',
        'vectorref', 'resourceid', 'constresourceid',
        'implicitfunctioninputs', 'implicitfunctionoutputs'}
own, calls = {}, {}
for fid, f in funcs.items():
    o, cl = 0, []
    for node in f['body'].iter():
        if node.tag.split('}')[1] in SKIP:
            continue
        if node.get('identifier') == 'functionID':
            cl.append(node.get('ref'))      # call site, needs resolving
            continue
        o += 1
    own[fid], calls[fid] = o, cl

def resolve(fid, ref):
    key = ref.split('.')[0]                 # "Resource_7.value" -> "Resource_7"
    for cr in funcs[fid]['body'].iter(I + 'constresourceid'):
        if cr.get('identifier') == key:
            v = cr.find(I + 'value')
            if v is not None:
                return int(v.get('value'))
            if cr.get('value'):
                return int(cr.get('value'))

memo = {}
def total(fid, d=0):
    if fid in memo:
        return memo[fid]
    t = own[fid]
    for r in calls[fid]:
        c = resolve(fid, r)
        if c in funcs and d < 25:
            t += total(c, d + 1)            # each call site is cloned
    memo[fid] = t
    return t

for fid in sorted(funcs, key=lambda k: -total(k)):
    print(f'{fid:3d}  {funcs[fid]["name"]:<22} own={own[fid]:4d} '
          f'inlined={total(fid):5d} calls={len(calls[fid]):3d}')
main = next(k for k in funcs if funcs[k]['name'] == 'main')
print('main inlined total =', total(main))
print('main direct calls:',
      [(funcs[resolve(main, r)]['name'], total(resolve(main, r)))
       for r in calls[main] if resolve(main, r) in funcs])
```

Also useful: `GraphFlattener::expectedNodeCount()` is exposed in the C++ API and
gives the post-flatten count directly.

### Budgets

| Flat node count in `main` | Verdict |
|---------------------------|---------|
| < 150 | comfortable at full quality |
| 150 – 400 | acceptable; preview stays responsive |
| 400 – 800 | sluggish; shadows/AO dominate |
| > 800 | interactive preview degrades badly |

---

## 4. Case study: `tea_light_house_v2` vs `v3`

Both models render the same scene. `v2` is responsive, `v3` is not.

| | v2 | v3 |
|---|---|---|
| Functions | 13 | 27 |
| `main` own ops | 6 | 46 |
| **`main` inlined total** | **347** | **1006** |
| Direct calls from `main` | 2 | 22 |

`v3`'s `main` inlines: `groundPlate` 45, `snowDrifts` 65, `pathSlab` 102,
`hedge` 81, `snowman` 65, `treeA`/`treeB`/`treeC` 20 each, `houseWalls` 186,
`snowRoof` 57, `windowSkin` 53, **ten separate `pick` calls at 10 ops each**,
`houseCuts` 121, `floorHole` 35.

The house itself is *not* the problem — `houseWalls` is 199 ops in v2 and 186 in
v3. The 2.9× blow-up is entirely scene content that v2 never had, gathered flat
into `main`.

Note that v3 *did* use functions. It just called them all from one place, so
flattening produced a 1006-node monolith anyway. The fix is not "more
functions" — it is fewer evaluated shapes and cheaper shapes.

---

## 5. Anti-pattern catalog

### 5.1 Flat scene union in `main`

```glsl
// BAD: 22 branches, all evaluated at every sample
shape = min(min(min(min(min(min(min(min(min(min(
    groundPlate_20(p), snowDrifts_21(p), pathSlab_22(p), hedge_23(p),
    snowman_24(p), treeA_25(p), treeB_26(p), treeC_27(p), house_28(p),
    snowRoof_29(p)), pick_30(p)), pick_31(p)), ...
```

**Fix:** cull by space, not by structure. Group objects that occupy disjoint
regions and gate them with a cheap bounding SDF so the expensive branch is only
reached where it can matter:

```glsl
float houseRegion = length(p - vec3(0, 0, 40)) - 60.0;
float treesRegion = length(p - vec3(30, 10, 20)) - 25.0;
shape = min(houseRegion, treesRegion);   // cheap gate, still one min
```

This does not skip evaluation, but a cheap gate *does* give the ray-marcher a
useful distance estimate, which is where the step-count win comes from.

### 5.2 Copy-paste variants of the same shape

`treeA`, `treeB`, `treeC` in v3 are three near-identical 20-op functions.

**Fix:** one `tree(pos, height, trunk_r, spread)` function called three times
with different arguments. Same node count, but reviewable, editable in one
place, and exportable to the library.

### 5.3 Ten hand-placed duplicates

Ten `pick` calls at 10 ops each = 100 ops for ten identical objects.

**Fix:** import `operations/polar_repetition` for radial arrays, or
`operations/symmetryX|Y|Z` for mirrored arrays. For free-form scatter, one
`pick(pos, offset)` called per instance is unavoidable — but then reduce the
per-instance cost instead.

### 5.4 `sin`/`cos` displacement on a large surface

`hedge` (81 ops) and `groundPlate` (45 ops) both displace their surface with
`sin`/`cos` ripples. High-frequency or large-amplitude displacement breaks the
Lipschitz bound the sphere tracer assumes: steps become conservative, backtracks
increase (metric 7), and refinement taps climb to the cap.

**Fix:** keep displacement low-frequency and small-amplitude, and count the
derivative: for `A * sin(w * x)` the slope is `A * w`. Keep `A * w < 0.3` or so.
Prefer a library lattice (`lattices/gyroid`, `honeycomb`) over hand-rolled
trigonometric surface noise.

### 5.5 `mod` / `floor` repetition

`pathSlab` (102 ops) uses modular repetition. `mod`/`fmod` produce discontinuous
distance fields, which forces tiny steps at every discontinuity.

**Fix:** use `operations/polar_repetition` or explicit domain folding
(`p = p - round(p / period) * period`) and make sure the SDF is evaluated in the
folded domain, so the field stays continuous.

### 5.6 Volumetric noise / image functions in the shape channel

`create_image3d_function` and noise-style fields are expensive per sample and
usually unnecessary. Use them for color or a custom volumetric property
(`create_volumetric_color`, `create_volumetric_property`) rather than for
`shape`.

### 5.7 Deep `min`/`max` chains instead of library CSG

Raw `min`/`max` are cheap; hand-rolled smooth blends are not. Import
`csg/smooth_union` or `csg/smooth_union_quadratic` instead of writing a
polynomial blend inline.

---

## 6. Domain meshes and bounding boxes

A domain mesh with `meshbboxonly="true"` is **free**. `Importer3mf` collects
those ids (`collectBboxOnlyMeshIds`) and `loadMeshes()` skips them entirely —
the mesh is never loaded as geometry. Its AABB is baked into the graph as a
single `BoxMinMax` op by `Builder::addLevelSetWithDomain`.

Do **not** shrink a bbox-only domain mesh to "improve performance"; it has no
effect. Do not blame it for a slow render. The precomputed-SDF grid is sized to
the numerically probed **model** bounds, not the domain box.

The one real constraint: the domain must contain the whole model, or geometry
gets clipped.

---

## 7. Performance debugging order

1. `get_program_snippet()` → count flat nodes reachable from `main`. If it is
   high, that is the answer. Fix structure before anything else.
2. Render at preview quality and at full quality. If the gap is large, shadows
   and AO dominate (`2 × ≤32` shadow taps + 8 AO taps per hit pixel) — reduce
   *surface area and detail*, not shape count.
3. If preview is *also* slow, the marcher is struggling: look for `mod`,
   `floor`, high-frequency `sin`, and discontinuous blends.
4. Only then consider camera/step settings.

Do not chase the OpenCL path (`src/kernel/rendering.cl`) on Apple Silicon
builds — it is not compiled in. (For the record, `calcAmbientOcclusion` at
`rendering.cl:523` mutates `renderingSettings.approximation` and never restores
it; that bug is OpenCL-only and irrelevant here.)
