# Feature Specification: Library Metadata for Selective Function Import

**Feature Branch**: `018-library-metadata`  
**Created**: 2026-02-14  
**Status**: Draft  
**Input**: User description: "Library metadata for selective function import with descriptions and export-to-library wizard"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Selective Import from Library File (Priority: P1)

A user opens the Library Browser, browses a category tab (e.g., "lattices"), and double-clicks a library entry. Instead of merging the entire file (including the example build item and any helper geometry), only the tagged importable function and its dependencies are added to the current document.

**Why this priority**: This is the core value proposition — making library import clean and predictable. Without it, importing a library function pollutes the document with unrelated example geometry.

**Independent Test**: Can be tested by creating a 3MF file with `gladius:library-functions` metadata tagging one function, plus an unrelated example build item. Double-clicking imports only the tagged function and its dependencies; the example mesh and build item are not imported.

**Acceptance Scenarios**:

1. **Given** a library 3MF file with `gladius:library-functions` metadata identifying one function by model resource ID and an unrelated example build item, **When** the user double-clicks the file in the Library Browser, **Then** only the identified function and its transitive dependencies (referenced functions, image stacks) are imported into the current document.
2. **Given** a library 3MF file with `gladius:library-functions` metadata identifying two functions by model resource ID, **When** the user double-clicks the file, **Then** both identified functions and all their transitive dependencies are imported.
3. **Given** a library 3MF file **without** `gladius:library-functions` metadata (legacy/plain 3MF), **When** the user double-clicks it, **Then** the entire file is merged as it is today (backward compatibility).
4. **Given** a library 3MF file where the tagged function depends on another function via a FunctionCall node, **When** the file is imported selectively, **Then** the dependency function is also imported.
5. **Given** a function with the same display name already exists in the current document, **When** the user imports a library function with that name, **Then** the existing duplicate-detection logic applies (same behavior as current merge).

---

### User Story 2 - Description Display in Library Browser (Priority: P2)

A user browses the Library Browser and sees a short description for each library entry below its thumbnail and filename. This helps the user understand what each entry offers without having to open or import it.

**Why this priority**: Descriptions are essential for usability as the library grows. Without them, users must guess what each entry does based on filename and thumbnail alone.

**Independent Test**: Can be tested by adding `gladius:library-description` metadata to a 3MF file, placing it in a library folder, and verifying the description text appears in the Library Browser UI.

**Acceptance Scenarios**:

1. **Given** a library 3MF file with `gladius:library-description` metadata set to "Triply periodic minimal surface pattern", **When** the user views the Library Browser, **Then** the description text is displayed below the file name.
2. **Given** a library 3MF file **without** `gladius:library-description` metadata, **When** the user views the Library Browser, **Then** no description is shown and the layout remains clean (no empty space or placeholder).
3. **Given** a description longer than 80 characters, **When** displayed in the Library Browser, **Then** the text is truncated with an ellipsis and the full description is visible on hover (tooltip).

---

### User Story 3 - Function Names Display in Library Browser (Priority: P2)

A user browses the Library Browser and can see which importable functions each library entry offers, displayed as labels near the thumbnail.

**Why this priority**: Knowing which functions will be imported helps users make informed choices. This pairs naturally with Story 2 and uses the same metadata already read for Story 1.

**Independent Test**: Can be tested by adding `gladius:library-functions` metadata listing two function names, and verifying both names appear in the Library Browser entry.

**Acceptance Scenarios**:

1. **Given** a library 3MF file with `gladius:library-functions` metadata listing two function resource IDs, **When** the user views the Library Browser, **Then** both functions' display names are resolved and shown as labels in the file's entry.
2. **Given** a library 3MF file without `gladius:library-functions` metadata, **When** the user views the Library Browser, **Then** no function labels are shown.

---

### User Story 4 - Export to Library Wizard (Priority: P3)

A user wants to contribute a reusable function to the library. They select a function from their current document, open the "Export to Library" wizard, choose a category (subfolder), enter a short description, and export. The wizard performs a normal 3MF save of a working copy, then removes unused resources not needed by the selected function's build item, and stamps the file with the appropriate metadata.

**Why this priority**: The export wizard enables library growth and user customization but requires Stories 1-2 to already work. It's a higher-effort feature that builds on the metadata foundation.

**Independent Test**: Can be tested by creating a document with multiple functions and build items, running the wizard to export one function, and verifying the resulting 3MF file contains the correct metadata and only the necessary resources.

**Acceptance Scenarios**:

1. **Given** a document with functions A, B, and C where build item X uses function A, **When** the user exports function A to the library via the wizard, **Then** the wizard auto-selects build item X (since it references function A via the dependency graph), the resulting 3MF file contains function A and its dependencies plus build item X as the example, and the `gladius:library-functions` metadata identifying function A by model resource ID.
7. **Given** the export wizard is open and multiple build items reference the selected function, **When** the wizard detects this, **Then** it presents the user with a choice of which build item to keep as the example.
2. **Given** the export wizard is open, **When** the user enters a description "Fast threaded bolt pattern", **Then** the resulting 3MF file contains `gladius:library-description` with that text.
3. **Given** the export wizard is open, **When** the user selects the category "screws" from the dropdown, **Then** the file is saved into the `library/screws/` subfolder.
4. **Given** no categories exist yet, **When** the user types a new category name "connectors", **Then** the subfolder `library/connectors/` is created and the file is saved there.
5. **Given** the wizard is about to export, **When** the user confirms, **Then** the wizard saves a working copy of the document as a 3MF, removes unused resources from that copy, sets the metadata, and writes the file. The original document is not modified.
6. **Given** the exported file, **When** it is opened in the Library Browser and double-clicked, **Then** only the tagged function and its dependencies are imported (round-trip validation).

---

### Edge Cases

- What happens when the `gladius:library-functions` metadata names a function that does not exist in the file? The import falls back to full merge behavior and logs a warning.
- What happens when a function's dependency chain includes an image stack (FunctionFromImage3D)? The image stack and its attachments are included in the selective import.
- What happens when the user exports to a library path that already contains a file with the same name? The wizard prompts for confirmation before overwriting.
- What happens when the library folder does not exist at startup? The Library Browser already handles this gracefully (shows empty content). The export wizard creates the folder if needed.
- What happens when importing a library function that references another function already present in the document? The existing duplicate-detection and ID-remapping logic handles this (already implemented).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST read `gladius:library-functions` metadata (namespace `gladius`, key `library-functions`) from the model-level metadata group of a 3MF file to determine which functions are tagged for selective import. The value contains semicolon-separated model resource IDs (`GetModelResourceID()`), not container-wide unique IDs.
- **FR-002**: System MUST read `gladius:library-description` metadata (namespace `gladius`, key `library-description`) from a 3MF file to retrieve the library entry's description text.
- **FR-003**: When `gladius:library-functions` metadata is present, the merge operation MUST import only the named functions and their transitive dependencies (other functions, image stacks, attachments). Build items, meshes, and unrelated resources MUST NOT be imported.
- **FR-004**: When `gladius:library-functions` metadata is absent, the merge operation MUST fall back to the existing full-merge behavior (backward compatibility).
- **FR-005**: The Library Browser MUST display the description text from `gladius:library-description` below each file entry's name when available.
- **FR-006**: The Library Browser MUST display the function names from `gladius:library-functions` as labels in each file entry when available.
- **FR-007**: The export wizard MUST allow the user to select which function(s) to tag as the library's importable function(s).
- **FR-008**: The export wizard MUST allow the user to enter a short description for the library entry.
- **FR-009**: The export wizard MUST allow the user to select an existing category (subfolder) or create a new one.
- **FR-010**: The export wizard MUST save a working copy of the document as a 3MF file, remove all resources not required by the selected function and its example build item, and stamp the file with `gladius:library-functions` and `gladius:library-description` metadata.
- **FR-011**: The export wizard MUST NOT modify the user's current working document.
- **FR-012**: The export wizard MUST include the current viewport thumbnail in the exported 3MF file.
- **FR-013**: Descriptions longer than 80 characters MUST be truncated with an ellipsis in the Library Browser, with the full text available via tooltip.

### Key Entities

- **Library Metadata**: Model-level 3MF metadata under the `gladius` namespace. Two keys: `library-functions` (semicolon-separated model resource IDs identifying the importable functions) and `library-description` (free-text description). Note: model resource IDs (`GetModelResourceID()`) are used, not container-wide unique resource IDs (`GetUniqueResourceID()`), since model resource IDs are stable within a single file.
- **Library Entry**: A 3MF file in a library category folder, optionally annotated with library metadata. Displayed as a thumbnail card in the Library Browser.
- **Library Category**: A subfolder under the library root directory. Rendered as a tab in the Library Browser. Categories are simply filesystem folders.
- **Resource Dependency Closure**: The set of all resources transitively required by a given function, as computed by the existing resource dependency graph infrastructure.

## Assumptions

- The `gladius:library-functions` metadata uses **model resource IDs** (`GetModelResourceID()`), which are unique within a single 3MF file and stable across re-saves. Container-wide unique resource IDs (`GetUniqueResourceID()`) are NOT used because they change during merge operations.
- Semicolons are used as the delimiter in `gladius:library-functions` to separate multiple resource IDs.
- Function display names are NOT required to be unique in 3MF and are therefore not suitable as identifiers. Display names are used only for UI presentation (Library Browser labels).
- The model-level metadata group supports custom namespace/key pairs, as confirmed by the 3MF format specification.
- The existing resource dependency graph correctly computes transitive dependencies including image stacks and referenced functions.
- The existing remove-unused-resources method correctly removes resources not referenced by any build item. The export wizard leverages this existing method.
- Library category tabs in the Library Browser already correspond to subfolders. No new UI for category management is needed beyond the export wizard's category picker.
- Library metadata (description, function names) is read eagerly during the directory scan, at the same time as thumbnail extraction. Since the file is already opened via lib3mf for thumbnail reading, querying the metadata group adds negligible cost.
- A single library root directory is used (the deployment folder's `library/`). Separating built-in vs. user libraries is deferred to a future enhancement.

## Clarifications

### Session 2026-02-14

- Q: What identifier should `gladius:library-functions` use for functions — display names, model resource IDs, or UUIDs? → A: Model resource IDs (`GetModelResourceID()`). Display names are not unique in 3MF; container-wide unique IDs shift during merge. Model resource IDs are stable within a single file.
- Q: How should the export wizard determine which build item to keep as the example? → A: Auto-select the build item whose object references the selected function (via the resource dependency graph). If multiple build items reference it, the user picks one.
- Q: Should library metadata (description, function list) be read eagerly during scan or lazily on demand? → A: Eagerly during directory scan, alongside thumbnail extraction. The file is already open, so the extra API call adds negligible cost.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can import a tagged library function with a single double-click, and no unrelated resources (example meshes, extra build items) appear in the document.
- **SC-002**: Users can identify library entries by reading their description and function names directly in the Library Browser, without needing to import or open the file.
- **SC-003**: Users can export a reusable function to the library in under 5 steps (select function, enter description, choose category, confirm export).
- **SC-004**: A library entry exported via the wizard can be successfully re-imported via the Library Browser (round-trip correctness).
- **SC-005**: All existing 3MF files without library metadata continue to work identically to today's behavior (zero regressions).
- **SC-006**: The selective import correctly resolves dependency chains of depth 3 or more (function A calls B which calls C — all three are imported).
