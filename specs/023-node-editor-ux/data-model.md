# Data Model: Node Editor UX Improvements (023)

## Entities

### 1. NumericWidget (new)

Encapsulates the enhanced numeric input UI state for a single parameter.

| Field | Type | Description |
|-------|------|-------------|
| `value` | `float*` | Pointer to the underlying parameter value |
| `contentType` | `ContentType` | Existing enum: `Length`, `Angle`, default |
| `layoutMode` | `WidgetLayoutMode` | `DialPlusDragFloat` (default) or `Slider` |
| `minValue` | `std::optional<float>` | Min bound (if bounded) |
| `maxValue` | `std::optional<float>` | Max bound (if bounded) |
| `isBounded` | `bool` | Whether the parameter has min/max |
| `accumulatedAngle` | `float` | For unbounded dials: cumulative rotation angle |

**Validation**: `layoutMode` must be one of the two supported values. If `isBounded`, both `minValue` and `maxValue` must be set.

### 2. WidgetLayoutMode (new enum)

```cpp
enum class WidgetLayoutMode
{
    DialPlusDragFloat, ///< Orbital dial paired with drag-float (default)
    Slider             ///< Linear slider with value label
};
```

Persisted per parameter in the document's node parameter metadata.

### 3. OrbitalDialState (new)

Transient per-frame state for the orbital dial interaction.

| Field | Type | Description |
|-------|------|-------------|
| `isActive` | `bool` | Whether user is currently dragging the dial |
| `dragStartAngle` | `float` | Angle at drag start (radians) |
| `currentAngle` | `float` | Current visual angle |
| `centerPos` | `ImVec2` | Screen-space center of the dial |
| `radius` | `float` | Dial radius in pixels |

### 4. LinkDragState (new / extends existing)

Tracks compatibility information during link creation.

| Field | Type | Description |
|-------|------|-------------|
| `isDragging` | `bool` | Whether a link drag is in progress |
| `sourcePortId` | `PortId` | The port being dragged from |
| `sourcePortType` | `std::type_index` | Resolved type of source port |
| `sourceIsOutput` | `bool` | Whether source is an output port |
| `compatiblePorts` | `std::unordered_set<int64_t>` | Set of compatible port/parameter IDs |

**State transitions**: `idle` → `dragging` (on drag start) → `idle` (on drop/cancel). Compatibility set is computed once on transition to `dragging`, and refreshed if `sourcePortId` changes.

### 5. NodeRenderStyle (modified)

Extends existing `NodeStyle` in `Style.h`.

| Field | Type | Description |
|-------|------|-------------|
| `color` | `ImColor` | Existing: node border/ring color |
| `activeColor` | `ImColor` | Existing: color when selected |
| `hoveredColor` | `ImColor` | Existing: color when hovered |
| `borderWidth` | `float` | New: ring/border thickness (default ~4.0) |
| `rounding` | `float` | New: corner rounding radius (default ~20.0) |
| `showCategoryIcon` | `bool` | New: whether to show an icon glyph |

### 6. ParameterThrottle (new)

Debounce controller for parameter-to-recompile pipeline.

| Field | Type | Description |
|-------|------|-------------|
| `lastChangeTime` | `std::chrono::steady_clock::time_point` | When the last parameter change occurred |
| `debounceInterval` | `std::chrono::milliseconds` | Default 100ms |
| `pendingRecompile` | `bool` | Whether a recompile is waiting |
| `latestValue` | `float` | The most recent parameter value (to send on recompile) |

### 7. BeginEndNodeUI (modified)

Enhanced UI state for begin/end node argument management.

| Field | Type | Description |
|-------|------|-------------|
| `arguments` | `std::vector<ArgumentEntry>` | Ordered list of function arguments |
| `dragSourceIndex` | `std::optional<int>` | Index of argument being reordered |
| `dragTargetIndex` | `std::optional<int>` | Drop target index for reordering |
| `pendingRemoval` | `std::optional<int>` | Argument index pending removal with confirmation |
| `showConfirmDialog` | `bool` | Whether the removal confirmation dialog is shown |

### 8. ArgumentEntry (new)

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Argument name (editable) |
| `typeIndex` | `std::type_index` | Data type of the argument |
| `portId` | `PortId` | Associated port/pin ID |
| `hasConnectedLinks` | `bool` | Whether any links are connected to this port |

## Relationships

```
NodeBase 1──* ParameterMap (existing) ── enhanced with NumericWidget state
NodeBase ──> NodeRenderStyle (via type → style lookup)
ModelEditor 1──1 LinkDragState
Begin/End 1──* ArgumentEntry (ordered)
MainWindow 1──1 ParameterThrottle
NumericWidget 1──1 OrbitalDialState (transient, during interaction)
```

## Key Interactions

1. **Parameter editing flow**: User interacts with NumericWidget → value updates immediately → ParameterThrottle debounces → recompile triggers on throttle expiry
2. **Link drag flow**: User starts drag → LinkDragState computed → ModelEditor sets compatible ports → NodeView renders pin highlight states → drop validates compatibility
3. **Argument reorder flow**: User drags argument row → BeginEndNodeUI tracks drag indices → on drop, Model reorders ports → connected links follow
