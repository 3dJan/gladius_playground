# Feature Specification: Render Mode Behavior and Interaction State Semantics

**Feature Branch**: `029-render-mode-behavior`  
**Created**: 2026-05-14  
**Status**: Draft  
**Input**: User description: "Based on the rendering-mode discussion in this session, define the behavior of the different modes, the expected state changes, and the intended user update experience."

## Problem Statement

The current rendering pipeline combines several update mechanisms:

- exact full-frame realtime raymarching,
- one-shot low-resolution preview,
- streaming low-resolution preview during parameter editing,
- static catch-up work such as parameter upload, bounding-box update, SDF precomputation, and high-quality progressive rendering.

During this session it became clear that the intended behavior of these mechanisms is not yet specified precisely enough. In particular, the system currently risks conflating:

- the **configured mode** (`Off`, `Auto`, `Force`),
- the **current interaction state** (`Static`, `CameraInteracting`, `ParameterInteracting`), and
- the **currently active render path** (exact realtime, low-res preview, streaming preview, static catch-up).

Without an explicit product-level contract, bugs emerge where:

- a parameter edit briefly flashes a surrogate or stale low-resolution preview before the intended interactive frame arrives,
- camera interaction becomes blocked by a previously-started parameter-preview loop,
- stale preview results overwrite a newer camera frame,
- Force mode is treated as proof that exact realtime output is currently active even when no exact realtime frame is actually in flight.

This specification defines the intended behavior of each mode, the valid state transitions, and the user-visible update experience.

## Design Goals

1. **Predictable mode semantics**: each rendering mode has a clear and testable contract.
2. **Responsive interaction**: camera and parameter edits must remain fluid and visually understandable.
3. **No stale-frame regressions**: old preview or HQ results must not replace fresher interactive output.
4. **Separation of policy and execution**: configured mode is a policy preference; actual in-flight jobs determine what is currently active.
5. **Graceful catch-up**: heavy background work should resume after interaction without visually disrupting the user.
6. **No quality regressions**: the viewport MUST NOT replace a currently-displayed frame with a lower-quality frame, even if the lower-quality frame is epoch-fresh.
7. **Stable Auto mode decisions**: once `Auto` mode commits to simpler rendering for a gesture, that decision is locked for the duration of the gesture to prevent mid-gesture quality oscillation.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Predictable Off Mode (Priority: P1)

As a user who disables realtime raymarching, I want all interactive feedback to use the preview path consistently, so that performance and image behavior are predictable.

**Why this priority**: Off mode is the explicit opt-out for exact realtime. It must be deterministic and easy to reason about.

**Independent Test**: Set the render mode to `Off`, move the camera, drag a parameter slider, and verify that no exact realtime path is used.

**Acceptance Scenarios**:

1. **Given** mode is `Off`, **When** the user moves the camera, **Then** the system shows preview-quality interactive feedback and does not schedule exact full-frame realtime rendering.
2. **Given** mode is `Off`, **When** the user drags a parameter slider, **Then** the system uses low-resolution preview or streaming preview feedback and does not schedule exact full-frame realtime rendering.
3. **Given** mode is `Off`, **When** interaction ends, **Then** the system transitions to static catch-up and eventually produces an updated high-quality frame.

---

### User Story 2 - Adaptive Auto Mode (Priority: P1)

As a user in `Auto` mode, I want the system to use exact realtime only when it is safe and beneficial, so that I get the best possible responsiveness without visual instability.

**Why this priority**: `Auto` is the default balancing mode between quality and responsiveness.

**Independent Test**: Run the same interaction sequence on both a fast and a slow scene and verify that camera behavior adapts while parameter behavior stays stable.

**Acceptance Scenarios**:

1. **Given** mode is `Auto` and recent exact realtime samples are within budget, **When** the user moves the camera, **Then** the system may schedule exact full-frame realtime rendering.
2. **Given** mode is `Auto` and exact realtime is not admissible, **When** the user moves the camera, **Then** the system falls back to preview-quality interaction without blanking the viewport.
3. **Given** mode is `Auto`, **When** the user drags a parameter slider, **Then** the system prefers low-resolution or streaming preview feedback rather than exact full-frame realtime.
4. **Given** mode is `Auto`, **When** interaction ends, **Then** the system resumes static catch-up and may use exact full-frame rendering only as part of static evaluation if budgets allow.
5. **Given** mode is `Auto` and the system has fallen back to preview-quality during an active gesture, **When** the user continues the same gesture, **Then** the system MUST NOT switch back to exact realtime mid-gesture.
6. **Given** mode is `Auto` and the last completed static HQ render took less than 100ms, **When** the user starts an interaction, **Then** the system may use exact realtime. **Given** the last static HQ render took 100ms or more, **When** the user starts an interaction, **Then** the system MUST use simpler rendering.

---

### User Story 3 - Exact Force Mode (Priority: P1)

As a user in `Force` mode, I want camera and parameter interactions to prefer exact full-frame realtime output, so that what I see during interaction matches the final result as closely as possible.

**Why this priority**: `Force` mode is the explicit request for exact interactive rendering. It must not silently degrade into a competing surrogate path unless exact rendering is truly unavailable.

**Independent Test**: Set mode to `Force`, drag a parameter slider, then immediately move the camera. Verify that no low-resolution surrogate flash appears and the camera remains responsive.

**Acceptance Scenarios**:

1. **Given** mode is `Force`, **When** the user moves the camera, **Then** the system prefers exact full-frame realtime rendering on every eligible tick.
2. **Given** mode is `Force`, **When** the user changes a parameter, **Then** the system prefers exact full-frame realtime rendering and must not start the streaming preview path in parallel.
3. **Given** mode is `Force` and exact realtime cannot start immediately, **When** the user continues interacting, **Then** the last valid frame remains visible rather than flashing a low-resolution surrogate or blank frame.
4. **Given** mode is `Force`, **When** camera interaction begins while parameter preview work is still active, **Then** camera interaction preempts that preview work and becomes the controlling interaction path.

---

### User Story 4 - Stable State Transitions (Priority: P1)

As a user switching between editing and navigation, I want the preview to transition cleanly between interaction types and static catch-up, so that I never lose control or see stale frames from the wrong path.

**Why this priority**: The highest-risk bugs in this session were transition bugs rather than pure rendering-speed problems.

**Independent Test**: Repeatedly alternate between parameter drag, camera movement, and idle periods while observing whether stale low-res or HQ frames overwrite the current interaction.

**Acceptance Scenarios**:

1. **Given** a parameter interaction is active, **When** the user begins moving the camera, **Then** camera interaction supersedes parameter interaction for scheduling and presentation.
2. **Given** an interactive frame is visible, **When** background bbox or SDF catch-up starts, **Then** the current interactive frame remains visible until a newer valid frame is ready.
3. **Given** a preview result was produced for an older view or parameter epoch, **When** a newer interaction state exists, **Then** the stale result is discarded and never presented.
4. **Given** interaction ends, **When** static catch-up begins, **Then** the system eventually converges to a high-quality current frame without showing older interactive artifacts.

---

### Edge Cases

- What happens if camera interaction begins while a parameter streaming preview loop is active? → Camera interaction must preempt and deactivate the parameter streaming path.
- What happens if exact realtime is configured (`Force`) but an exact realtime job is not actually running? → The system must treat this as a policy preference, not as proof that exact realtime output is currently active.
- What happens if bbox or SDF catch-up becomes ready while the user is still interacting? → Catch-up work may continue in the background, but it must not override or freeze the active interaction path.
- What happens if low-resolution preview results arrive with a stale `viewEpoch`? → They must be discarded before presentation.
- What happens if exact realtime cannot currently be admitted in `Force` mode? → Keep the most recent valid frame visible and retry exact realtime on the next eligible tick; do not flash a surrogate frame by policy.

## Mode Behavior Summary *(mandatory)*

### Configured Modes

| Mode | Camera Interaction | Parameter Interaction | Fallback Policy | Intended User Experience |
| --- | --- | --- | --- | --- |
| `Off` | Preview-quality only | Preview-quality only | Low-res/streaming preview | Stable, predictable surrogate interaction with HQ catch-up after idle |
| `Auto` | Exact realtime if last static HQ render < 100ms, else preview-quality; once fallen back to simpler during a gesture, stays simpler until gesture ends | Preview-quality preferred | Adaptive; hysteresis prevents mid-gesture quality oscillation | Best-effort responsiveness: quality adapts per scene speed, never switches back mid-gesture |
| `Force` | Exact full-frame realtime preferred | Exact full-frame realtime preferred | Keep current valid frame if exact path not yet ready | Exact interactive output whenever possible, never an intentional surrogate flash |

### Interaction-State Matrix

| Interaction State | Off | Auto | Force |
| --- | --- | --- | --- |
| `Static` | Static catch-up only | Static catch-up, optionally exact static full frame if admitted | Static catch-up, but interactive exact path takes priority whenever interaction resumes |
| `CameraInteracting` | Low-res preview | Exact realtime if admitted, else low-res preview | Exact realtime preferred; current valid frame held if admission is transiently blocked |
| `ParameterInteracting` | Low-res preview or streaming preview | Low-res preview or streaming preview | Exact realtime preferred; streaming preview must not be started |

## State Model *(mandatory)*

### Key Entities

- **ConfiguredMode**: user-selected realtime behavior policy (`Off`, `Auto`, `Force`).
- **InteractionState**: semantic interaction owner (`Static`, `CameraInteracting`, `ParameterInteracting`).
- **ExactRealtimeActive**: true only when an exact full-frame realtime job is actually in flight or being presented.
- **PreviewPath**: low-resolution one-shot or streaming preview path used for surrogate interaction feedback.
- **StaticCatchUp**: background work needed to converge to the latest high-quality result, including parameter upload, bounding box update, SDF precomputation, and HQ rendering.
- **Freshness Stamp**: scene/parameter/view/viewport identity used to reject stale work before presentation.
- **StaticHQRenderTime**: the wall-clock duration of the most recently completed HQ render in `Static` state. Used by `Auto` mode as the admission baseline: < 100ms allows exact realtime during interactions, ≥ 100ms requires simpler rendering.
- **AutoInteractionDecision**: a per-gesture flag set when `Auto` mode first evaluates admission for the current gesture. Once set to "simpler", it is locked for the remainder of the gesture and cleared only when the interaction ends.

### State Transition Rules

| Trigger | Previous State | Next State | Required Side Effects | User-Visible Outcome |
| --- | --- | --- | --- | --- |
| Camera input detected | Any | `CameraInteracting` | Increment `viewEpoch`; invalidate view-dependent interactive work; stop parameter streaming preview if active | Camera feedback becomes the highest-priority output |
| Parameter value changes with interaction active | `Static` or `ParameterInteracting` | `ParameterInteracting` | Increment `parameterEpoch`; invalidate parameter/bbox/SDF freshness; choose preview path based on mode | User sees immediate parameter feedback according to mode |
| Parameter interaction ends / debounce expires | `ParameterInteracting` | `Static` | Stop streaming preview if active; schedule bbox/SDF/HQ catch-up | Last interactive frame remains until static output catches up |
| Camera interaction ends | `CameraInteracting` | `Static` | Schedule static catch-up appropriate to latest parameter/view state | System converges from interactive feedback to current HQ output |
| Structural model change | Any | `Static` | Increment `sceneEpoch`; invalidate all dependent work; stop preview loops | Last valid frame may remain visible with busy feedback until fresh output arrives |
| Auto mode falls back during interaction | `CameraInteracting` or `ParameterInteracting` (Auto, exact path) | Same interaction state | Set `AutoInteractionDecision` = simpler; lock decision for remainder of gesture | Rendering simplifies without switching back until the gesture completes |

### Precedence Rules

1. **Camera interaction supersedes parameter interaction for presentation and scheduling.**
2. **Configured `Force` mode does not imply `ExactRealtimeActive`.**
3. **Preview suppression rules must key off actual exact realtime activity and frame freshness, not just configured mode.**
4. **Static catch-up must never overwrite a fresher interactive frame.**
5. **Low-resolution or streaming preview must never run in parallel as the authoritative interaction path in `Force` mode.**
6. **A lower-quality frame MUST NOT replace a higher-quality frame currently being displayed, even if the lower-quality frame carries a current epoch stamp.**
7. **In `Auto` mode, once the system has committed to simpler rendering for the current gesture, `AutoInteractionDecision` is locked for the rest of that gesture.**

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST expose three configured render modes: `Off`, `Auto`, and `Force`.
- **FR-002**: System MUST model interaction using the semantic states `Static`, `CameraInteracting`, and `ParameterInteracting`.
- **FR-003**: Camera input MUST transition the system to `CameraInteracting` and increment a view-freshness generation.
- **FR-004**: Parameter edits MUST transition the system to `ParameterInteracting` while interaction remains active and increment a parameter-freshness generation.
- **FR-005**: Structural model edits MUST invalidate program, parameter, bbox, SDF, and display freshness and transition the system to `Static`.
- **FR-006**: The system MUST distinguish configured mode from actual in-flight exact realtime activity.
- **FR-007**: Presentation logic MUST use actual in-flight exact realtime state to decide whether to preserve exact front buffers or suppress preview results.
- **FR-008**: In `Off` mode, the system MUST NOT schedule exact full-frame realtime rendering for camera or parameter interaction.
- **FR-009**: In `Auto` mode, the system MUST use exact full-frame realtime for camera interaction only when the last measured `StaticHQRenderTime` is less than 100ms. If `StaticHQRenderTime` is 100ms or more, the system MUST fall back to simpler rendering during interactions.
- **FR-010**: In `Auto` mode, parameter interaction MUST prefer preview-quality interaction feedback and MUST NOT require exact full-frame realtime.
- **FR-011**: In `Force` mode, camera interaction MUST prefer exact full-frame realtime on every eligible tick.
- **FR-012**: In `Force` mode, parameter interaction MUST prefer exact full-frame realtime and MUST NOT start the streaming preview path in parallel.
- **FR-013**: Beginning camera interaction while parameter streaming preview is active MUST deactivate or cancel the parameter streaming preview path.
- **FR-014**: If exact realtime cannot start immediately in `Force` mode, the system MUST keep the most recent valid frame visible rather than intentionally presenting a surrogate preview or blank frame.
- **FR-015**: Background bbox and SDF catch-up MUST NOT freeze camera interaction or overwrite a fresher interactive frame.
- **FR-016**: Preview, exact realtime, and HQ frames MUST carry enough freshness information to reject stale scene, parameter, view, or viewport results before presentation.
- **FR-017**: Low-resolution preview results produced for an older `viewEpoch` MUST be discarded when a newer camera view is active.
- **FR-018**: When interaction ends, the system MUST first render a final frame in the mode that was active during interaction (which may be a low-resolution preview frame) before starting HQ catch-up. HQ catch-up itself MUST then proceed in dependency order: parameter upload if needed, bounding-box update if stale, SDF precomputation if stale, then current-frame HQ rendering.
- **FR-019**: The system MUST support last-write-wins semantics for rapid parameter changes so that only the newest parameter state needs to be rendered.
- **FR-020**: The viewport MUST continue showing a valid frame while newer work is pending; the system MUST avoid blanking the render window during normal interaction-state transitions.
- **FR-021**: Preview-quality interaction paths MUST be allowed to trade fidelity for latency in `Off` and `Auto`, but MUST remain visually coherent and ordered.
- **FR-022**: The system SHOULD show busy or catch-up feedback while heavy static work is pending, without replacing the last valid frame.
- **FR-023**: In `Auto` mode, once `AutoInteractionDecision` is set to "simpler" for a gesture, the system MUST NOT schedule exact full-frame realtime rendering until the gesture ends and `AutoInteractionDecision` is cleared.
- **FR-024**: The system MUST measure the wall-clock duration of each completed static HQ render and record it as `StaticHQRenderTime`. This value MUST be used as the sole admission criterion for exact realtime in `Auto` mode.
- **FR-025**: When transitioning between rendering quality levels (exact realtime ↔ preview), the first frame at the new quality level MUST reflect the same scene and view state as the last frame displayed at the previous level. The system MUST NOT present a frame from a stale or different model/view state during a quality transition.
- **FR-026**: The system MUST NOT present a lower-quality frame in place of a higher-quality frame that is already being shown, even if the lower-quality frame carries a current epoch stamp.
- **FR-027**: The low-resolution preview path MUST use precomputed distance field data when a fresh precomputed SDF is available for the current scene and parameter state.
- **FR-028**: The low-resolution preview path MUST dynamically adapt its framebuffer resolution to maintain a target frame rate during interaction.
- **FR-029**: Progressive HQ catch-up MUST begin with a low-resolution result and incrementally add scanlines or detail passes in full-quality mode until the full-resolution HQ frame is complete.
- **FR-030**: During progressive HQ rendering, each completed pass MUST be presented to the user immediately so that quality visibly improves over time rather than appearing only when fully complete.

## Intended User Experience *(mandatory)*

### Off Mode

- The viewport behaves predictably and conservatively.
- During camera motion and parameter edits, the user sees preview-quality feedback.
- The system may use low-resolution or surrogate representations, but behavior is consistent and understandable.
- Once interaction ends, the system catches up to a current HQ frame.

### Auto Mode

- The viewport feels adaptive.
- Whether the system uses exact rendering or simpler preview is determined by how long the last static HQ render took: under 100ms uses exact rendering; 100ms or more uses simpler preview.
- Once the system has committed to simpler rendering for a gesture (e.g. a camera drag), it stays in that mode for the entire gesture. There is no mid-gesture switch back to higher quality, which would be visually jarring.
- Parameter edits remain responsive and visually stable, without forcing an exact full-frame path that could destabilize responsiveness.
- The user should never need to guess whether the system is "stuck"; the latest valid frame remains visible while catch-up proceeds.
- Quality never regresses: the viewport never replaces a sharp frame with a blurrier one.

### Force Mode

- The viewport prioritizes exactness during interaction.
- Camera movement should feel like exact interactive rendering, not a switch to a visibly different surrogate path.
- Parameter edits should behave like camera motion in the sense that they request immediate exact updates rather than a competing low-res streaming path.
- If exact rendering is temporarily blocked by transient conditions, the viewport should hold the latest valid frame and remain responsive instead of flashing a stale or lower-fidelity alternative.

### Interaction Transitions

- Starting camera movement after parameter editing should feel like a clean handoff, not a conflict between two rendering policies.
- Ending interaction should feel like a convergence from “interactive latest valid frame” to “current HQ frame,” not like a reset or visual regression.
- Users should not see a stale frame from the wrong interaction type appear late.

## Clarifications

### Session 2026-05-14

- Q: Does `Force` mode mean exact realtime output is always currently active? → A: No. `Force` is a scheduling policy preference, not proof that an exact realtime frame is currently in flight.
- Q: Should parameter editing in `Force` mode use the same streaming preview path as `Off`/`Auto`? → A: No. `Force` mode parameter interaction should prefer exact full-frame realtime and must not start the streaming preview path in parallel.
- Q: What happens if camera motion begins while parameter preview is active? → A: Camera interaction preempts parameter streaming preview and becomes the controlling interaction path.
- Q: What should happen if a stale preview result arrives after the camera moved? → A: The stale preview result must be discarded and not presented.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In `Off` mode, no exact full-frame realtime jobs are scheduled during camera or parameter interaction.
- **SC-002**: In `Auto` mode, camera interaction falls back cleanly between exact realtime and preview-quality interaction without stale-frame artifacts.
- **SC-003**: In `Force` mode, parameter changes do not trigger a visible low-resolution surrogate flash.
- **SC-004**: In `Force` mode, beginning camera interaction immediately after parameter editing does not freeze the viewport or require waiting for a parameter preview loop to finish.
- **SC-005**: Stale preview frames are never displayed after a newer camera or parameter state has been presented.
- **SC-006**: When interaction ends, the system always converges to a current HQ frame for the latest scene, parameter, view, and viewport state.
- **SC-007**: During normal interaction-state transitions, the viewport always shows either the newest valid frame or a busy/catch-up state over the last valid frame; it never intentionally blanks the window.
- **SC-008**: In `Auto` mode, during a single continuous gesture (one camera drag or one parameter drag), the rendering quality never switches from simpler preview back to exact realtime mid-gesture.
- **SC-009**: In `Auto` mode, the admission decision for exact realtime is demonstrably correlated with whether `StaticHQRenderTime` is below 100ms for the most recent static render.
- **SC-010**: The viewport never replaces a currently-displayed frame with a lower-quality frame that shows the same or an older model/view state.
- **SC-011**: After interaction ends, the first new frame presented shows the same model and view state as the last interactive frame, regardless of quality level.

## Assumptions

- Exact full-frame realtime rendering is more expensive than low-resolution preview and therefore must remain policy-controlled.
- Users interpret visibly different rendering paths as different semantic states; therefore sudden surrogate flashes in `Force` mode are considered correctness bugs, not merely quality tradeoffs.
- The existing async infrastructure can represent freshness using scene, parameter, view, and viewport generations.
- Catch-up work such as bbox and SDF updates may continue in the background as long as it does not become the authoritative interactive presentation path.

## Out of Scope

- Kernel-level performance optimization of realtime raymarching.
- UI wording or menu design for exposing the three modes.
- Export or file-loading behavior outside the preview/update pipeline.
