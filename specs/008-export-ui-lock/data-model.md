# Data Model: Export UI Lock

**Feature**: 008-export-ui-lock  
**Date**: 2025-01-06

## Entities

This feature does not introduce new persistent entities. It modifies the runtime state tracking of the existing `ExportState` class.

## ExportState (Existing Class - Extended)

### Current State
```
ExportState
├── m_exportInProgress: atomic<bool>  # Thread-safe flag
├── m_exportDescription: string       # Description for logging
├── isExportInProgress(): bool        # Read current state
├── beginExport(description): void    # Start export
└── endExport(): void                 # End export
```

### Extended Interface
No new fields required. The existing interface is sufficient:
- `isExportInProgress()` is called by UI components to check if blocking is needed
- `m_exportDescription` can be displayed on the overlay (already accessible)

## Component Dependencies

### State Flow
```
MeshExporter                      UI Components
     │                                  │
     │  beginExport()                   │
     ├──────────────────► ExportState ◄─┤ isExportInProgress()
     │                        │         │
     │                        ▼         │
     │                 m_exportInProgress
     │                        │         │
     │  endExport()           │         │
     ├──────────────────────────────────┤
     │                                  │
     ▼                                  ▼
Export Thread                    Main Thread (UI)
```

### UI Components Accessing ExportState
| Component | Member | Set By |
|-----------|--------|--------|
| MainWindow | `m_exportState` (value) | Self-owned |
| ModelEditor | `m_exportState` (pointer) | `setExportState()` |
| ResourceView | `m_exportState` (pointer) | `setExportState()` |
| BeamLatticeView | `m_exportState` (pointer) | `setExportState()` |
| **NodeView** | **`m_exportState` (pointer)** | **NEW: `setExportState()`** |

## State Transitions

### Export Lock State Machine
```
                   ┌───────────────────────┐
                   │                       │
                   │    UNLOCKED           │
                   │  (normal operation)   │
                   │                       │
                   └───────────┬───────────┘
                               │
                               │ beginExport()
                               ▼
                   ┌───────────────────────┐
                   │                       │
                   │     LOCKED            │
                   │  • Overlay visible    │
                   │  • Inputs blocked     │
                   │  • File ops disabled  │
                   │                       │
                   └───────────┬───────────┘
                               │
                               │ endExport()
                               │ (success or failure)
                               ▼
                   ┌───────────────────────┐
                   │                       │
                   │    UNLOCKED           │
                   │  (normal operation)   │
                   │                       │
                   └───────────────────────┘
```

## UI State During Lock

### Overlay Properties
| Property | Value |
|----------|-------|
| Background color | RGBA(0, 0, 0, 180) - ~70% black |
| Text color | RGBA(255, 255, 255, 255) - white |
| Message | "Export in progress..." |
| Position | Centered in Model Editor window |

### Blocked Interactions
| Interaction | Block Method |
|-------------|--------------|
| Parameter sliders/inputs | `ImGui::BeginDisabled()` |
| Node creation | Early return in `onCreateNode()` |
| Node deletion | Early return in `onDeleteNode()` |
| Link creation/deletion | Early return in handlers |
| Copy/paste shortcuts | Check before action |
| Undo/Redo | Menu items disabled |
| File operations | Already blocked in MainWindow |

### Allowed Interactions
| Interaction | Rationale |
|-------------|-----------|
| Viewport pan/zoom | Non-destructive navigation |
| Window resize/move | OS-level window management |
| Progress bar updates | Export feedback |
| Cancel button | Export control |
| Close confirmation | User safety |
