# Quickstart: Image3D & FunctionFromImage3D UI

**Date**: 2026-01-26  
**Branch**: `016-image3d-ui`

## Prerequisites

- Gladius built with `Build ALL (linux-releaseWithDebug)` task
- Sample 3MF file with ImageStack/FunctionFromImage3D or PNG image directory

## User Workflows

### 1. View ImageStack Layers

1. Open a 3MF file containing an ImageStack
2. In the **Outline**, expand the **Resources** section
3. Click on an **Image3D** entry
4. The **ImageStack Viewer** panel opens showing layer 1
5. Use the **slider** or **mouse wheel** to navigate layers
6. Layer indicator shows "Layer N of M"

### 2. Configure FunctionFromImage3D

1. Open a 3MF file containing a FunctionFromImage3D
2. In the **Outline**, click on a **FunctionFromImage3D** entry
3. The editor shows two tabs: **Properties** and **Graph**
4. In **Properties** tab:
   - **Filter**: Dropdown with "Linear" / "Nearest"
   - **Tile Style U/V/W**: Three dropdowns with "Wrap" / "Mirror" / "Clamp"
   - **Image Stack**: Dropdown showing available ImageStacks
   - **Offset/Scale**: Numeric inputs for value transformation
5. Changes apply immediately to the 3D preview

### 3. Preview FunctionFromImage3D Output

1. With FunctionFromImage3D selected, view the **2D Slice Preview**
2. Use the **slice position slider** to move through the volume
3. Extend slider beyond [0,1] to see tile behavior
4. Preview updates within 500ms of any configuration change

### 4. Import ImageStack from Directory

1. From menu: **File → Import → Image Stack from Directory...**
2. Select folder containing PNG images
3. Files are sorted by filename (e.g., `layer_001.png`, `layer_002.png`)
4. If images have different sizes, smaller images are padded
5. New ImageStack appears in **Outline → Resources**

### 5. Transform ImageStack

1. Select an ImageStack in the Outline
2. In the **ImageStack Viewer** panel toolbar:
   - **Flip H** - Mirror horizontally
   - **Flip V** - Mirror vertically  
   - **Rotate CW** - Rotate 90° clockwise
   - **Rotate CCW** - Rotate 90° counter-clockwise
3. All transforms support **Undo** (Ctrl+Z)

### 6. Create FunctionFromImage3D

1. Select an ImageStack in the Outline
2. Right-click → **Create FunctionFromImage3D**
3. New function created with defaults:
   - Filter: Linear
   - Tile Style: Wrap (all axes)
   - Offset: 0, Scale: 1
4. Function appears in Outline and is auto-selected

## Keyboard Shortcuts

| Action | Shortcut |
|--------|----------|
| Undo | Ctrl+Z |
| Redo | Ctrl+Y / Ctrl+Shift+Z |
| Navigate layers (when ImageStack viewer focused) | Arrow Up/Down |
| Switch to Properties tab | Ctrl+1 |
| Switch to Graph tab | Ctrl+2 |

## Troubleshooting

### "Referenced ImageStack not found" error
The FunctionFromImage3D points to a deleted or missing ImageStack. Use the ImageStack selector dropdown to choose a valid resource.

### Layer navigation is slow for large stacks
For ImageStacks with 1000+ layers or 4K images, initial loading may take a few seconds. A loading indicator is shown; navigation remains responsive once loaded.

### Transforms don't appear to apply
Ensure changes are saved (File → Save). Transforms modify the in-memory ImageStack; unsaved changes are lost on close.

## API Reference

### ImageStack Transforms

```cpp
// Flip all layers horizontally
imageStack.flipHorizontal();

// Flip all layers vertically
imageStack.flipVertical();

// Rotate all layers 90° clockwise
imageStack.rotate90CW();
```

### FunctionFromImage3D Configuration

```cpp
// Get the ImageSampler node from a function
auto* sampler = findImageSampler(function);

// Set filter mode
sampler->parameter().at(FieldNames::Filter) = VariantParameter(static_cast<int>(SF_LINEAR));

// Set tile style for U axis
sampler->parameter().at(FieldNames::TileStyleU) = VariantParameter(static_cast<int>(TTS_CLAMP));
```
