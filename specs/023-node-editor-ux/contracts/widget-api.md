# Contract: Numeric Widget and Pin Behavior

This feature does not expose a REST or GraphQL API. The contracts below define the user-facing behavior and component boundaries that implementations must preserve.

## 1. Numeric widget contract

### Inputs

- Parameter identity
- Current numeric value
- Content type (for formatting and sensitivity rules)
- Optional bounds
- Layout mode (`DialPlusDragFloat` or `Slider`)

### Required behavior

- The widget must allow immediate inline editing.
- In `DialPlusDragFloat` mode, the dial and drag-float must always represent the same underlying value.
- In `Slider` mode, the widget must still preserve keyboard accessibility and current-value visibility.
- Modifier keys must affect sensitivity consistently across all numeric widgets.
- Double-click or equivalent direct-entry affordance must allow precise typed values.

### Outputs

- Updated numeric value
- Immediate UI-visible feedback for the latest value
- A parameter-change event that can be coalesced before expensive recompute work

## 2. Shared pin interaction contract

### Inputs

- Port identity
- Port direction
- Resolved port type
- Label mode (inline or tooltip-only)
- Current link-drag session, if any

### Required behavior

- All pins must share the same minimum hit target size.
- All pins must support the same hover, click, and drag-start behavior.
- Compact nodes and regular nodes must not diverge in drag reliability.
- Hover feedback and tooltips must respect whether the node uses inline labels or tooltip-only labels.
- During link drag, pins must move into highlighted or dimmed states based on compatibility.

### Outputs

- Pin interaction events (hover, click, drag start)
- Visual state transitions (`Normal`, `Highlighted`, `Dimmed`)
- A stable anchor point for the rendered link geometry
