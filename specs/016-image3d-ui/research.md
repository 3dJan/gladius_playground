# Research: Image3D & FunctionFromImage3D UI

**Date**: 2026-01-26  
**Branch**: `016-image3d-ui`

## Decision Summary

| Item | Decision | Rationale | Alternatives Considered |
|------|----------|-----------|-------------------------|
| Panel Integration | Tabbed interface in existing ModelEditor area | Consistent with existing UI, preserves graph access | Replace vs. side-by-side |
| Preview Rendering | ImGui Image from CPU-sampled texture | Simple, existing SliceView pattern | GPU rendering |
| Layer Navigation | Slider + mouse wheel | Standard patterns, matches existing slice navigation | Keyboard-only |
| Undo System | Extend existing History class | Consistent with existing undo for node edits | Separate undo stack |
| Transform Implementation | In-place modification of ImageStack::Image | Direct, efficient, minimal memory overhead | Copy-on-write |

## Technical Research

### 1. Existing UI Architecture

**ImGui-based**: All UI uses Dear ImGui with immediate-mode rendering.

**Key Components**:
- `ModelEditor` - Central class managing function editing, navigation, undo
- `NodeView` - Graph/node visualization for functions
- `Outline` - Tree view of resources and objects (currently basic)
- `SliceView` - 2D slice visualization with zoom/pan, good reference for layer viewer

**Selection Pattern**: Selection in the Outline triggers updates to `ModelEditor::m_currentModel` which controls what NodeView displays.

### 2. Existing ImageStack Infrastructure

**Data Model** ([ImageStack.h](gladius/src/io/3mf/ImageStack.h)):
- `ImageStack` contains `std::vector<Image>`
- `Image` contains raw pixel data, dimensions, format (8/16-bit, RGB/RGBA/grayscale)
- `swapXYData()` method exists on Image - provides pattern for transforms

**Import Flow** ([ImageStackCreator.cpp](gladius/src/io/3mf/ImageStackCreator.cpp)):
- `addImageStackFromDirectory()` - creates ImageStack from PNG directory
- `importDirectoryAsFunctionFromImage3D()` - creates both ImageStack and FunctionFromImage3D
- Already handles file ordering by filename

### 3. FunctionFromImage3D Implementation

**Import** ([Importer3mf.cpp](gladius/src/io/3mf/Importer3mf.cpp)):
- `processFunctionFromImage3d()` extracts settings from Lib3MF and calls Builder

**Internal Representation** ([Builder.cpp](gladius/src/nodes/Builder.cpp#L545)):
- `createFunctionFromImage3D()` creates a node graph:
  - `ImageSampler` node with filter/tilestyle parameters
  - `Resource` node referencing the ImageStack
  - Scale/Offset nodes for value transformation
  - Begin/End nodes with standard outputs (color, r, g, b, alpha)

**Key Settings** (`SamplingSettings` struct):
- `filter`: SF_LINEAR or SF_NEAREST
- `tileStyleU/V/W`: TTS_WRAP, TTS_MIRROR, TTS_CLAMP
- `offset`, `scale`: float values

### 4. Undo System ([ModelEditor.cpp](gladius/src/ui/ModelEditor.cpp#L2340))

- Uses `History` class that stores assembly snapshots
- `createUndoRestorePoint(description)` - captures state before modification
- Can be extended to cover ImageStack transforms by storing ImageStack state

### 5. Resource ID Management

Resources are identified by `ResourceId` (typedef for `uint32_t`).
`Assembly` class manages models and resources by ID.

## Implementation Approach

### Phase 1: ImageStack Viewer (P1)

1. Create `ImageStackView` class following `SliceView` pattern
2. Extend `ResourceView` or create dedicated panel
3. Render current layer as ImGui texture
4. Slider widget for layer navigation
5. Mouse wheel handler for scrolling

### Phase 2: FunctionFromImage3D Properties Panel (P1)

1. Create `FunctionFromImage3DView` class
2. Add tab switching to ModelEditor when FunctionFromImage3D is selected
3. Controls for filter (combo), tilestyle (3x combo), offset/scale (drag float)
4. Changes update `ImageSampler` node parameters directly

### Phase 3: ImageStack Selector (P2)

1. Enumerate ImageStack resources from Assembly
2. Dropdown with resource ID and optional preview thumbnail
3. Update `Resource` node in function graph when selection changes

### Phase 4: 2D Slice Preview (P2)

1. Sample FunctionFromImage3D at slice position via CPU evaluation
2. Convert to RGBA texture
3. Display in ImGui::Image widget
4. Slider for slice position (with option to go outside [0,1])

### Phase 5: Import Enhancement (P2)

1. Extend `ImageStackCreator` with dimension normalization
2. Add padding logic for differing image sizes
3. Progress callback for large imports

### Phase 6: Transforms (P3)

1. Add `flipHorizontal()`, `flipVertical()`, `rotate90CW()` to `ImageStack`
2. Integrate with undo system
3. UI buttons in ImageStackView

### Phase 7: Creation Workflow (P3)

1. Context menu on ImageStack in Outline
2. Call existing `importDirectoryAsFunctionFromImage3D()`
3. Default parameters pre-filled in Properties panel
