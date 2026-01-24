# Feature Specification: FunctionCall Node Navigation

**Feature Branch**: `013-func-call-nav`  
**Created**: 2026-01-24  
**Status**: Draft  
**Input**: User description: "When user double-clicks on free area in FunctionCall node, ModelEditor should navigate to the referenced function. Browser-like history navigation with mouse back/forward buttons. Feature may have existed before but might be broken."

## Context

Investigation reveals that this feature already has an implementation in the codebase:
- Double-click detection exists in `NodeView::show()` (lines 316-336)
- Navigation history exists via `FunctionNavigationHistory` class
- Mouse back/forward button handling exists in `ModelEditor::showAndEdit()` (lines 1335-1346)

However, the current double-click detection may be broken because it uses `ImGui::IsItemHovered()` after `content()` is drawn, which only checks the last ImGui item - likely an inner UI element, not the node's clickable area.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Navigate to Function via Double-Click (Priority: P1)

As a user working with a complex node graph containing FunctionCall nodes, I want to double-click on a FunctionCall node to quickly jump into the referenced function's graph, so I can inspect or modify its implementation without manually finding it in the outline.

**Why this priority**: This is the core navigation action that enables rapid exploration of nested function graphs - essential for productivity when working with modular designs.

**Independent Test**: Can be fully tested by creating a model with nested FunctionCall nodes, double-clicking on them, and verifying the editor switches to show the referenced function.

**Acceptance Scenarios**:

1. **Given** a model open in the ModelEditor with a FunctionCall node referencing function "MyShape", **When** user double-clicks anywhere on the FunctionCall node's free area (header, body background), **Then** the ModelEditor navigates to display the "MyShape" function graph.

2. **Given** a model open with a FunctionCall node referencing a valid function, **When** user double-clicks on the node name input field or parameter inputs, **Then** the double-click is consumed by the input field (normal text editing behavior) and navigation does NOT occur.

3. **Given** a FunctionCall node referencing a function that no longer exists (deleted), **When** user double-clicks on the node, **Then** nothing happens (no crash, no navigation).

---

### User Story 2 - Navigate Back with Mouse Button (Priority: P1)

As a user who has drilled down into nested functions, I want to press the mouse back button to return to the previous function I was viewing, so I can quickly navigate my edit history like a web browser.

**Why this priority**: Essential companion to Story 1 - without back navigation, drilling into functions becomes a one-way trip requiring manual navigation to return.

**Independent Test**: Navigate to a function via any method, then press mouse back button and verify return to previous function.

**Acceptance Scenarios**:

1. **Given** user navigated from function A to function B (via double-click or outline), **When** user presses mouse back button (Mouse4/X1), **Then** ModelEditor returns to function A.

2. **Given** user is viewing the first function in their session (no navigation history), **When** user presses mouse back button, **Then** nothing happens (no crash, no change).

3. **Given** user's mouse cursor is outside the ModelEditor area, **When** user presses mouse back button, **Then** the navigation action is NOT triggered.

---

### User Story 3 - Navigate Forward with Mouse Button (Priority: P2)

As a user who went back in history, I want to press the mouse forward button to go forward again, completing the browser-like navigation experience.

**Why this priority**: Valuable but secondary - users first need back navigation before forward becomes useful.

**Independent Test**: Navigate A→B, go back to A, then press forward and verify return to B.

**Acceptance Scenarios**:

1. **Given** user navigated from A to B, then pressed back to return to A, **When** user presses mouse forward button (Mouse5/X2), **Then** ModelEditor navigates to function B.

2. **Given** user is at the most recent point in navigation history (no forward history), **When** user presses mouse forward button, **Then** nothing happens.

3. **Given** user went back to A, then navigated to a new function C (not B), **When** user presses forward button, **Then** nothing happens (forward history was truncated when new navigation occurred).

---

### User Story 4 - FunctionGradient Node Navigation (Priority: P3)

As a user working with FunctionGradient nodes, I want double-click navigation to also work on these nodes, so I can inspect the underlying function they reference.

**Why this priority**: Extends the core feature to another node type - valuable but not blocking core workflow.

**Independent Test**: Create a FunctionGradient node referencing a function, double-click it, verify navigation.

**Acceptance Scenarios**:

1. **Given** a FunctionGradient node referencing function "MyShape", **When** user double-clicks on the node's free area, **Then** ModelEditor navigates to the "MyShape" function.

---

### Edge Cases

- What happens when double-clicking on a FunctionCall node with functionId = 0 (not yet configured)?
  - Expected: No navigation, no crash.
- How does system handle double-clicking while an export is in progress?
  - Expected: Navigation should be allowed (read-only operation).
- What happens if user rapidly double-clicks multiple times?
  - Expected: Only one navigation occurs, no history corruption.
- What happens when the referenced function exists but has been renamed?
  - Expected: Navigation works (references are by ID, not name).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST detect double-clicks on FunctionCall node's non-interactive areas (header, body background).
- **FR-002**: System MUST navigate to the referenced function when a valid FunctionCall node is double-clicked.
- **FR-003**: System MUST NOT navigate when double-clicking on interactive elements (input fields, dropdowns) within a node.
- **FR-004**: System MUST record each navigation in a history stack.
- **FR-005**: System MUST navigate backward when mouse back button (X1/Mouse4) is pressed while hovering the ModelEditor.
- **FR-006**: System MUST navigate forward when mouse forward button (X2/Mouse5) is pressed while hovering the ModelEditor.
- **FR-007**: System MUST truncate forward history when user navigates to a new function after going back.
- **FR-008**: System MUST silently ignore navigation requests when no valid target exists (avoid crashes).
- **FR-009**: System SHOULD extend double-click navigation to FunctionGradient nodes.

### Key Entities

- **FunctionNavigationHistory**: Maintains a stack of visited function IDs with current position index, supporting back/forward traversal.
- **FunctionCall Node**: A node that references another function by ResourceId; the primary target for double-click navigation.
- **FunctionGradient Node**: A node that also references a function; secondary target for navigation.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can navigate into a FunctionCall node's referenced function within 1 second via double-click.
- **SC-002**: Users can return to the previous function within 1 second using mouse back button.
- **SC-003**: Navigation history correctly tracks at least 50 entries without corruption.
- **SC-004**: 100% of FunctionCall double-click attempts on valid nodes result in successful navigation.
- **SC-005**: Zero crashes occur when interacting with navigation features under any edge case conditions.

## Assumptions

- Mouse back/forward buttons are mapped to X1 (Mouse4) and X2 (Mouse5) respectively, which is standard for most mice and detected via `ImGuiKey_MouseX1` and `ImGuiKey_MouseX2`.
- The current `FunctionNavigationHistory` implementation is functionally correct; the issue lies in the double-click detection mechanism in `NodeView`.
- The ImGui Node Editor library provides APIs to detect clicks on node areas beyond individual widgets.
