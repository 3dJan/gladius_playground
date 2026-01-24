# Quickstart: Testing FunctionCall Node Navigation

**Date**: 2026-01-24  
**Branch**: `013-func-call-nav`

## Build

```bash
# Use VS Code task (preferred)
Ctrl+Shift+B → "Build ALL (linux-releaseWithDebug)"

# Or via CMake preset
cd gladius && cmake --build --preset linux-releaseWithDebug
```

## Manual Testing Procedure

### Prerequisites

1. Launch Gladius
2. Open or create a 3MF file with at least two functions (e.g., a main function that calls a helper function)

### Test 1: Double-Click Navigation (FR-001, FR-002)

1. In the node editor, locate a **FunctionCall** node (shows function reference)
2. **Double-click** on the node's background area (not on input fields)
3. **Expected**: Editor navigates to the referenced function

### Test 2: No Navigation on Input Fields (FR-003)

1. Locate a **FunctionCall** node with an input field
2. **Double-click** on the input field itself
3. **Expected**: Text selection/editing activates, NO navigation occurs

### Test 3: Mouse Back Button (FR-005)

1. Navigate to a function via double-click (Test 1)
2. Press the **mouse back button** (X1, typically on side of mouse)
3. **Expected**: Editor returns to the previous function

### Test 4: Mouse Forward Button (FR-006)

1. After going back (Test 3), press **mouse forward button** (X2)
2. **Expected**: Editor returns to the function you navigated away from

### Test 5: History Truncation (FR-007)

1. Navigate: Function A → Function B → Function C
2. Go back twice to Function A
3. Navigate to Function D (new navigation)
4. Try to go forward
5. **Expected**: Forward button does nothing (history was truncated)

### Test 6: Invalid Target Function (FR-008)

1. If a FunctionCall references a non-existent function (corrupted file or deleted function)
2. Double-click on it
3. **Expected**: No crash, no navigation, silent failure

### Test 7: FunctionGradient Node (FR-009)

1. Locate a **FunctionGradient** node (if available)
2. Double-click on its background area
3. **Expected**: Navigates to the referenced function (same as FunctionCall)

## Sample 3MF Files for Testing

The following example files contain nested function calls:

- `gladius/examples/implicit/gyroid.3mf`
- Any 3MF with custom functions (File → New → Add Function)

## Verification Checklist

| # | Test | Pass? |
|---|------|-------|
| 1 | Double-click navigates to function | ☐ |
| 2 | Input fields don't trigger navigation | ☐ |
| 3 | Mouse back button works | ☐ |
| 4 | Mouse forward button works | ☐ |
| 5 | History truncates on new navigation | ☐ |
| 6 | Invalid target fails silently | ☐ |
| 7 | FunctionGradient nodes work | ☐ |
