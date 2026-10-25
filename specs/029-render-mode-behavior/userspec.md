# User Specification: Rendering Mode Behavior

**Created**: 2026-05-15  
**Source**: User input from session 2026-05-14

---

## What the user wants

### Camera movement should always work

Moving the camera (orbiting, panning, zooming) is the most basic interaction in the preview window.  
No matter what mode is selected and no matter what was being done before (editing a slider, waiting for a model to compile), moving the camera should:

- feel immediate — the view should respond as I drag,
- never freeze mid-movement,
- never require waiting for some previous operation to finish first.

If I was editing a parameter and then grab the camera, the parameter editing is over.  
Camera wins. The view moves.

---

### Each mode should mean something clear

The three rendering modes available in the menu should have predictable and distinct meanings.

**Off**

I have turned off the fancy interactive rendering.  
Everything I see while moving the camera or editing parameters is a simplified preview.  
It is fine to look less exact than the final result. That is the tradeoff I made.  
When I stop moving, the system should catch up and show me a proper result.

**Auto**

The system decides.  
On fast models it tries to show me exact-looking results while I interact.  
On slow models it falls back to something simpler.  
Either way, the camera should still move without freezing.  
I do not need to know which strategy it chose.

**Force**

I have turned on exact interactive rendering. I want to see the real result as I interact.  
When I move the camera I want to see the actual raymarched model, not a blurry approximation.  
When I drag a slider I want to see the actual updated model immediately, not a blurry approximation.

If the system cannot keep up immediately it should just hold the last frame it produced  
and continue as fast as it can. It must not suddenly switch to a visibly different, lower-quality image.  
It must not freeze.

---

### Switching between editing and moving should be seamless

A typical session looks like:

1. I drag a slider to change a radius.
2. I orbit the camera to check the shape from a different angle.
3. I drag the slider again.

None of those steps should:

- leave the viewport blank,
- show a leftover frame from the wrong step,
- cause a visible jump to a lower-quality image,
- freeze the camera because step 1 is still running.

When I stop doing anything, the system should quietly catch up to a full-quality current result in the background. If the full-quality result would take a long time to produce, it should be rendered progressively, starting with a low-res preview by rendering more lines in full-quality mode.

---

### In Auto mode the transition between "fast enough" and "not fast enough" should only happen towards the faster version during interactions
If the system is in Auto mode and it decides that the model is too slow to render in full quality while I interact, it should switch to a simpler rendering mode. But it than should stay in that mode until I stop interacting. It should not switch back and forth while I am dragging a slider or moving the camera.

---

### How the renderer decides what is "fast enough" for Auto mode
The time for the high quality rendering in static mode should be measured and used as a baseline for deciding what is "fast enough" for Auto mode. If the high quality rendering takes more than 100ms, then Auto mode should switch to a simpler rendering mode during interactions. If it takes less than 100ms, then Auto mode should use the high quality rendering during interactions.

### Transitions between rendering qualities should be seamless
When switching between rendering qualities (either because of mode change or because Auto mode decides to switch), the transition should be seamless in the sense that the same state of the model and view is shown. For example, if I am dragging a slider and the system decides to switch to a simpler rendering mode, it should not suddenly show me a different frame that was rendered in the previous mode. It should continue showing the same frame but just render it in the new mode.

### What the low-res preview is made from
The low res preview uses the precomputed distance field when available and adopts its framebuffer resolution to the framerate.

### How catch-up is sequenced after interaction ends
When I stop interacting, the system should start a catch-up process to produce a full-quality current result if the current frame is not already full-quality. This process should be done in the background and should not block the UI. The system should first render a final frame in the current mode (which may be a low-res preview) and then start rendering the full-quality result. If the full-quality result takes a long time to produce, it should be rendered progressively, starting with a low-res preview by rendering more lines in full-quality mode.The time of the high quality rendering in static mode should be measured (s. above).
