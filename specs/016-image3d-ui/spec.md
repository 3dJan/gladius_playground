# Feature Specification: Image3D & FunctionFromImage3D UI

**Feature Branch**: `016-image3d-ui`  
**Created**: 2026-01-26  
**Status**: Draft  
**Input**: User description: "Extend UI capabilities for FunctionFromImage3D and ImageStack: create/view/edit ImageStacks with layer scrolling, basic transforms (flip/rotate), dedicated FunctionFromImage3D editor with filter/tiling config, preview, and ImageStack selector accessible from Outline click"

## Clarifications

### Session 2026-01-26

- Q: When the user clicks a FunctionFromImage3D in the Outline, how should the configuration panel relate to the existing graph editor? → A: Tabbed view (user can switch between 'Properties' tab and 'Graph' tab)
- Q: What type of preview should the FunctionFromImage3D panel display? → A: 2D slice viewer (shows a cross-section at a user-controlled position)
- Q: Should transform operations (flip/rotate) and configuration changes support undo? → A: Full undo support (all operations undoable via Ctrl+Z)
- Q: When importing a directory of PNGs with different image sizes, what should happen? → A: Use largest size (pad smaller images to match largest dimensions)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - View and Navigate ImageStack Layers (Priority: P1)

A user working with volumetric data needs to inspect individual layers of an ImageStack to understand the 3D data structure. They select an ImageStack resource and see a 2D view of a single layer with a slider or scroll mechanism to navigate through all layers (Z-axis).

**Why this priority**: Viewing is the most fundamental operation - users cannot effectively work with ImageStacks without being able to see their contents. This enables all subsequent editing and configuration workflows.

**Independent Test**: Can be fully tested by loading a 3MF file containing an ImageStack, selecting it in the Outline, and verifying the layer viewer displays correctly with navigation controls.

**Acceptance Scenarios**:

1. **Given** a document with an ImageStack resource, **When** the user selects the ImageStack in the Outline, **Then** a 2D image viewer displays the first layer with a slider showing current layer position (e.g., "Layer 1 of 50")
2. **Given** the ImageStack viewer is open, **When** the user moves the layer slider, **Then** the displayed image updates to show the selected layer in real-time
3. **Given** the ImageStack viewer is open, **When** the user scrolls with the mouse wheel over the image, **Then** the layer advances/retreats through the stack
4. **Given** an ImageStack with varying layer dimensions, **When** displayed, **Then** the viewer scales appropriately to show the full layer without cropping

---

### User Story 2 - Configure FunctionFromImage3D Settings (Priority: P1)

A user has a FunctionFromImage3D that samples from an ImageStack. They need to configure sampling behavior (filter mode, tile styles) through an intuitive UI rather than editing raw graph nodes. Clicking the FunctionFromImage3D in the Outline opens a dedicated configuration panel.

**Why this priority**: This is the core differentiator from the current graph-based editing. Users need direct access to the most commonly adjusted parameters without navigating a complex node graph.

**Independent Test**: Can be fully tested by selecting a FunctionFromImage3D in the Outline and verifying all configuration options are accessible and changes persist correctly.

**Acceptance Scenarios**:

1. **Given** a document with a FunctionFromImage3D, **When** the user clicks it in the Outline, **Then** a tabbed interface appears with 'Properties' and 'Graph' tabs, with 'Properties' tab showing the configuration panel
2. **Given** the FunctionFromImage3D panel is open, **When** the user selects a filter mode (Linear/Nearest), **Then** the setting is applied to the underlying function immediately
3. **Given** the FunctionFromImage3D panel is open, **When** the user configures tile style for U/V/W axes (Wrap/Mirror/Clamp), **Then** each axis can be configured independently
4. **Given** the user changes any setting, **When** viewing the 3D preview, **Then** the preview updates to reflect the new configuration

---

### User Story 3 - Select ImageStack for FunctionFromImage3D (Priority: P2)

A user creating or modifying a FunctionFromImage3D needs to easily select which ImageStack resource it references. The UI provides a visual selector showing available ImageStacks with thumbnails.

**Why this priority**: After viewing and basic configuration, users need the ability to connect functions to their data sources. This completes the essential workflow.

**Independent Test**: Can be fully tested by opening a FunctionFromImage3D panel and using the selector to change the referenced ImageStack, verifying the preview updates.

**Acceptance Scenarios**:

1. **Given** a document with multiple ImageStack resources, **When** the user opens the FunctionFromImage3D panel, **Then** a dropdown or visual picker shows all available ImageStacks
2. **Given** the ImageStack selector is visible, **When** the user hovers over an ImageStack option, **Then** a tooltip or preview shows a representative layer from that stack
3. **Given** the user selects a different ImageStack, **When** confirmed, **Then** the FunctionFromImage3D updates its image3dID reference
4. **Given** an empty document with no ImageStacks, **When** viewing the selector, **Then** a helpful message indicates no ImageStacks are available (creation via US5 import workflow)

---

### User Story 4 - Preview FunctionFromImage3D Output (Priority: P2)

A user wants to see how their FunctionFromImage3D configuration affects the output before applying it to geometry. A live preview shows a sample slice or volume rendering of the function output.

**Why this priority**: Visual feedback is essential for iterating on configuration. Without preview, users must repeatedly modify, export, and check results externally.

**Independent Test**: Can be fully tested by changing FunctionFromImage3D settings and verifying the preview updates to reflect those changes.

**Acceptance Scenarios**:

1. **Given** a FunctionFromImage3D with a valid ImageStack, **When** the configuration panel is open, **Then** a 2D slice preview displays a cross-section of the sampled output with a slider to control slice position
2. **Given** the preview is visible, **When** the user adjusts filter or tile settings, **Then** the 2D slice preview updates within 500ms to show the effect
3. **Given** a FunctionFromImage3D with tile style set to "wrap", **When** the user positions the slice outside [0,1] range, **Then** the preview demonstrates the wrapping behavior visually
4. **Given** a FunctionFromImage3D with scale/offset values, **When** viewing the preview, **Then** the output values reflect the applied transformation

---

### User Story 5 - Create New ImageStack from Image Directory (Priority: P2)

A user has a directory of PNG images representing volume slices and wants to import them as an ImageStack. The import process auto-detects dimensions and creates the resource.

**Why this priority**: Creating ImageStacks enables users to work with their own volumetric data. This is essential but ranked after viewing/configuration as users may receive pre-made 3MF files.

**Independent Test**: Can be fully tested by selecting a directory of PNG images and verifying an ImageStack is created with correct dimensions.

**Acceptance Scenarios**:

1. **Given** the user initiates "Create ImageStack from Directory", **When** they select a folder containing PNG files, **Then** the system imports all valid images as layers
2. **Given** a directory with sequentially named PNGs, **When** imported, **Then** the layers are ordered correctly (e.g., layer_001.png before layer_002.png)
3. **Given** images of varying sizes in the directory, **When** importing, **Then** smaller images are padded to match the largest dimensions, and the user is notified which images were resized
4. **Given** a successful import, **When** complete, **Then** the new ImageStack appears in the Outline and can be viewed immediately

---

### User Story 6 - Basic ImageStack Transforms (Priority: P3)

A user discovers their imported ImageStack has incorrect orientation. They need to flip the stack along X or Y axis, or rotate it, without re-importing.

**Why this priority**: Transform operations are convenience features that improve workflow but aren't essential for basic functionality. Users could re-export corrected source data as an alternative.

**Independent Test**: Can be fully tested by applying a flip transform to an ImageStack and verifying all layers are correctly transformed.

**Acceptance Scenarios**:

1. **Given** an ImageStack is selected, **When** the user chooses "Flip Horizontal", **Then** all layers are mirrored along the X-axis
2. **Given** an ImageStack is selected, **When** the user chooses "Flip Vertical", **Then** all layers are mirrored along the Y-axis
3. **Given** an ImageStack is selected, **When** the user chooses "Rotate 90° CW", **Then** all layers are rotated clockwise, updating dimensions if rectangular
4. **Given** the user performs a transform, **When** viewing the ImageStack, **Then** the changes are immediately visible in the layer viewer

---

### User Story 7 - Create FunctionFromImage3D (Priority: P3)

A user has an ImageStack and wants to create a new FunctionFromImage3D that references it. The creation wizard guides them through initial setup.

**Why this priority**: Creation workflow is important but users often receive pre-configured 3MF files. The edit workflow (P1) covers more common daily usage.

**Independent Test**: Can be fully tested by creating a new FunctionFromImage3D from an existing ImageStack and verifying it appears in the Outline with correct references.

**Acceptance Scenarios**:

1. **Given** an ImageStack exists, **When** the user right-clicks it and selects "Create FunctionFromImage3D", **Then** a new function is created referencing that ImageStack
2. **Given** the creation dialog, **When** displayed, **Then** sensible defaults are pre-selected (Linear filter, Wrap tile style, scale=1, offset=0)
3. **Given** a new FunctionFromImage3D is created, **When** the user clicks it in Outline, **Then** the configuration panel opens for further editing

---

### Edge Cases

- What happens when an ImageStack has only 1 layer? → Layer slider should still appear but indicate "Layer 1 of 1"
- How does the system handle corrupted or unreadable PNG files during import? → Skip corrupted files, report which files failed, continue with valid files
- What happens when a FunctionFromImage3D references a deleted ImageStack? → Show error state in panel with option to select a new ImageStack
- How are very large ImageStacks (1000+ layers, 4K resolution) handled? → Use progressive loading; show loading indicator; allow navigation even while loading
- What happens with 16-bit PNG images vs 8-bit? → Support both; preserve bit depth during transforms

## Requirements *(mandatory)*

### Functional Requirements

#### ImageStack Viewer
- **FR-001**: System MUST display a 2D image view of the currently selected layer from an ImageStack
- **FR-002**: System MUST provide a slider control to navigate through layers (1 to N)
- **FR-003**: System MUST display current layer index and total layer count
- **FR-004**: System MUST support mouse wheel scrolling through layers when hovering over the viewer
- **FR-005**: System MUST scale the image to fit the available panel space while maintaining aspect ratio

#### FunctionFromImage3D Configuration Panel
- **FR-006**: System MUST display a tabbed interface with 'Properties' and 'Graph' tabs when a FunctionFromImage3D is selected in the Outline, defaulting to 'Properties' tab
- **FR-007**: System MUST provide UI controls to select filter mode: "Linear" or "Nearest"
- **FR-008**: System MUST provide UI controls to select tile style for each axis (U, V, W) independently
- **FR-009**: Tile style options MUST include: "Wrap", "Mirror", and "Clamp"
- **FR-010**: System MUST provide numeric input fields for valueOffset and valueScale parameters
- **FR-011**: System MUST provide a visual selector for the referenced ImageStack resource
- **FR-012**: Changes in the configuration panel MUST immediately update the underlying function data

#### Preview
- **FR-013**: System MUST display a 2D slice preview showing a cross-section of the FunctionFromImage3D output at a user-controllable position
- **FR-014**: Preview MUST update within 500ms when configuration parameters change
- **FR-015**: Preview MUST allow positioning the slice outside [0,1] range to demonstrate tile style behavior

#### ImageStack Creation
- **FR-016**: System MUST allow importing a directory of PNG images as a new ImageStack
- **FR-017**: System MUST automatically detect image dimensions (width, height) from imported files
- **FR-018**: System MUST order layers based on filename sorting
- **FR-026**: System MUST pad smaller images to match the largest dimensions when importing images of varying sizes

#### ImageStack Transforms
- **FR-019**: System MUST support flipping all ImageStack layers horizontally (X-axis)
- **FR-020**: System MUST support flipping all ImageStack layers vertically (Y-axis)
- **FR-021**: System MUST support rotating all ImageStack layers by 90° increments

#### Undo Support
- **FR-024**: System MUST support undo/redo for all transform operations (flip, rotate)
- **FR-025**: System MUST support undo/redo for all FunctionFromImage3D configuration changes

#### FunctionFromImage3D Creation
- **FR-022**: System MUST allow creating a new FunctionFromImage3D from an existing ImageStack
- **FR-023**: System MUST initialize new FunctionFromImage3D with sensible default values (Linear filter, Wrap tile style, scale=1, offset=0)

### Key Entities

- **ImageStack**: A collection of 2D image layers representing volumetric data. Key attributes: layer count, width, height, pixel format, resource ID.
- **FunctionFromImage3D**: A function resource that samples from an ImageStack. Key attributes: image3dID reference, filter mode, tile styles (U/V/W), valueOffset, valueScale.
- **Layer**: A single 2D image within an ImageStack. Key attributes: index, image data, dimensions.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can navigate through all layers of an ImageStack within 200ms response time per layer change
- **SC-002**: Users can configure all FunctionFromImage3D parameters through the new UI without opening the graph editor
- **SC-003**: Preview updates reflect configuration changes within 500ms
- **SC-004**: Users can import a directory of 100 PNG images as an ImageStack in under 30 seconds
- **SC-005**: 90% of users can successfully create and configure a FunctionFromImage3D on their first attempt without documentation
- **SC-006**: Transform operations (flip/rotate) on an ImageStack complete within 5 seconds for stacks up to 100 layers of 1024x1024 images
