# Data Model: Node Editor UX Improvements (023)

## Entities

### 1. PortPresentationState

Represents how a visible port behaves and appears during normal interaction and link creation.

| Field | Type | Description |
|-------|------|-------------|
| `portId` | Identifier | Stable identifier for the visible port |
| `direction` | Input / Output | Whether the port receives or emits links |
| `resolvedType` | Type reference | Current data type used for compatibility decisions |
| `labelMode` | Inline / TooltipOnly | Whether name/type is shown inline or only on hover |
| `visualState` | Normal / Highlighted / Dimmed | Current interaction-driven visual state |
| `hitTargetProfile` | Shared profile reference | Shared sizing/hover rules used across all node types |

**Validation rules**:
- `resolvedType` may be unresolved only for ports explicitly allowed to defer type resolution.
- `hitTargetProfile` must match the shared pin interaction rules used by other node types.

### 2. LinkDragSession

Captures the active drag operation used to evaluate and render compatibility feedback.

| Field | Type | Description |
|-------|------|-------------|
| `sourcePortId` | Identifier | Port where the drag originated |
| `sourceDirection` | Input / Output | Direction of the source port |
| `sourceResolvedType` | Type reference | Current source type for compatibility evaluation |
| `candidateSet` | Collection of identifiers | Ports that are compatible for the active drag |
| `status` | Idle / Dragging / Cancelled / Dropped | Lifecycle of the interaction |

**State transitions**:
- `Idle → Dragging` when the user begins a valid drag from a port
- `Dragging → Dropped` when the user releases on a compatible target
- `Dragging → Cancelled` when the gesture is aborted
- `Dropped/Cancelled → Idle` after cleanup

### 3. NumericParameterPresentation

Defines the UI presentation and interaction rules for an inline numeric parameter editor.

| Field | Type | Description |
|-------|------|-------------|
| `parameterId` | Identifier | Stable parameter identifier |
| `contentType` | Domain type | Length, angle, scalar, vector component, etc. |
| `layoutMode` | DialPlusDragFloat / Slider | Chosen presentation mode |
| `boundsMode` | Bounded / Unbounded | Whether min/max are defined |
| `currentValue` | Numeric value | Current displayed/editable value |
| `sensitivityProfile` | Fine/Default/Coarse rules | Drag and keyboard adjustment behavior |

**Validation rules**:
- Bounded parameters require both lower and upper limits.
- Dial and drag-float views must remain synchronized for the same parameter.

### 4. CompactNodePresentation

Represents the compact visual layout applied to simple nodes.

| Field | Type | Description |
|-------|------|-------------|
| `nodeId` | Identifier | Stable node identifier |
| `shapeProfile` | Rounded / Capsule | Selected compact body style |
| `categoryColor` | Visual token | Border/ring color derived from node category |
| `centerContent` | Title / Glyph / Title+Glyph | Primary visual identifier shown in the center |
| `leftRailPorts` | Ordered list of PortPresentationState | Input ports shown on the left rail |
| `rightRailPorts` | Ordered list of PortPresentationState | Output ports shown on the right rail |
| `fallbackMode` | Compact / Expanded | Whether the node must expand to preserve usability |

**Validation rules**:
- Compact layout must never reduce hit targets below the shared port interaction minimum.
- `fallbackMode` switches to expanded when content cannot fit without clipping or overlap.

### 5. SignatureRow

Represents a single editable entry in a begin/end node signature.

| Field | Type | Description |
|-------|------|-------------|
| `rowId` | Identifier | Stable row identity |
| `name` | Text | User-visible argument/output name |
| `type` | Type reference | Declared type |
| `orderIndex` | Integer | Visual and semantic order |
| `connectionState` | Connected / Unconnected | Whether links currently reference the row |
| `pendingAction` | None / Rename / Remove / Reorder | Current user action being resolved |

**Validation rules**:
- Names must remain non-empty.
- Reorder operations must preserve link attachment semantics.

### 6. RecompileCoalescingState

Represents the boundary between immediate UI edits and deferred expensive recompute work.

| Field | Type | Description |
|-------|------|-------------|
| `latestEditTimestamp` | Time value | Timestamp of the most recent parameter edit |
| `pendingRecompile` | Boolean | Whether a recompute is queued |
| `coalescingWindow` | Duration | Minimum delay before expensive work is retriggered |
| `latestAuthoritativeValue` | Value snapshot | Most recent user-facing parameter value |

**Validation rules**:
- UI-visible values update immediately even while recompute is deferred.
- Only the latest pending value may trigger the next expensive recompute.

## Relationships

```text
CompactNodePresentation
├── leftRailPorts  -> PortPresentationState[*]
├── rightRailPorts -> PortPresentationState[*]
└── centerContent

LinkDragSession
├── sourcePortId -> PortPresentationState
└── candidateSet -> PortPresentationState[*]

NumericParameterPresentation
└── feeds -> RecompileCoalescingState

Begin/End node
└── owns -> SignatureRow[*]
```

## Key Interaction Flows

1. **Link creation flow**
    - User begins drag from a `PortPresentationState`
    - `LinkDragSession` is created
    - Other ports move into highlighted/dimmed states based on compatibility
    - Drop or cancel returns all ports to `Normal`

2. **Numeric edit flow**
    - User edits a `NumericParameterPresentation`
    - Value changes immediately in the UI
    - `RecompileCoalescingState` marks the model dirty and delays expensive recompute until the coalescing boundary is met

3. **Compact-node fallback flow**
    - `CompactNodePresentation` attempts compact layout
    - If content cannot fit without clipping or shrinking hit targets, `fallbackMode` becomes `Expanded`

4. **Signature editing flow**
    - User edits a `SignatureRow`
    - Row action resolves inline (rename, remove, reorder)
    - Begin/end node and connected links update while preserving semantic order
