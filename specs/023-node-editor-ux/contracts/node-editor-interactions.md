# Contract: Node Editor Interaction Flows

## 1. Link-drag interaction flow

### Trigger

User begins dragging from any visible port.

### Guarantees

- Compatibility feedback appears within one frame.
- Compatible targets are visually distinct from incompatible targets.
- Cancelling the drag restores the idle visual state with no lingering artifacts.
- Successful drop behavior is identical regardless of node style.

## 2. Compact-node presentation flow

### Trigger

The editor renders a node eligible for compact presentation.

### Guarantees

- Compact layout uses shared left/right pin rails.
- Compact layout never reduces pin hit targets below the shared minimum.
- If the content does not fit cleanly, the node expands or falls back rather than clipping.
- Link anchors remain visually aligned with rendered pins.

## 3. Begin/end signature editing flow

### Trigger

User edits the signature of a begin or end node.

### Guarantees

- Add, rename, remove, and reorder actions are available inline.
- Removing connected signature rows requires an explicit confirmation step.
- Reordering preserves link attachment semantics.
- Begin/end nodes remain visually distinct from standard computational nodes.

## 4. Responsiveness flow

### Trigger

User performs rapid numeric edits that would otherwise trigger repeated recompute work.

### Guarantees

- Widget feedback remains immediate.
- Expensive recompute requests are coalesced so the latest value wins.
- The node editor remains interactive while deferred work completes.