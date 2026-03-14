# Quickstart: Node Editor UX Improvements (023)

## Preconditions

- Open branch `023-node-editor-ux`
- Open the Gladius workspace in VS Code
- Use the Linux release-with-debug configuration already defined in workspace tasks

## Build and test workflow

1. Run the VS Code task **`Build ALL (linux-releaseWithDebug)`**.
2. Run the VS Code task **`Run Gladius Tests (linux-releaseWithDebug)`**.
3. If failures occur, inspect them before continuing with visual review.

## Manual verification sequence

### 1. Shared port behavior

- Open a graph containing both compact nodes and regular nodes.
- Verify that every visible port has the same apparent size and hover feel.
- Start link drags from inputs and outputs on both node styles.
- Confirm drag starts reliably from the whole port, not from a tiny sub-region.

### 2. Link compatibility highlighting

- Start a drag from a typed output port.
- Confirm compatible targets highlight immediately.
- Confirm incompatible targets dim.
- Cancel with Escape and verify all ports return to normal.

### 3. Compact node presentation

- Open a function with several simple operator nodes and several larger nodes.
- Verify compact nodes appear rounded/capsule-like with left/right pin rails.
- Confirm no ports, labels, or embedded widgets are clipped.
- Confirm link anchors visually meet the ports consistently.

### 4. Numeric editing

- Edit scalar and vector parameters inline.
- Verify drag, keyboard arrows, and direct entry behave consistently.
- Verify dial + drag-float stay in sync.
- Verify bounded and unbounded parameters behave correctly.

### 5. Begin/end node usability

- Add, rename, remove, and reorder arguments/outputs.
- Verify rows remain readable and links remain coherent after reordering.

### 6. Responsiveness

- Rapidly drag a parameter on a non-trivial model.
- Confirm the node editor remains interactive while preview updates continue.
- Confirm the latest parameter value wins even if intermediate recomputes are skipped.

## Evidence to capture during review

- Screenshots showing compact and regular nodes side by side
- A short note confirming port drag reliability across node styles
- A short note confirming no clipping or anchor misalignment at default zoom
- Test results from the standard workspace test task
