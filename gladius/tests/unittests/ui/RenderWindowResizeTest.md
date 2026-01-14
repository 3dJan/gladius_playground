# Developer Testing Guide: Fix Resize Flicker

**Feature**: Window Resize Without Flicker  
**Branch**: `001-fix-resize-flicker`  
**Date**: 2026-01-06

## Overview

This guide provides step-by-step instructions for developers to verify that window resize operations no longer cause render area clearing or flicker. All testing is manual and visual, as the fix addresses a user-perceived visual quality issue.

## Prerequisites

- Gladius built successfully with the resize flicker fix
- A project file with rendered 3D content (use `gladius/examples/` for test models)
- Monitor setup: single monitor (minimum), dual-monitor setup with different DPI (optional, for P3 testing)

## Test Environment Setup

### 1. Build the Application

```bash
cd /home/jan/projects/gladius
# Use the standard VS Code build task
# Or run manually:
cd gladius
cmake --preset ReleaseWithDebug
cmake --build --preset ReleaseWithDebug
```

### 2. Prepare Test Content

```bash
# Option A: Use included example
./out/build/linux-releaseWithDebug/src/gladius examples/template.3mf

# Option B: Create test implicit surface scene with visible detail
# (launch Gladius and create a model with multiple visible features for flicker detection)
```

## Test Scenarios

### P1: Manual Window Resize (Core Functionality)

**Objective**: Verify that manually resizing the window by dragging edges or corners does not clear the rendered content.

#### Test 1.1: Single-Edge Horizontal Resize

**Steps**:
1. Launch Gladius with a rendered model visible in the viewport
2. Wait for high-quality rendering to complete (ensure detailed content is visible)
3. Move mouse to the right edge of the window until resize cursor appears
4. Click and drag the edge to make the window wider
5. Observe the viewport content during the drag

**Expected Behavior**:
- ✅ Rendered content remains visible throughout the drag
- ✅ Content may appear stretched/scaled temporarily but NEVER clears to blank
- ✅ After releasing mouse, a new preview renders at the correct dimensions
- ✅ No white/black flicker or flash during the entire operation

**Failure Indicators**:
- ❌ Viewport shows blank/white/black area during resize
- ❌ Visible flash or flicker when starting or stopping the drag
- ❌ Content disappears and reappears

#### Test 1.2: Single-Edge Vertical Resize

**Steps**:
1. Repeat Test 1.1 but drag the bottom edge vertically
2. Observe viewport during drag

**Expected Behavior**: Same as Test 1.1

#### Test 1.3: Corner Resize (Two-Dimensional)

**Steps**:
1. Drag the bottom-right corner diagonally to resize both dimensions simultaneously
2. Observe viewport during drag

**Expected Behavior**: Same as Test 1.1

#### Test 1.4: Rapid Continuous Resize (Stress Test)

**Steps**:
1. Rapidly drag window edge back and forth (small movements, 10-20 times in 5 seconds)
2. Observe for any flicker or clearing during the rapid changes

**Expected Behavior**:
- ✅ Content remains stable without clearing
- ✅ May show scaled/stretched content but NEVER blank frames
- ✅ After stopping, preview updates smoothly to new size

---

### P2: Maximize/Restore Stability (Common Workflow)

**Objective**: Verify that maximize and restore operations preserve rendered content.

#### Test 2.1: Maximize Transition

**Steps**:
1. Start with a normal-sized window showing rendered content
2. Click the maximize button (or double-click title bar, depending on window manager)
3. Observe viewport during the maximize animation

**Expected Behavior**:
- ✅ Content remains visible during maximize transition
- ✅ No clearing or flicker during animation
- ✅ After maximized, preview updates to fill new dimensions

#### Test 2.2: Restore Transition

**Steps**:
1. Start with a maximized window showing rendered content
2. Click the restore button (or double-click title bar)
3. Observe viewport during the restore animation

**Expected Behavior**: Same as Test 2.1

#### Test 2.3: Rapid Maximize/Restore Toggle

**Steps**:
1. Rapidly toggle maximize/restore 5 times in quick succession
2. Observe for flicker or clearing

**Expected Behavior**:
- ✅ Content remains visible through all transitions
- ✅ No flicker or clearing observed

---

### P3: Multi-Monitor Drag Stability (Advanced)

**Objective**: Verify content stability when dragging between monitors with different DPI or resolutions.

**Prerequisites**: 
- Two monitors connected with different DPI settings (e.g., 4K + 1080p, or different scaling factors)
- Linux display scaling configured differently for each monitor

#### Test 3.1: Standard-to-HiDPI Transition

**Steps**:
1. Position Gladius window on standard DPI monitor with rendered content
2. Drag window title bar to move to high-DPI monitor
3. Observe viewport during the transition

**Expected Behavior**:
- ✅ Content remains visible during drag between monitors
- ✅ No clearing when crossing monitor boundary
- ✅ After transition, content may briefly appear scaled then updates to correct DPI

#### Test 3.2: HiDPI-to-Standard Transition

**Steps**:
1. Reverse of Test 3.1 - start on high-DPI monitor, drag to standard DPI
2. Observe viewport during transition

**Expected Behavior**: Same as Test 3.1

---

### Edge Cases

#### Test E.1: Resize to Minimum Dimensions

**Steps**:
1. Drag window edges to make window as small as possible (until window manager prevents further resize)
2. Observe viewport during resize to very small dimensions

**Expected Behavior**:
- ✅ Content remains visible (scaled down) without clearing
- ✅ No crash or error if viewport becomes very small (e.g., 100x100 pixels)

#### Test E.2: Resize During Active Rendering

**Steps**:
1. Make a model change that triggers long progressive rendering (e.g., adjust a parameter)
2. While progressive rendering is in progress (watch progress chunks), resize window
3. Observe whether rendering cancels gracefully and content persists

**Expected Behavior**:
- ✅ Progressive rendering cancels (expected)
- ✅ Last completed content remains visible
- ✅ New preview starts at new dimensions after resize completes
- ✅ No flicker or clearing during cancellation

#### Test E.3: Minimize and Restore

**Steps**:
1. Minimize window to taskbar/dock
2. Restore window from taskbar/dock
3. Observe viewport state after restore

**Expected Behavior**:
- ✅ Content from before minimize is still visible
- ✅ No clearing or flicker during restore
- ⚠️ Note: Some window managers may clear minimized windows; this is acceptable as it's out of application control

#### Test E.4: Aspect Ratio Change (Widescreen ↔ Portrait)

**Steps**:
1. Start with widescreen aspect ratio (e.g., 16:9)
2. Resize dramatically to change to tall/portrait aspect ratio (e.g., 9:16)
3. Observe during the dramatic aspect ratio change

**Expected Behavior**:
- ✅ Content remains visible during aspect ratio change
- ✅ Content scales/stretches temporarily but does not clear
- ✅ New preview updates to correct aspect ratio after resize completes

---

## Success Criteria Verification

After completing all tests, verify against specification success criteria:

| Criteria ID | Description | Pass/Fail |
|-------------|-------------|-----------|
| **SC-001** | Resize in any direction without clearing/flickering | ☐ |
| **SC-002** | Content stable during 100% of resize operations | ☐ |
| **SC-003** | Zero instances of clearing during testing | ☐ |
| **SC-004** | No visual discontinuity or blank frames | ☐ |
| **SC-005** | Improved perceived quality (subjective assessment) | ☐ |
| **SC-006** | Resize handling completes in <16ms per frame | ☐* |

*SC-006 Note: Performance can be measured using Tracy profiler if available, or inferred from smooth 60fps visual experience.

## Troubleshooting

### If Flicker Still Occurs

1. **Check build**: Ensure the resize fix branch is actually built and running
   ```bash
   git branch  # Should show '001-fix-resize-flicker'
   git log -1  # Should show recent resize fix commit
   ```

2. **Verify fix is active**: Add temporary debug logging to resize detection code to confirm code path is executed

3. **Graphics driver issues**: Try with different OpenGL backends or update graphics drivers

4. **Window manager interference**: Some compositors (e.g., Compiz, KWin effects) may introduce flicker independent of application

### Performance Issues

If resize operations are sluggish or drop below 60fps:

1. Check CPU/GPU utilization during resize
2. Verify async rendering is enabled (check config)
3. Profile with Tracy to identify bottlenecks
4. Check if low-res preview resolution is appropriate (should be < 50% of full resolution)

### Known Limitations

1. **Multi-monitor DPI transitions**: Tested on single-monitor setup. Multi-monitor behavior with different DPI verified via code inspection but not manual testing.

2. **Minimize/restore**: Window manager may clear minimized windows (out of application control).

3. **Very small viewports**: Minimum dimension clamped to 1px. Below this, window manager constraints apply.

4. **Extreme rapid resize**: Content may appear stretched/scaled during continuous drag but never clears to blank.

## Recording Test Results

For each test case, record:

- **Test ID**: (e.g., Test 1.1)
- **Pass/Fail**: ✅ or ❌
- **Notes**: Any observations, edge cases, or issues
- **Environment**: GPU model, driver version, window manager (if relevant)

Example:
```
Test 1.1: ✅ PASS
Notes: Resize smooth, no flicker observed. Content stretched during drag then updated correctly.
Environment: NVIDIA RTX 3070, driver 525.60.11, GNOME 42
```

## Regression Testing

After fix is merged, add these test scenarios to the regular manual QA checklist for future releases to prevent regression.

---

**Questions or Issues?**  
If you encounter problems or unexpected behavior, document with:
- Exact steps to reproduce
- Screenshots or screen recording if possible
- GPU/driver information
- Window manager details

Report in the feature branch PR or project issue tracker.
