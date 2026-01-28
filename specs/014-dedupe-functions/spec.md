# Feature Specification: Deduplicate Functionally Identical Functions

**Feature Branch**: `014-dedupe-functions`  
**Created**: 2026-01-24  
**Status**: Draft  
**Input**: User description: "sometimes a 3mf file might contain functions that do exactly the same (e.g. after merging functions from one file into another). we want to have a method for comparing functions. function should be considered functionally equal, if they are mathematical identical, so they would return the same results (ignoring node names and other decorational data not affecting the function evaluation). We want a button in the model editor outline to remove functionally identical functions. of course, all references to the removed functions should be updated. the comparison and clean up methods should be covered by unit tests."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Clean Up Duplicate Functions After Merge (Priority: P1)

A user has merged multiple 3MF files containing implicit functions into a single document. Some functions are mathematically identical but have different names or node identifiers (originating from different source files). The user wants to consolidate these duplicates to reduce clutter and file size.

**Why this priority**: This is the primary use case that motivated the feature. Duplicate functions waste memory, increase file size, and create confusion in the function list.

**Independent Test**: Can be fully tested by loading a 3MF file with known duplicate functions, clicking the deduplicate button, and verifying only unique functions remain with all references preserved.

**Acceptance Scenarios**:

1. **Given** a document with two functions `func_A` and `func_B` that are mathematically identical (same graph structure, same node types, same constants), **When** the user clicks "Remove Duplicate Functions" in the model editor outline, **Then** one function is removed and all `FunctionCall` nodes referencing the removed function are updated to reference the retained function.

2. **Given** a document with three functions where `func_A` and `func_C` are mathematically identical but `func_B` is different, **When** the user clicks "Remove Duplicate Functions", **Then** only `func_C` (or `func_A`) is removed, `func_B` remains unchanged, and references are updated accordingly.

3. **Given** a document with no duplicate functions, **When** the user clicks "Remove Duplicate Functions", **Then** a message indicates no duplicates were found and no functions are removed.

---

### User Story 2 - Preserve Function with Established References (Priority: P2)

When duplicates exist, the system should intelligently choose which function to keep. Functions that have external references (level sets, mesh objects) or are referenced more frequently should be preserved.

**Why this priority**: Improves user experience by keeping the most "important" function rather than arbitrarily choosing.

**Independent Test**: Can be tested by creating duplicates with different reference counts and verifying the more-referenced function is retained.

**Acceptance Scenarios**:

1. **Given** two identical functions where `func_A` has an external reference (level set) and `func_B` has none, **When** deduplication runs, **Then** `func_A` is retained and `func_B` is removed (regardless of internal reference counts).

2. **Given** two identical functions where both have external references, or neither has external references, and `func_A` is referenced by 5 `FunctionCall` nodes and `func_B` is referenced by 1, **When** deduplication runs, **Then** `func_A` is retained and `func_B` is removed.

3. **Given** two identical functions with equal external and internal reference counts, **When** deduplication runs, **Then** the function with the lower `ResourceId` is retained (deterministic behavior).

---

### User Story 3 - Undo Deduplication (Priority: P3)

The user should be able to undo the deduplication operation if they accidentally removed a function they wanted to keep (e.g., for documentation purposes).

**Why this priority**: Safety net for users; undo is a standard expectation for destructive operations.

**Independent Test**: Can be tested by performing deduplication, then pressing undo, and verifying all functions and references are restored.

**Acceptance Scenarios**:

1. **Given** deduplication has just been performed, **When** the user presses Ctrl+Z or clicks Undo, **Then** all removed functions are restored and all reference updates are reverted.

---

### Edge Cases

- What happens when a function references itself recursively? (Self-referencing functions should be handled correctly in comparison)
- How does the system handle functions with identical structure but different constant values? (They should NOT be considered duplicates)
- What happens when the assembly model itself is a duplicate of another function? (The assembly model should never be removed)
- How are circular references between functions handled? (FunctionA → FunctionB → FunctionA)
- What happens if removing a function would break external references (e.g., from mesh objects or level sets)? (All references must be updated, not just FunctionCall nodes)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide a function comparison algorithm that determines mathematical equality between two `Model` instances
- **FR-002**: System MUST ignore the following when comparing functions: node names, node display names, node IDs, function display names, function names, and port identifiers
- **FR-003**: System MUST consider the following when comparing functions: graph topology (connectivity), node types, constant values (Constant, ConstVec, ConstMat nodes), and resource references (ResourceId values)
- **FR-004**: System MUST use a tolerance for floating-point constant comparisons (epsilon ~1e-6)
- **FR-005**: System MUST provide a method to find all duplicate function groups in an Assembly
- **FR-006**: System MUST provide a method to remove duplicate functions and update all references
- **FR-007**: System MUST update all `FunctionCall` nodes that reference a removed function to reference the retained equivalent function
- **FR-008**: System MUST update all external references (level sets, mesh object references) that point to removed functions
- **FR-009**: System MUST never remove the assembly model (root function) during deduplication
- **FR-010**: System MUST provide a button in the model editor outline to trigger deduplication
- **FR-011**: System MUST display feedback to the user indicating how many duplicates were found and removed
- **FR-012**: Deduplication operation MUST be undoable via the standard undo mechanism
- **FR-013**: Comparison and deduplication logic MUST be covered by comprehensive unit tests

### Key Entities

- **Model**: Represents an implicit function as a directed acyclic graph (DAG) of nodes. Contains nodes, ports, and parameters.
- **Assembly**: Container for multiple `Model` instances, mapping `ResourceId` to `SharedModel`.
- **FunctionCall**: Node type that references another function by `ResourceId`.
- **NodeBase**: Base class for all node types, contains type, parameters, and ports.

## Assumptions

- The existing `FunctionComparator` in `gladius/src/io/3mf/FunctionComparator.h` compares at the Lib3MF level and is NOT suitable for this feature (it compares names and identifiers). A new comparator working at the internal `Model` level will be created.
- Two functions are considered mathematically identical if they would produce the same output for any given input, which can be determined by comparing:
  - Graph structure (isomorphism)
  - Node types at corresponding positions
  - Constant values with floating-point tolerance
- The deduplication will work on the internal representation, not the 3MF file format level.
- When multiple functions are identical, the one with the lowest `ResourceId` or highest reference count will be retained (deterministic).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can identify and remove duplicate functions with a single button click
- **SC-002**: All references to removed functions are correctly updated (no broken `FunctionCall` links)
- **SC-003**: Deduplication completes within 5 seconds for assemblies with up to 100 functions
- **SC-004**: Unit test coverage includes: identical functions, nearly-identical functions (different constants), structurally different functions, self-referencing functions, and circular references
- **SC-005**: No data loss - deduplication is fully reversible via undo
- **SC-006**: File size reduction proportional to the number of removed duplicates when saving
