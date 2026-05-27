# Render Mode State Transition Trace

**Date**: 2026-05-15  
**Scope**: Parameter and camera interaction update flow for `Off`, `Auto`, and `Force` render modes.

This document records the actual state transitions found in the code while investigating why the previous unit tests passed even though interactive parameter updates did not match `userspec.md`.

## Key components

- `MainWindow::updateModel()` detects dirty model/parameter state and decides whether to notify `RenderWindow`.
- `RenderWindow::invalidateViewDueToParameterChange()` marks render state dirty and queues `RenderUpdateCoordinator::notifyParameterChanged(true)`.
- `RenderUpdateCoordinator` owns semantic state: `Static`, `CameraInteracting`, `ParameterInteracting`.
- `RenderWindow::renderAsync()` translates coordinator commands to concrete async jobs, but it also still has a legacy low-resolution preview branch driven by `state.isMoving`, `m_forceLowResRenderOnNextFrame`, and `m_lowResFeedbackPending`.
- `RenderWindow::processAsyncPreviewResults()` presents low-resolution preview results unless they are stale or suppressed.

## Pre-fix transition trace

### Force mode, continuous parameter drag

1. `MainWindow::updateModel()` sees `m_parameterDirty`.
2. Because `isForceRealtimeMode()` is true, streaming preview is not started.
3. `RenderWindow::invalidateViewDueToParameterChange()` queues `notifyParameterChanged(true)` and clears low-res flags.
4. `RenderUpdateCoordinator` can schedule `ParameterUpload` and `RealtimeFullFrame`.
5. However, `RenderWindow::renderAsync()` later reaches the legacy low-res branch if `state.isMoving`, `forceLowResRender`, or `lowResPending` is true.
6. `state.isMoving` can be true for reasons unrelated to the coordinator's realtime policy (camera debounce, resize/centering paths, or other UI motion state).
7. The legacy branch can call `scheduleAsyncPreviewJob()` / `renderLowResPreview()` even while Force policy says to hold the current frame or use exact realtime.

**Failure**: The coordinator tests passed because they only asserted the semantic command stream. They did not cover the later `RenderWindow::renderAsync()` fallback that could still schedule low-res preview.

### Auto/Off mode, continuous parameter drag with streaming preview

1. First dirty parameter update enters `MainWindow::updateModel()`.
2. `RenderWindow::invalidateViewDueToParameterChange()` is called.
3. `RenderWindow::startStreamingPreview()` starts the streaming preview coroutine.
4. Subsequent parameter changes happen while `isStreamingPreviewActive()` is true.
5. The old `MainWindow` branch skipped `invalidateViewDueToParameterChange()` while streaming was active.
6. It still cleared `m_parameterDirty`, so no render-window state was refreshed for the new parameter value.
7. `m_lastParameterChangeTime` remained at the first change, allowing the debounce path to conclude that the drag ended while the user was still changing parameters.
8. Streaming could be stopped and static catch-up could start from a stale interaction boundary.

**Failure**: Continuous Auto/Off parameter changes could be visually dropped or cut short because repeated dirty events refreshed neither the semantic interaction nor the debounce timestamp.

### Force mode, parameter interaction end

1. Force mode intentionally does not start streaming preview.
2. The old debounce path called `stopStreamingPreview()` as the way to signal parameter interaction end.
3. `stopStreamingPreview()` only notified the coordinator if streaming had been active.
4. In Force mode, streaming was inactive, so the coordinator could remain in `ParameterInteracting`.

**Failure**: Static catch-up could fail to begin because semantic interaction end was coupled to the streaming transport lifecycle.

## Fixed transition model

### Force mode parameter update

1. `MainWindow::updateModel()` classifies the parameter change as `invalidateInteraction=true`, `startStreamingPreview=false`.
2. `RenderWindow::invalidateViewDueToParameterChange()` queues the coordinator parameter-change event.
3. `RenderUpdateCoordinator::scheduleInteractiveFrame()` schedules `RealtimeFullFrame` for `ParameterInteracting` in `Force` mode when guards allow it.
4. If guards block exact realtime, the coordinator emits `KeepCurrentFrame` and does not schedule preview.
5. `RenderWindow::renderAsync()` uses `shouldUseLegacyLowResPreview()` to suppress the legacy low-res branch whenever Force is the active interaction policy.

### Auto mode parameter update

1. First parameter dirty event invalidates the interaction and starts streaming preview.
2. Later dirty events while streaming is active use `refreshStreamingParameterInteraction()` instead of being ignored.
3. The refresh keeps the interaction alive by updating `m_lastParameterChangeTime`, dirty flags, SDF invalidation, and bbox-stale state.
4. Streaming preview continues to push latest Assembly parameters and render low-res feedback.
5. When dirty events stop and the debounce interval elapses, `finishParameterInteraction()` performs the semantic transition to `Static` and catch-up begins.

### Camera preemption

1. Camera input calls `invalidateCameraView()`.
2. Streaming preview flags are cleared immediately.
3. The coordinator transitions to `CameraInteracting` and schedules the correct mode-dependent camera feedback.
4. Force and Auto-exact interactions suppress legacy low-res fallback; Off and Auto-preview can use preview feedback.

## Unit-test coverage map

| Requirement from userspec | Unit coverage |
| --- | --- |
| Camera movement schedules according to mode and does not wait for previous preview | `RenderUpdateCoordinator.CameraChanged_*`, `RenderUpdateCoordinator.CameraBackpressure_*` |
| `Off` interactions use preview-quality feedback | `RenderUpdateCoordinator.CameraChanged_WithAutoLearning_StartsLowResolutionPreview`, `RenderModeUpdatePolicy.LegacyLowResPreview_WithAutoPreviewParameterInteraction_IsAllowed` (Off policy covered by dispatch tests) |
| `Auto` uses exact only after fast static HQ and otherwise preview | `RenderUpdateCoordinator.CameraChanged_WithAutoRealtimeAfterFastSamples_StartsRealtimeFullFrame`, `RenderUpdateCoordinator.ActiveParameterChanged_AfterFastAutoSample_ResetsInteractionAdmission` |
| Auto simpler decision does not oscillate mid-gesture | `RenderUpdateCoordinator.CameraChanged_WithAutoRealtimeGuardBlocker_KeepsCurrentFrame`, per-gesture latch tests in coordinator suite |
| Force camera uses exact realtime or holds current frame | `RenderUpdateCoordinator.CameraChanged_WithForcedRealtime_StartsRealtimeFullFrame`, guard-blocker tests |
| Force parameter edits use exact realtime, not preview | `RenderUpdateCoordinator.ParameterDrag_WithForcedRealtime_StartsRealtimeFullFrame`, `RenderModeUpdatePolicy.LegacyLowResPreview_WithForceParameterInteraction_IsSuppressed` |
| Force parameter guard blockers hold current frame | `RenderUpdateCoordinator.ParameterDrag_WithForcedRealtimeGuardBlocker_KeepsCurrentFrame` |
| Continuous Force parameter updates never fall through to preview | `RenderUpdateCoordinator.ContinuousParameterDrag_WithForcedRealtimeInFlight_NeverStartsPreview`, `RenderModeUpdatePolicy.LegacyLowResPreview_WithForceParameterInteraction_IsSuppressed` |
| Continuous Auto/Off parameter updates are not dropped while streaming | `RenderModeUpdatePolicy.ParameterChangeDispatch_WithAutoStreamingActive_RefreshesInteraction`, `RenderModeUpdatePolicy.ParameterChangeDispatch_WithOffStreamingActive_RefreshesInteraction`, `RenderUpdateCoordinator.ContinuousParameterDrag_WithStaleAutoPreview_CompletesLatestPreview` |
| Parameter interaction end is semantic, not dependent on streaming transport | `RenderWindow::finishParameterInteraction()` implementation; policy documented here. A direct GL-free unit test is covered through dispatch policy and coordinator static catch-up tests. |
| Static catch-up sequence after interaction | `RenderUpdateCoordinator.StaticCatchUp_AfterParameterEdit_SequencesUploadBboxSdfThenHq` |
| No lower-quality preview replaces an exact/HQ frame | `RenderModeUpdatePolicy.LegacyLowResPreview_WithExactRealtimeInFlight_IsSuppressed` plus preview suppression in `processAsyncPreviewResults()` |

## Remaining caveat

`RenderWindow` is still difficult to test directly because most concrete scheduling functions require a live `ComputeCore`, OpenCL images, and a GL context. The low-level decisions that caused the observed regressions are now extracted into `RenderModeUpdatePolicy.h` so they are unit-tested without GL/OpenCL, while `RenderWindow` and `MainWindow` call that policy in production.
