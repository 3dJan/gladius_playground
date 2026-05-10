# Quickstart: Export UI Lock

**Feature**: 008-export-ui-lock  
**Date**: 2025-01-06

## What This Feature Does

When exporting a mesh (any format, any extraction method), the Model Editor displays a semi-transparent overlay that:
1. Blocks all model-modifying interactions (parameter changes, node creation/deletion)
2. Shows a clear "Export in progress..." message
3. Automatically disappears when export completes

## Verification Steps

### Test 1: Overlay Appears During Export
1. Open Gladius with any model containing geometry
2. Open the Mesh Export dialog (File → Export Mesh or equivalent)
3. Start an export (choose any format/settings)
4. **Expected**: Semi-transparent dark overlay appears over the Model Editor
5. **Expected**: Text "Export in progress..." is centered on the overlay
6. Wait for export to complete
7. **Expected**: Overlay disappears immediately

### Test 2: Parameter Editing Blocked
1. Open a model with numeric parameters visible in the node editor
2. Start an export
3. While export is in progress, attempt to:
   - Drag a parameter slider
   - Type in a parameter input field
   - Change a dropdown selection
4. **Expected**: All inputs appear grayed out and do not respond
5. Wait for export to complete
6. **Expected**: Parameters become editable again

### Test 3: Node Operations Blocked
1. Open a model in the node editor
2. Start an export
3. While export is in progress, attempt to:
   - Right-click to create a new node
   - Select and delete a node (Del key)
   - Create a new link between nodes
4. **Expected**: All operations are silently blocked
5. Wait for export to complete
6. **Expected**: Node operations work normally again

### Test 4: File Operations Blocked
1. Start an export
2. While export is in progress, check the File menu:
   - New
   - Open
   - Import functions
3. **Expected**: These menu items are grayed out / disabled
4. Wait for export to complete
5. **Expected**: Menu items are enabled again

### Test 5: Viewport Navigation Allowed
1. Start an export
2. While export is in progress, attempt to:
   - Pan the node editor canvas
   - Zoom in/out on the node editor
   - Pan/rotate the 3D viewport
3. **Expected**: All navigation works normally (non-destructive operations allowed)

### Test 6: Multiple Export Formats
Repeat Test 1 with:
- STL export
- 3MF export
- Different mesh extraction methods (MDC, HDC if available)

**Expected**: Overlay appears consistently for all export types.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                     MainWindow                          │
│  ┌───────────────┐                                      │
│  │  ExportState  │◄────────────────┐                    │
│  │  (atomic)     │                 │                    │
│  └───────┬───────┘                 │                    │
│          │                         │                    │
│          ▼                         │                    │
│  ┌───────────────┐         ┌───────┴───────┐            │
│  │  ModelEditor  │         │  MeshExporter │            │
│  │  (disabled)   │         │  (bg thread)  │            │
│  │               │         └───────────────┘            │
│  │  ┌─────────┐  │                                      │
│  │  │NodeView │  │                                      │
│  │  │(disabled)  │                                      │
│  │  └─────────┘  │                                      │
│  │               │                                      │
│  └───────────────┘                                      │
│                                                         │
│  ┌─────────────────────────────────────────────────┐    │
│  │     FULLSCREEN OVERLAY (60% opacity black)      │    │
│  │         "Export in progress..."                 │    │
│  └─────────────────────────────────────────────────┘    │
│                                                         │
│  ┌───────────────┐  ◄── Export dialog appears on top   │
│  │Export Progress│                                      │
│  │  [####    ]   │                                      │
│  │  [Cancel]     │                                      │
│  └───────────────┘                                      │
└─────────────────────────────────────────────────────────┘
```

## Files Modified

| File | Changes |
|------|---------|
| `MainWindow.cpp` | Add fullscreen overlay rendering, export state checks |
| `MainWindow.h` | Add `renderExportOverlay()` method |
| `NodeView.cpp` | Add export state checks to input handlers |
| `NodeView.h` | Add `ExportState*` member and setter |
| `ExportState_tests.cpp` | New unit tests |

## Building and Testing

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run all tests
# Use VS Code task: "Run Gladius Tests (linux-releaseWithDebug)"

# Manual verification
# Run Gladius and follow the verification steps above
```
