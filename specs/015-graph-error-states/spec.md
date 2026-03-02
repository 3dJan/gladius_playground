# Feature Specification: Graph Error States

**Feature Branch**: `015-graph-error-states`  
**Created**: January 24, 2026  
**Status**: Draft  
**Input**: User description: "when a graph is modified by the user, it might get invalidated causing updates (code generation, preview rendering) to fail. sometimes the update attempts get repeated endlessly, with and endless repetition of the same error, the application becomes unusable. A better approach would be to handle the errors as states. we could track all unresolved issues and present all issues as list to the user (maybe with links to the nodes) and actual suggestion for fixing the issues. Updates should not be done until we leave the error state. that also means we need to seperate the validation from the update/refresh. validation should not create events, but just update the list of issues."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - View All Graph Validation Issues (Priority: P1)

As a user editing a function graph, I want to see all validation issues in a consolidated list so that I understand what problems need to be fixed before the model can be rendered or exported.

**Why this priority**: This is the core capability that enables users to understand why their graph is invalid. Without visibility into issues, users cannot take corrective action.

**Independent Test**: Can be tested by creating a graph with multiple validation errors and verifying all issues appear in a consolidated list. Delivers immediate value by providing actionable feedback.

**UI Location**: Issues are displayed in two places:
- **Event Viewer panel**: Shows all validation issues across all models in the assembly (global overview)
- **Collapsible overlay/banner in graph editor**: Shows issues for the currently open graph only (context-specific, non-intrusive)

**Acceptance Scenarios**:

1. **Given** a graph with missing connections on multiple nodes, **When** the user modifies the graph, **Then** all missing connection issues are displayed in the Event Viewer and relevant issues appear in the graph editor overlay
2. **Given** a graph with type mismatches between connected ports, **When** validation runs, **Then** all type mismatch issues appear in the list with affected node names
3. **Given** a graph with a cyclic dependency, **When** validation runs, **Then** a cycle detection issue appears in the list
4. **Given** a previously invalid graph that is now corrected, **When** all issues are resolved, **Then** the issues list becomes empty and the graph editor overlay disappears

---

### User Story 2 - Automatic Update Suppression in Error State (Priority: P1)

As a user, I want the application to stop attempting updates when the graph is invalid so that I can work on fixing issues without being interrupted by repeated error messages.

**Why this priority**: Critical for usability. The current endless error repetition makes the application unusable and prevents productive editing.

**Independent Test**: Can be tested by creating an invalid graph and verifying that code generation and preview rendering attempts stop after validation fails.

**Acceptance Scenarios**:

1. **Given** a graph with validation errors, **When** the system detects the invalid state, **Then** code generation is not attempted
2. **Given** a graph with validation errors, **When** the system detects the invalid state, **Then** preview rendering is not attempted
3. **Given** an invalid graph that becomes valid, **When** all issues are resolved, **Then** updates automatically resume
4. **Given** a graph transitioning from valid to invalid during editing, **When** an edit introduces an error, **Then** any in-progress updates complete but no new updates are started

---

### User Story 3 - Navigate to Problem Nodes (Priority: P2)

As a user, I want to click on an issue in the list to navigate directly to the affected node so that I can quickly locate and fix problems.

**Why this priority**: Enhances productivity by reducing the time to locate issues, especially in complex graphs with many nodes.

**Independent Test**: Can be tested by clicking on an issue and verifying the graph view centers on and highlights the affected node.

**Acceptance Scenarios**:

1. **Given** an issue in the list referencing a specific node, **When** the user clicks on the issue, **Then** the graph view scrolls to center the affected node
2. **Given** an issue in the list referencing a specific node, **When** the user clicks on the issue, **Then** the affected node is visually highlighted
3. **Given** an issue involving a connection between two nodes, **When** the user clicks on the issue, **Then** both nodes and the problematic connection are highlighted

---

### User Story 4 - View Fix Suggestions (Priority: P2)

As a user, I want each validation issue to include a suggestion for how to fix it so that I can resolve problems efficiently without needing to guess.

**Why this priority**: Improves user experience by providing actionable guidance, reducing frustration and learning curve.

**Independent Test**: Can be tested by creating various types of invalid graphs and verifying each issue type includes an appropriate fix suggestion.

**Acceptance Scenarios**:

1. **Given** an issue about a missing input connection, **When** viewing the issue details, **Then** a suggestion to "Connect an output from another node to this parameter" is shown
2. **Given** an issue about a type mismatch, **When** viewing the issue details, **Then** a suggestion indicating the expected and actual types is shown
3. **Given** an issue about a deleted reference, **When** viewing the issue details, **Then** a suggestion to "Reconnect the parameter or remove the connection" is shown
4. **Given** an issue about a cyclic dependency, **When** viewing the issue details, **Then** a suggestion identifying the nodes involved in the cycle is shown

---

### User Story 5 - Silent Validation During Interactive Editing (Priority: P3)

As a user editing interactively, I want validation to run silently without flooding the event log so that the event viewer remains useful for meaningful events.

**Why this priority**: Improves signal-to-noise ratio in the event log, making it easier to spot important events.

**Independent Test**: Can be tested by making multiple invalid edits and verifying the event log does not contain repeated validation error entries.

**Acceptance Scenarios**:

1. **Given** validation runs during interactive graph editing, **When** issues are found, **Then** issues are added to the issues list, not the event log
2. **Given** an invalid graph is edited multiple times, **When** validation runs each time, **Then** the event log does not receive repeated error entries
3. **Given** a graph transitions from valid to invalid during editing, **When** this state change occurs, **Then** a single state-change event may be logged (not per-issue events)

---

### User Story 6 - Validation Events for API and File Loading (Priority: P3)

As an API consumer or user loading a 3MF file, I want validation issues to be reported as events once so that I can programmatically detect and handle invalid models.

**Why this priority**: API consumers rely on events for error handling; file loading is a one-time operation where event logging is appropriate.

**Independent Test**: Can be tested by loading an invalid 3MF file via API and verifying validation errors appear as events exactly once.

**Acceptance Scenarios**:

1. **Given** an invalid 3MF file is loaded, **When** validation runs, **Then** validation issues are logged as events once
2. **Given** a model is validated via the API, **When** issues are found, **Then** issues are reported as events for API consumers
3. **Given** an invalid file is loaded and then edited interactively, **When** subsequent edits are made, **Then** no additional validation events are generated (only the initial load events)

---

### Edge Cases

- What happens when a node referenced by an issue is deleted? The issue should be automatically removed from the list on the next validation pass.
- What happens when an issue is partially fixed but creates a new issue? Both states should be reflected - old issue removed, new issue added.
- How does the system handle concurrent edits while validation is running? Validation should be debounced to avoid excessive runs during rapid editing.
- What happens to issues in sub-functions (nested models)? Issues should indicate which model they belong to.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST maintain a persistent list of validation issues that survives across validation runs
- **FR-002**: System MUST track each issue with: message, model identifier, node identifier, port/parameter name, and fix suggestion
- **FR-003**: System MUST prevent code generation (OpenCL compilation) when the graph has unresolved validation issues
- **FR-004**: System MUST prevent preview rendering updates when the graph has unresolved validation issues  
- **FR-005**: System MUST automatically resume updates when all validation issues are resolved
- **FR-006**: System MUST allow navigation from an issue to the affected node in the graph view
- **FR-006a**: System MUST display all validation issues in the Event Viewer panel for global overview
- **FR-006b**: System MUST display context-specific issues in a collapsible overlay/banner in the graph editor for the currently open model
- **FR-006c**: Graph editor overlay MUST be non-intrusive (collapsible, does not block editing)
- **FR-007**: System MUST provide fix suggestions for each issue type (missing connection, type mismatch, invalid reference, cycle)
- **FR-008**: Validation during interactive editing MUST NOT add events to the event log for individual issues
- **FR-009**: Validation during API calls or 3MF file loading MUST log issues as events exactly once
- **FR-010**: System MUST run validation after each graph modification (with appropriate debouncing)
- **FR-011**: System MUST clear issues that are no longer applicable after graph modifications
- **FR-012**: System MUST display issues from all models in an assembly (main function and sub-functions)

### Key Entities

- **ValidationIssue**: Represents a single problem in the graph. Contains: message, model ID, node ID, port/parameter name, issue type, and fix suggestion.
- **IssueList**: Collection of all current ValidationIssues. Provides methods to add, remove, clear, and query issues.
- **GraphValidityState**: Enum representing whether the graph is valid or invalid. Used to gate updates.
- **IssueSeverity**: Classification of issue importance (Error blocks updates; Warning does not).
- **ValidationContext**: Indicates whether validation is triggered by interactive editing (silent) or by API/file loading (events emitted once).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can see all validation issues in a single consolidated view within 1 second of making an edit
- **SC-002**: Application remains responsive during graph editing, with no repeated error dialogs or frozen UI
- **SC-003**: Users can navigate to a problem node with a single click from the issue list
- **SC-004**: 100% of issue types include an actionable fix suggestion
- **SC-005**: Event log contains zero repeated validation error messages during interactive editing sessions
- **SC-006**: API and file loading operations emit validation events exactly once per issue
- **SC-007**: Updates resume automatically within 1 second of the last issue being resolved

## Clarifications

### Session 2026-01-24

- Q: Where should the consolidated issues list be displayed in the UI? → A: Both Event Viewer panel (global overview of all issues) AND collapsible overlay/banner in graph editor (issues for currently open graph, non-intrusive)

## Assumptions

- Existing `ValidationError` struct can be extended to include fix suggestions
- The current `Validator` class architecture supports separation of validation from event logging
- Graph view supports programmatic scrolling and node highlighting
- Debouncing validation to ~100-200ms after last edit is acceptable for responsiveness
- Validation call sites can distinguish between interactive editing context and API/file loading context
- Event Viewer panel can be extended to show/filter validation issues
- Graph editor supports adding a collapsible overlay component
