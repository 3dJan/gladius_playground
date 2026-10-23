# Translucency in the marching-ray renderers

Status: **both backends implemented - WebGPU validated with Tint, OpenCL validated by an
e2e pixel test. UI, MCP and thumbnail plumbing in place.**
3MF `<v:translucency>` serialization is **out of scope** - lib3mf 2.6.0 exposes no
Translucency / Translucent / VolumetricData API (verified: 0 hits in `lib3mf_abi.hpp` and
`lib3mf_implicit.hpp`). Authoring therefore happens through the implicit graph.

## 1. What translucency means here

The 3MF Volumetric Extension (`<v:translucency>`, spec v1.0.0 section 5.2.4) defines
translucency as a spatially varying scalar field whose value is the ISO 19307:2026
one-parameter "translucency alpha", clamped to its valid range. It is *not* alpha
coverage: alpha answers "how much of the background shows through this pixel", while
translucency answers "how much light survives after travelling through the solid itself".
A fully opaque silhouette can still be translucent - light entering the far side is
attenuated before it reaches the eye.

### Attenuation model

We reuse the same Beer-Lambert model already used for filament optics in
`src/io/3mf/FilamentOpticalProperties.h` (an `opacity` reached over a `referenceThickness`,
default 0.4 mm). Given the authored alpha `a` in [0,1] and a reference thickness `d0`:

$$ \sigma = \frac{-\ln(1 - a)}{d_0}, \qquad T(t) = e^{-\sigma t} $$

so a slab of thickness `d0` absorbs exactly the authored alpha. `a -> 1` is clamped to
0.9999 to keep `sigma` finite.

## 2. Authoring: a scalar graph channel

The assembly `End` node gains an optional scalar output channel named `translucency`
(`FieldNames::Translucency`). A model is "qualified for translucency" when its `Begin`
exposes a `pos` Float3 and its `End` exposes a Float `translucency` parameter
(`ModelUtils::isQualifiedForTranslucency`).

This is deliberately a *second* generated evaluator, never a widened shape/color signature.
The shape/color evaluator ABI stays `vec4 = (rgb, sdf)` in all three code generators
because the kernels contain ~141 `model(...)` call sites. `Builder::appendFunctionForTranslucencyOutput`
wires a function's scalar channel into the assembly, mirroring
`Builder::appendFunctionForColorOutput`.

## 3. Two evaluators per scene

`AnalyticRenderSceneSnapshotFactory` runs a second `ToWgslVisitor` pass in
`EvaluatorChannel::Translucency` mode over the already-flattened assembly, producing
`RenderSceneSnapshot::translucencyEvaluatorWgsl`:

```wgsl
fn evaluateTranslucency(position: vec3<f32>) -> f32 {
    return clamp(<expr>, 0.0f, 1.0f);
}
```

Parameter storage is shared: the factory takes the union of parameter ids used by either
pass and sizes the parameter buffer to the maximum. The WebGPU frame composer always emits
an `evaluateTranslucency` symbol - the real one, or an opaque `return 0.0f;` stub - so the
marcher can call it unconditionally with no `#ifdef`-style branching.

## 4. WebGPU marcher (`src/webgpu/shaders/frame_compute.wgsl`)

- `RF_ENABLE_TRANSLUCENCY = (1u << 20)` in `kernel/types.h`, mirrored as a WGSL const.
- `FrameUniforms` grows to 176 bytes with `translucency_params: vec4<f32>`:
  `x` = reference thickness (mm), `y` = interior step size, `z` = max interior steps,
  `w` = transmittance floor. The free `flags_mode_reserved.w` carries `maxSurfaces`
  (0 selects the shader default of 3).
- The sphere-trace loop is extracted into a restartable `march_surface(eye, dir,
  startDistance, maxDistance) -> MarchResult`, so a translucent ray can walk through
  several shells.
- `interior_transmittance` marches *through* the solid with fixed steps (sphere tracing
  degenerates inside a solid because every step lands back on the boundary), refining the
  far crossing by bisection and accumulating optical depth with the trapezoid rule.
- `main()` keeps a pixel-identical opaque fast path when `!translucency_enabled()`, and
  otherwise runs a front-to-back compositor: shade each surface, multiply by the
  transmittance accumulated through the solid before it, and stop once the remaining
  transmittance drops below the floor or the ray exits. Output alpha stays 1.0.
- Slice/cut-off mode and the low-resolution preview stay opaque.

## 5. OpenCL parity (done)

The OpenCL render path is already fed a flattened assembly
(`Document::updateFlatAssembly` -> `ComputeCore::refreshProgram(m_flatAssembly)`), so a
second `ToOclVisitor` pass emits `float translucencyModel(float3, PAYLOAD_ARGS)` as a
separate model source.

- `nodes/TranslucencyCodeGenerator` produces that source (or the zero stub) and reports
  `active`; `ProgramManager::setModelSources` keeps the optimized/preview pair.
- `PAYLOAD_ARGS` is never widened - the channel receives the payload it already expects.
- `kernel/types.h`: `RF_ENABLE_TRANSLUCENCY = (1u << 20)` and `RenderingSettings` grows to
  17 packed floats (reference thickness, interior step, max interior steps, transmittance
  floor, max surfaces), guarded by the `sizeof` static_assert.
- `kernel/translucency.cl` holds `attenuationCoefficient`, `interiorTransmittance` and
  `renderTranslucentRay`, and is linked only into `RenderProgram` (between `rendering.cl`
  and `renderer.cl`) - the slicer/contour/DC programs link `rendering.cl` without the entry
  point, so `rendering.cl` itself must stay free of compositor names.
- `renderer.cl` HQ kernels call `shadeRayResult`; the preview kernel keeps `determineColor`.
- `RendererCapability::TranslucentRendering` (1u << 12) is advertised by both renderers.

### Availability gating (both backends)

The zero stub means "no channel", and `sigma = 0` makes an opaque model render *fully
transparent* if the flag is honoured anyway. Availability is therefore published with the
source, not only kept in `ComputeCore`:

- `ResourceContext::isTranslucencyEntryPointActive()` (atomic) mirrors
  `ComputeCore::setTranslucencyAvailability()`, which is set at every source publication.
- `OpenCLRenderRequestFactory::create` and `WebGPUComputeRenderer::createFrameRequest` clear
  `RF_ENABLE_TRANSLUCENCY` when the flag is false, so a scene without the channel renders
  exactly as before even when the user switched the feature on.
- `AM_ONLY_PRECOMPSDF` dispatches stay opaque: the command-stream preview cannot express the
  channel, and `isTranslucencyEnabled()` rejects that approximation mode.

## 6. Controls, MCP and thumbnails

`compute/TranslucencyConfig` is the single source of truth for the five knobs (with the
clamps each one must respect) and offers `read`/`apply` overloads for both
`RenderingSettings` and `RenderSettingsSnapshot`.

- UI: `ui/RenderTranslucencyControls` (enable + thickness/step/surfaces/floor) is drawn from
  both menu bars (`RenderWindow.cpp`, `RenderWindowMenu.cpp`); `RenderWindow` reads and
  re-applies the active config, and `MainWindow` persists the enabled state.
- MCP `render_view`: `translucency` plus the four knob keys, echoed back by
  `describeTranslucency`. OpenCL keeps its settings on `ResourceContext`, so a one-off
  request pushes and restores them.
- `Document::createThumbnailPng` accepts a `RenderSettingsSnapshot` and pushes/pops
  translucency around the render.

## 7. Tests

- `TranslucencyChannel_tests` (12): graph qualification and codegen for both channels.
- `TranslucencyCodeGenerator_tests` (6), `TranslucencyKernelSource_tests` (6, reads the
  shipped `.cl` sources), `TranslucencyConfig_tests` (6), `ComputeBackend*_tests` (11).
- `TranslucencyRendering_tests` (3): end-to-end OpenCL. Two boxes stacked along the view
  axis with a constant channel; the opaque path hides the far box, the translucent path
  reveals it, so the centre pixel brightens by the expected Beer-Lambert factor.
  Note for future kernel tests: `updateParameterBlocking` must run *before*
  `refreshProgram`, otherwise unconnected constants compile to `parameter[0]`.

## 8. Selecting translucency per volume data (done)

The channel can now be attached to a `<volumedata>` the same way the color function is, so
translucency is authorable and persists across save/reload without waiting on lib3mf.

- Storage: a generic `<v:property name="translucency" functionid="N" channel="..." .../>`
  child of `<v:volumedata>` (spec §5.2.5 explicitly allows translucency as a property).
  lib3mf 2.6.0 round-trips this, so the file stays valid 3MF. The dedicated
  `<v:translucency>` element (§5.2.4) is still not written or read.
- `io/VolumeDataTranslucency` is the only place that knows the property key:
  `findTranslucencyProperty{,Index}`, `addTranslucencyProperty`, `removeTranslucencyProperty`,
  plus the `NO_TRANSLUCENCY_PROPERTY` sentinel.
- UI: `ui/VolumeDataTranslucencyView` renders the function + channel dropdowns inside the
  VolumeData table (`VolumeDataView.cpp`). Candidate functions come from
  `nodes::isQualifiedForScalarPropertyChannel` and their options from
  `nodes::collectScalarOutputChannelNames` (scalar `End` outputs only).
  Every edit goes through `applyMutation`, i.e.
  `update3mfModel -> mutate -> markFileAsChanged -> updateDocumentFrom3mfModel`, because
  `Document::clearBuildItems()` rebuilds the assembly *without* a `translucency` sink.
- Import: `Importer3mf::applyTranslucencyProperty` mirrors `applyColorProperty` - resolve the
  referenced function model, then
  `Builder::appendFunctionForTranslucencyOutput(..., channelName)`, which creates the `End`
  sink on demand. An unknown/vector channel or a second translucency definition logs a warning
  and is skipped instead of failing the import.
- `ResourceDependencyGraph::processVolumeData` already walks `GetPropertyCount()`, so
  dependency tracking needed no change; `Writer3mf::updateModel` only rewrites functions, so a
  UI-added property survives an update.
- Tests: `VolumeDataTranslucencyProperty_tests` (17 - lib3mf property CRUD, buffer round trip,
  `ModelUtils` channel collection, `Builder` wiring) and
  `VolumeDataTranslucencyImport_tests` (5 - import wiring, custom channel, unknown channel,
  duplicate definition, color + translucency together).
- Limitation: lib3mf has no `SetName`, so an existing property can only be removed and re-added.

## 9. Explicitly out of scope

- 3MF `<v:translucency>` read/write (lib3mf gap).
- Point-evaluation paths (SDF evaluation shaders, face-color sampling, mesh color export, contours).
- Refraction / index of refraction, subsurface scattering, coloured (RGB) attenuation.
- Changing the pixel output format (stays RGBA8 / CL_RGBA-float with alpha 1).
- Preview / low-resolution and slice-mode translucency.
