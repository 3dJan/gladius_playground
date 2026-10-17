# Plan: Make the Slice View fully usable with the WebGPU backend

Goal: identical slice-preview UX with WebGPU as with OpenCL — open the slice window,
drag the Z slider, see contours update live, zoom/pan/center, distance measurement.

## Current state

Working:
- Backend contour pipeline (`Document::generateContourWebGpu`) + parity tests pass.
- Slice window opens in WebGPU mode (no crash after the null-core fix).
- A "Slice Position [mm]" slider exists in the Settings dialog (menu → Settings),
  wired to `SliceView::setSliceHeight()`.
- Canvas rendering (grid, rulers, zoom/pan, contour drawing, distance measurement)
  is implemented in `SliceViewWebGPU.cpp::renderImpl`.

Broken / missing (why no contours appear today):

1. **Contour generation almost never triggers.**
   `SliceViewWebGPU.cpp::renderImpl` gates generation on
   `m_document->dispatchStructuralUpdateIfReady()` — this only returns `true`
   while a *debounced structural edit* is pending. After a plain file load (or a
   pure parameter change) nothing is pending, so the condition is false forever
   and the cache is never filled.
2. **Flat assembly can go stale.** `generateContourWebGpu` reads
   `getFlatAssembly()`. Without a dispatched structural update, edits/loads are
   not reflected in contours.
3. **`updateContours()` drops invalidation in mixed builds.** In an
   OpenCL-enabled build running the WebGPU backend (`m_core == nullptr`), the
   `#else` branch of `MainWindow::updateContours()` early-returns on `!m_core`,
   so `m_contoursDirty` from parameter edits never reaches the slice view.
4. **Shortcuts dead:** `SliceView::isHovered()` is hardcoded `false` in the
   WebGPU translation unit → Alt+/Alt−/R and wheel zoom never fire.
5. **Center View / auto-center broken:** `centerView()` just calls
   `resetView()`; it does not fit-to-contour like the OpenCL version
   (`calculateContourBounds` + zoom-to-fit).
6. **Slider range is fake.** `Document::computeBoundingBox()` returns a fixed
   0–400 mm fallback when `m_core` is null, so the slider rarely matches the
   actual model height.
7. **UI stutter risk:** generation is synchronous (~100 ms+); dragging the
   slider regenerates inline on the UI thread.

## Work items

### W1 — Fix contour generation trigger (blocking, small)

In `SliceViewWebGPU.cpp::renderImpl`:
- Generate whenever `!m_webGpuCacheValid` **or** the throttle interval elapsed —
  remove the `dispatchStructuralUpdateIfReady()` gate entirely.
- Before generating, ensure the flat assembly reflects the current model:
  if `m_document->hasStructuralEditPending()` and the debounce elapsed, call
  `dispatchStructuralUpdateIfReady()`; otherwise rely on the last flat assembly.
  (Keeps edits visible without coupling generation to the debouncer.)
- Key the cache by `(m_sliceZ_mm, m_minFeatureSize_mm, document structuralEditEpoch())`
  instead of a single bool, so model changes invalidate correctly.

### W2 — Route invalidation correctly in mixed builds (small)

`MainWindow::updateContours()`: replace the `#if/#else` split with a single
backend-neutral body:
- If `m_core` → existing OpenCL behavior (`invalidateContourCache`).
- Else → `m_sliceView.invalidateContours(); m_contoursDirty = false;`

Same treatment inside `sliceWindow()` is already done (null-core guard).

### W3 — Feature parity for view helpers (small)

In `SliceViewWebGPU.cpp`:
- `isHovered()`: implement like the OpenCL version
  (`ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && isVisible()`).
- `centerView()`: port the OpenCL implementation (bounds-based zoom-to-fit using
  `calculateContourBounds()`); keep `resetView()` as fallback when no contours.
  This also makes the empty→content auto-center transition work.

### W4 — Real slider range (medium)

Give the WebGPU path real model bounds instead of the 0–400 fallback:
- Preferred: use `m_runtime->getBoundsService()` (already on
  `IBackendRuntime`) to obtain analytic bounds; fall back to
  `computeBoundingBox()`.
- Cache bounds per structural epoch; refresh on file load / edit.
- MainWindow settings dialog uses these bounds for zMin/zMax (with small margin),
  matching the OpenCL feel. Initialize `m_uiSliceHeight_mm` to mid-height on
  first show / after load.

### W5 — Async generation + progress semantics (medium)

Mirror `core.isSlicingInProgress()` UX without blocking the UI thread:
- Move `generateContourWebGpu` onto a `std::future` owned by `SliceView`
  (one in-flight job; latest-request-wins: if a newer request arrives, drop the
  stale result when it lands).
- Expose `bool isSlicingInProgress() const` on `SliceView`; renderImpl shows the
  existing loading indicator while a job runs and swaps in results on completion.
- Keep the synchronous path as fallback for headless/tests.

### W6 — Optional polish (can defer)

- Inline mini-slider at the top of the Slice window itself (both backends) so
  users don't need the Settings dialog; keep the Settings entry in sync.
- Normals / source-vertex overlays: currently disabled with a hint; leave as
  OpenCL-only unless the WGSL evaluator later exposes gradient samples cheaply.
- Status-bar hint "Slicing…" while a WebGPU job is in flight.

## Tests

- Existing: `*ContourExtractionBackendParity*` (must stay green).
- New unit test: `Document_ContourWebGpu_AfterLoadWithoutStructuralEdit_ReturnsContours`
  — load template doc, call `generateContourWebGpu` directly (guards W1's
  decoupling from the debouncer). GPU-gated (`GLADIUS_RUN_WEBGPU_TESTS=1`).
- Manual smoke checklist (WebGPU preset):
  1. Launch → open example model → menu Slice → window shows contours.
  2. Settings → drag Slice Position slider → contours follow within ~100 ms.
  3. Edit a parameter in the node editor → contours update.
  4. Alt+= / Alt− / R and wheel zoom work over the slice window.
  5. Center View fits contours; auto-center on first content.

## Order of execution

W1 + W2 (unblock: contours actually appear) → W3 (shortcuts/centering) →
W4 (real slider range) → W5 (smooth dragging) → W6 (polish).
