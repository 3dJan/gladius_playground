# Export Lock Contract

**Feature**: 008-export-ui-lock  
**Date**: 2025-01-06

## Overview

This document defines the internal API contracts for the export UI lock feature. Since this is a UI feature without external APIs, contracts focus on internal component interfaces.

## ExportState Interface (Existing)

```cpp
/// Thread-safe export state tracking for UI coordination
class ExportState
{
public:
    /// @brief Check if an export operation is currently in progress
    /// @return true if export is active, false otherwise
    /// @note Thread-safe: uses atomic load
    [[nodiscard]] bool isExportInProgress() const;
    
    /// @brief Mark the start of an export operation
    /// @param description Human-readable description for logging/display
    /// @note Thread-safe: uses atomic store
    void beginExport(std::string description);
    
    /// @brief Mark the end of an export operation
    /// @note Thread-safe: uses atomic store
    void endExport();
    
    /// @brief Get the current export description
    /// @return Description string (empty if no export in progress)
    [[nodiscard]] std::string const& getDescription() const;
};
```

## NodeView Extension

```cpp
/// Add to NodeView class
class NodeView
{
public:
    /// @brief Set the export state for blocking input during export
    /// @param state Pointer to ExportState (may be nullptr)
    void setExportState(ExportState* state);
    
private:
    ExportState* m_exportState{nullptr};
};
```

## ModelEditor Extension

```cpp
/// Add to ModelEditor class (already has setExportState)
class ModelEditor
{
public:
    // Existing: void setExportState(ExportState* state);
    
private:
    /// @brief Render the export overlay if export is in progress
    /// @note Called after ed::End(), before ImGui::End()
    void renderExportOverlay();
};
```

## Behavioral Contracts

### BC-001: Overlay Visibility
```
GIVEN export is in progress
WHEN ModelEditor::showAndEdit() renders
THEN a semi-transparent overlay MUST be displayed over the editor content
```

### BC-002: Input Blocking
```
GIVEN export is in progress
WHEN user interacts with any parameter input widget
THEN the interaction MUST be ignored (no model changes)
```

### BC-003: Node Operations Blocked
```
GIVEN export is in progress
WHEN user attempts to create, delete, or link nodes
THEN the operation MUST be blocked (no model changes)
```

### BC-004: State Consistency
```
GIVEN ExportGuard is used for export scoping
WHEN export thread completes (success or exception)
THEN endExport() MUST be called (RAII guarantee)
AND overlay MUST be removed on next frame
```

### BC-005: Thread Safety
```
GIVEN export runs on background thread
AND UI runs on main thread
WHEN both threads access ExportState
THEN no data races MUST occur (atomic operations)
```

## Error Handling

### Export Failure
- `ExportGuard` destructor calls `endExport()` on scope exit
- UI lock is released immediately on failure
- No additional error handling needed in UI lock code

### Null ExportState
- Components MUST check for nullptr before accessing ExportState
- Pattern: `if (m_exportState && m_exportState->isExportInProgress())`
