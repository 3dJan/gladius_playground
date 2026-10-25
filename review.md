# Code Review: Render Mode Behavior and Parameter Changes

## Summary

Reviewed the generated C++/build-system diff against `develop` (`review.diff`, 6573 lines). The branch introduces a render-update coordinator, realtime raymarch admission controller, async presentation changes, low-resolution/streaming preview plumbing, and unit tests around these behaviors.

The intended behavior from `specs/029-render-mode-behavior/userspec.md` is clear: camera movement should always remain responsive; `Off` uses previews; `Auto` adapts but keeps parameter edits preview-oriented; `Force` should prefer exact full-frame realtime for both camera and parameter edits and must not intentionally show a lower-quality surrogate. Parameter interactions should also end cleanly and resume static catch-up.

The most important issues are correctness gaps around parameter changes. In particular, Force-mode parameter edits are still routed through low-resolution preview, and Force-mode parameter interactions may never transition back to `Static` because semantic interaction completion is coupled to the streaming-preview lifecycle.

## Findings

### 1. Force-mode parameter edits are scheduled as low-resolution preview instead of exact realtime

- **Location**: `gladius/src/ui/render/RenderUpdateCoordinator.h:425`
- **Issue**: `scheduleInteractiveFrame()` returns immediately for every `ParameterInteracting` state and starts `RenderTaskType::LowResolutionPreview` before checking the configured render mode. This directly violates the userspec for `Force`: “When I drag a slider I want to see the actual updated model immediately, not a blurry approximation.” It also explains why parameter changes do not fulfill the spec even though Force camera motion can schedule `RealtimeFullFrame`.
- **Suggestion**:

```cpp
if (m_interactionState == RenderInteractionState::ParameterInteracting)
{
    if (m_realtime.config().mode == RealtimeRaymarchMode::Force)
    {
        if (m_realtime.canAttemptRealtime(m_width, m_height, m_realtimeGuards))
        {
            startTask(decision, RenderTaskType::RealtimeFullFrame, m_latestStamp);
            return;
        }

        keepCurrentFrame(decision);
        return;
    }

    startTask(decision, RenderTaskType::LowResolutionPreview, m_latestStamp);
    return;
}
```

- **Rationale**: This preserves the preview path for `Off` and `Auto` parameter edits while making `Force` behave like an exact-rendering policy. If realtime admission is temporarily blocked, it follows FR-014 by keeping the current frame instead of falling back to a surrogate preview.

### 2. Force-mode parameter interactions may never end in the coordinator

- **Location**: `gladius/src/ui/MainWindow.cpp:2724`, `gladius/src/ui/RenderWindow.cpp:2075`, `gladius/src/ui/RenderWindow.cpp:4183`
- **Issue**: `MainWindow` disables streaming preview in Force mode via `useStreamingPreview = !m_renderWindow.isForceRealtimeMode()`. Later, the debounce path calls `stopStreamingPreview()` to signal that the parameter drag ended. However, `stopStreamingPreview()` only calls `notifyParameterInteractionEnded()` when `m_streamingPreviewActive` was true. In Force mode streaming was never started, so `wasActive` is false and the coordinator can remain stuck in `ParameterInteracting`. While stuck, `tick()` keeps scheduling interactive work and never runs `scheduleStaticCatchUp()`, so bbox/SDF/HQ convergence can fail to follow the specified post-interaction sequence.
- **Suggestion**:

```cpp
void RenderWindow::finishParameterInteraction()
{
    m_streamingPreviewActive.store(false, std::memory_order_release);
    m_streamingFrameConsumed.store(true, std::memory_order_release);

    if (m_renderUpdateCoordinator.interactionState() ==
        async_rendering::RenderInteractionState::ParameterInteracting)
    {
        queueRenderDecision(m_renderUpdateCoordinator.notifyParameterInteractionEnded());
    }
}
```

Then call `finishParameterInteraction()` from the debounce block instead of using `stopStreamingPreview()` as the semantic end-of-interaction signal. Keep `stopStreamingPreview()` as a transport/lifecycle helper for the streaming worker if needed.

- **Rationale**: “Parameter interaction ended” is a semantic state transition and should not depend on whether the chosen feedback path happened to be streaming preview. Decoupling these concepts lets `Force` parameter edits resume static catch-up even when no streaming worker was active.

### 3. Active parameter changes keep stale Auto-mode realtime learning

- **Location**: `gladius/src/ui/render/RenderUpdateCoordinator.h:169`
- **Issue**: `notifyParameterChanged(true)` increments `parameterEpoch` but only calls `resetRealtimeLearning()` when `interactionActive` is false. During a slider drag, the old `m_staticHQRenderTimeMs` from the previous parameter state remains available. If the user releases the slider and moves the camera before a new static HQ sample is measured, `Auto` can admit exact realtime using a timing sample from the old model/parameter state.
- **Suggestion**:

```cpp
++m_latestStamp.parameterEpoch;
resetRealtimeLearning();
m_parameterUploadStamp.reset();
m_boundingBoxStamp.reset();
m_sdfStamp.reset();
```

If resetting on every drag tick is considered too aggressive, reset at the first parameter epoch change for the gesture and keep the value invalid until the next static HQ completion for the new epoch.

- **Rationale**: The userspec says Auto should decide from the high-quality render time in static mode for the current state. Parameter changes can materially alter render cost, so old timing data should not admit exact realtime for a new parameter state.

### 4. `isRealtimeActive()` still treats Force mode as actual realtime activity

- **Location**: `gladius/src/ui/render/RealtimeRaymarchController.cpp:310`, used at `gladius/src/ui/RenderWindow.cpp:2381`
- **Issue**: `RealtimeRaymarchController::isRealtimeActive()` returns true whenever mode is `Force`, even if no exact realtime job is in flight and no exact realtime frame is being presented. This contradicts the spec clarification that `Force` is a scheduling policy preference, not proof that exact realtime output is active. `RenderWindow` then uses this value to compute `useLowResPreviewBeforeHq`, coupling static catch-up behavior to configured mode instead of actual render state.
- **Suggestion**:

```cpp
bool RealtimeRaymarchController::isRealtimeActive() const noexcept
{
    return m_realtimeActive;
}
```

For Force-specific policy checks, add a differently named method such as `isRealtimePreferred()` or check `config().mode == RealtimeRaymarchMode::Force` at the policy site. For presentation/catch-up decisions in `RenderWindow`, prefer actual state, for example:

```cpp
auto const exactRealtimeJobInFlight =
    m_asyncRealtimeJobInFlight.load(std::memory_order_acquire);
bool const useLowResPreviewBeforeHq = !exactRealtimeJobInFlight;
```

- **Rationale**: Separating policy preference from actual activity avoids stale-frame and catch-up decisions being made as if exact output exists when it does not. This aligns with FR-006/FR-007 and the userspec’s Force-mode clarification.

### 5. The unit test locks in behavior opposite to the Force-mode userspec

- **Location**: `gladius/tests/unittests/ui/render/RenderUpdateCoordinator_tests.cpp:347`
- **Issue**: `ParameterDrag_WithForcedRealtime_UsesPreviewNotRealtime` asserts that Force-mode parameter dragging starts `LowResolutionPreview` and not `RealtimeFullFrame`. This directly conflicts with the attached userspec and makes the implementation regress toward the wrong product behavior.
- **Suggestion**:

```cpp
TEST(RenderUpdateCoordinator, ParameterDrag_WithForcedRealtime_StartsRealtimeFullFrame)
{
    RenderUpdateCoordinator coordinator;
    coordinator.configureRealtime(forcedRealtimeConfig());
    ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

    auto const decision = coordinator.notifyParameterChanged(true);

    EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
    EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
    EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
}
```

Also add a guarded variant that verifies Force parameter interaction emits `KeepCurrentFrame` when realtime admission is blocked instead of falling back to preview.

- **Rationale**: Tests should encode the spec contract. As written, this test can cause future fixes to be rejected and is likely why the implementation currently fails parameter-change expectations in Force mode.

## Notes

No build-system-specific findings were found in the reviewed diff. The highest-risk area is the interaction between `MainWindow` parameter throttling, `RenderWindow` streaming-preview lifecycle, and `RenderUpdateCoordinator` semantic state. The immediate fix should start with the coordinator’s Force parameter branch and the semantic parameter-end transition, then update tests to match the userspec.
