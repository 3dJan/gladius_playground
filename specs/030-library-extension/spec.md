# Feature Specification: Extend the 3MF Library with New Items

**Feature Branch**: `030-library-extension`  
**Created**: 2026-06-10  
**Status**: Draft  
**Input**: User description: "Extend the 3mf library. Derive reasonable additions from the existing set of items (e.g., if there is a symmetryX then there should also be symmetryY and symmetryZ). Define what makes up a good library item. Also consider the helpfulness and visual appeal of the thumbnails (important for our user experience)."

## User Scenarios & Testing

### User Story 1 - Discover New Library Items Quickly (Priority: P1)

A Gladius user browses the library panel, sees thumbnails with clear labels, and wants to find items relevant to their current modeling task. They rely on tags and descriptions to filter and select appropriate primitives, modifiers, or operations.

**Why this priority**: Without proper metadata (tags, descriptions), users cannot efficiently discover new items. This is foundational for any library extension to be useful.

**Independent Test**: Can be fully tested by opening the library panel in Gladius, filtering by tags like "symmetry" or "mechanical", and verifying that newly added items appear with correct thumbnails and metadata.

**Acceptance Scenarios**:

1. **Given** a user has opened the library panel, **When** they type "symmetry" in the search box, **Then** all symmetry-related entries (symmetryX, symmetryY, symmetryZ) are displayed
2. **Given** a user is looking for mechanical parts, **When** they filter by tag "mechanical", **Then** helix_spring, involute_gear, iso_external_thread, and new items like hex_nut appear in the list
3. **Given** a user hovers over a library item thumbnail, **When** the tooltip displays, **Then** it shows the description and all tags

---

### User Story 2 - Apply Symmetry Operations to Shapes (Priority: P1)

A user has modeled a complex shape using primitives and CSG operations. They want to mirror their work across multiple axes for symmetry in their design. Currently only symmetryX exists, forcing them to manually duplicate or use workarounds.

**Why this priority**: This is the most obvious gap - if symmetryX exists but Y/Z don't, users cannot complete symmetric designs efficiently. It's a direct extension of existing functionality.

**Independent Test**: Can be fully tested by importing symmetryY and symmetryZ into a project, applying them to a test shape, and verifying correct mirroring behavior in the preview viewport.

**Acceptance Scenarios**:

1. **Given** a user has imported symmetryY from the library, **When** they apply it to a shape, **Then** the shape is mirrored across the Y axis (top/bottom reflection)
2. **Given** a user applies both symmetryX and symmetryZ, **When** viewing the result, **Then** the shape is reflected across both axes simultaneously
3. **Given** a user imports symmetryXYZ, **When** they apply it to an asymmetric shape, **Then** the result shows point reflection through the origin (all 8 octants)

---

### User Story 3 - Add Missing Basic Primitives (Priority: P2)

A user needs common geometric shapes for mechanical modeling but finds that sphere, cylinder, and cone are available while ellipsoid, capsule, and other basic forms are missing. They expect the primitives category to cover fundamental geometry.

**Why this priority**: Basic primitives form the foundation of all SDF modeling. Missing common shapes forces users to compose complex workarounds from simpler ones.

**Independent Test**: Can be fully tested by importing ellipsoid, capsule, and diamond into a project, verifying they render correctly in the preview viewport with proper bounding boxes.

**Acceptance Scenarios**:

1. **Given** a user imports ellipsoid, **When** they adjust the three radius parameters, **Then** the shape stretches proportionally along X, Y, Z axes
2. **Given** a user imports capsule, **When** they set height > 0 with equal radii, **Then** it renders as a cylinder with hemispherical end caps
3. **Given** a user imports diamond (bicone), **When** they adjust topRadius and bottomRadius independently, **Then** the shape smoothly transitions between two cone profiles

---

### User Story 4 - Apply Shape Deformation Modifiers (Priority: P2)

A user wants to bend or twist existing shapes for organic modeling. Currently only offset, round, shell, onion, extrude_z, and clamp_distance exist as modifiers. They need deformation operations like twist and bend.

**Why this priority**: Modifiers extend the expressive power of the library beyond static geometry. Twist and bend are natural complements to existing transform-domain operations.

**Independent Test**: Can be fully tested by importing twist and bend modifiers, applying them to a cylinder or box, and verifying smooth deformation across the parameter range.

**Acceptance Scenarios**:

1. **Given** a user imports the twist modifier, **When** they apply it to a cylinder with angle=90°, **Then** the top face rotates 90° relative to the base while maintaining volume
2. **Given** a user applies bend modifier to a box, **When** they increase the bend angle from 0° to 180°, **Then** the shape smoothly curves into an arc without self-intersection
3. **Given** a user combines twist with existing round modifier, **When** viewing the result, **Then** both deformations are applied in sequence (twist first, then edge rounding)

---

### User Story 5 - Browse Mechanical Parts for Engineering Applications (Priority: P3)

A mechanical engineer using Gladius wants to quickly find fasteners and bearings for assembly modeling. They expect the mechanical category to include common hardware like nuts, washers, and bolts alongside existing gear and spring items.

**Why this priority**: This extends the library into practical engineering use cases, making it more valuable for CAD-like workflows. It's lower priority because core geometry (primitives, modifiers) is more fundamental.

**Independent Test**: Can be fully tested by importing hex_nut, washer_flat, and rivet, verifying they render with correct proportions and can be combined with existing helix_spring or iso_external_thread items.

**Acceptance Scenarios**:

1. **Given** a user imports hex_nut, **When** they set the metric size parameter (e.g., M10), **Then** the nut has correct hexagonal profile and threaded hole
2. **Given** a user combines washer_flat with socket_cap_screw, **When** viewing the assembly in preview, **Then** the washer sits flush against the screw head with proper inner/outer diameter ratio
3. **Given** a user filters mechanical items by tag "fastener", **Then** hex_nut, washer_flat, rivet, and iso_external_thread all appear

---

### Edge Cases

- What happens when a symmetry operation is applied to an already-symmetric shape? → Result should be identical (idempotent behavior)
- How does twist modifier handle shapes with non-uniform cross-sections? → Should apply rotation proportional to local distance from base plane
- What if a user imports both symmetryX and a custom X-mirror function with different names? → Both appear as separate entries; no automatic deduplication
- How are thumbnails generated for wireframe-only items (e.g., lattice structures)? → Render solid version with slight opacity for depth perception

## Requirements

### Functional Requirements

- **FR-001**: System MUST allow importing new library items from the library panel without restarting Gladius
- **FR-002**: System MUST display thumbnails for all library entries in the browser view, generated from a representative rendering of the tagged function's output
- **FR-003**: System MUST support filtering library entries by tags with real-time search results
- **FR-004**: Each new library item MUST include: (a) descriptive name in lower_snake_case, (b) one-sentence description, (c) minimum 5 relevant tags, (d) exactly one tagged function
- **FR-005**: System MUST validate imported library items before display, rejecting entries with duplicate function names or missing tagged functions
- **FR-006**: Symmetry operations (symmetryY, symmetryZ, symmetryXYZ) MUST return `vec3 result` representing the transformed position
- **FR-007**: Shape deformation modifiers (twist, bend) MUST support parameter ranges that produce visually smooth deformations without self-intersection artifacts
- **FR-008**: Mechanical parts (hex_nut, washer_flat, rivet) MUST use standard metric dimensions where applicable (e.g., M6, M8, M10, M12)

### Assumptions

- Library items follow the existing 3MF format with embedded GLSL-like function snippets
- Thumbnail generation uses the same rendering pipeline as the preview viewport (OpenCL-based ray marching)
- Users expect new items to be importable via the library browser UI, not only through manual file placement
- Tag system is already implemented and searchable in the current Gladius version

### Key Entities

- **LibraryEntry**: Represents a single 3MF item with name, description, tags, tagged function reference, thumbnail path, and shipped status
- **TaggedFunction**: The primary exportable function within an entry; returns either `float shape` (SDF) or `vec3 result` (transform/texture); identified by resource_id
- **Thumbnail**: Rendered image of the entry's geometry at standard camera position (isometric, upper-left lighting, neutral background); stored as PNG in library directory

## Success Criteria

### Measurable Outcomes

- **SC-001**: Users can find any symmetry-related item within 2 clicks from the library browser search
- **SC-002**: All newly added library items display valid thumbnails that accurately represent the geometry (no blank, clipped, or distorted previews)
- **SC-003**: New library items have complete metadata: 100% of entries include description text and minimum 5 tags
- **SC-004**: Users can apply symmetryY, symmetryZ, twist, and bend modifiers to any primitive shape without errors in the preview viewport


**Feature Branch**: `[###-feature-name]`  
**Created**: [DATE]  
**Status**: Draft  
**Input**: User description: "$ARGUMENTS"

## User Scenarios & Testing *(mandatory)*

<!--
  IMPORTANT: User stories should be PRIORITIZED as user journeys ordered by importance.
  Each user story/journey must be INDEPENDENTLY TESTABLE - meaning if you implement just ONE of them,
  you should still have a viable MVP (Minimum Viable Product) that delivers value.
  
  Assign priorities (P1, P2, P3, etc.) to each story, where P1 is the most critical.
  Think of each story as a standalone slice of functionality that can be:
  - Developed independently
  - Tested independently
  - Deployed independently
  - Demonstrated to users independently
-->

### User Story 1 - [Brief Title] (Priority: P1)

[Describe this user journey in plain language]

**Why this priority**: [Explain the value and why it has this priority level]

**Independent Test**: [Describe how this can be tested independently - e.g., "Can be fully tested by [specific action] and delivers [specific value]"]

**Acceptance Scenarios**:

1. **Given** [initial state], **When** [action], **Then** [expected outcome]
2. **Given** [initial state], **When** [action], **Then** [expected outcome]

---

### User Story 2 - [Brief Title] (Priority: P2)

[Describe this user journey in plain language]

**Why this priority**: [Explain the value and why it has this priority level]

**Independent Test**: [Describe how this can be tested independently]

**Acceptance Scenarios**:

1. **Given** [initial state], **When** [action], **Then** [expected outcome]

---

### User Story 3 - [Brief Title] (Priority: P3)

[Describe this user journey in plain language]

**Why this priority**: [Explain the value and why it has this priority level]

**Independent Test**: [Describe how this can be tested independently]

**Acceptance Scenarios**:

1. **Given** [initial state], **When** [action], **Then** [expected outcome]

---

[Add more user stories as needed, each with an assigned priority]

### Edge Cases

<!--
  ACTION REQUIRED: The content in this section represents placeholders.
  Fill them out with the right edge cases.
-->

- What happens when [boundary condition]?
- How does system handle [error scenario]?

## Requirements *(mandatory)*

<!--
  ACTION REQUIRED: The content in this section represents placeholders.
  Fill them out with the right functional requirements.
-->

### Functional Requirements

- **FR-001**: System MUST [specific capability, e.g., "allow users to create accounts"]
- **FR-002**: System MUST [specific capability, e.g., "validate email addresses"]  
- **FR-003**: Users MUST be able to [key interaction, e.g., "reset their password"]
- **FR-004**: System MUST [data requirement, e.g., "persist user preferences"]
- **FR-005**: System MUST [behavior, e.g., "log all security events"]

*Example of marking unclear requirements:*

- **FR-006**: System MUST authenticate users via [NEEDS CLARIFICATION: auth method not specified - email/password, SSO, OAuth?]
- **FR-007**: System MUST retain user data for [NEEDS CLARIFICATION: retention period not specified]

### Key Entities *(include if feature involves data)*

- **[Entity 1]**: [What it represents, key attributes without implementation]
- **[Entity 2]**: [What it represents, relationships to other entities]

## Success Criteria *(mandatory)*

<!--
  ACTION REQUIRED: Define measurable success criteria.
  These must be technology-agnostic and measurable.
-->


