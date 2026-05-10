# Data Model: Async Export Cancellation

**Feature**: 009-async-export-cancel  
**Date**: 2025-01-07

## Entities

### CancellationToken

Thread-safe signal for cooperative cancellation between UI and worker threads.

```
CancellationToken
├── m_cancelled: atomic<bool>      # Cancellation flag
│
├── requestCancellation()          # Set flag to true (UI thread)
├── isCancelled(): bool            # Check flag (worker thread)
└── reset()                        # Clear flag for reuse
```

**Thread Safety**: Lock-free via atomic operations
**Ownership**: Owned by MeshExportDialog, shared with active exporter via pointer
**Lifetime**: Lives for duration of dialog, reset between exports

### ExportState (Extended)

Current state plus new phase tracking.

```
ExportState
├── m_exportInProgress: atomic<bool>    # Existing
├── m_exportDescription: string          # Existing
├── m_phase: atomic<ExportPhase>         # NEW
│
├── beginExport(description)             # Set Exporting phase
├── beginCancellation()                  # NEW: Set Cancelling phase
├── endExport()                          # Set Idle phase
├── isExportInProgress(): bool           # Existing
├── isCancelling(): bool                 # NEW: Check Cancelling phase
└── getPhase(): ExportPhase              # NEW: Get current phase
```

**New Enum**:
```
ExportPhase
├── Idle        # No export active
├── Exporting   # Export in progress
└── Cancelling  # Cancel requested, waiting for abort
```

### IExporter (Extended Interface)

```
IExporter
├── beginExport(path, core)        # Existing
├── advanceExport(core): bool      # Existing
├── finalize()                     # Existing
├── getProgress(): double          # Existing
│
├── setCancellationToken(token*)   # NEW: Accept token before export
└── isCancelled(): bool            # NEW: Check if should abort
```

**Note**: `setCancellationToken` is optional - exporters not supporting cancellation can ignore it.

## State Transitions

### Export Lifecycle

```
                    ┌─────────────────────────────────────┐
                    │                                     │
                    ▼                                     │
    ┌──────┐   beginExport   ┌───────────┐   complete   ┌─────────────┐
    │ Idle │ ──────────────► │ Exporting │ ───────────► │ (cleanup)   │
    └──────┘                 └───────────┘              └──────┬──────┘
        ▲                         │                            │
        │                         │ requestCancel              │
        │                         ▼                            │
        │                    ┌────────────┐                    │
        │                    │ Cancelling │                    │
        │                    └─────┬──────┘                    │
        │                          │ worker exits              │
        │                          ▼                           │
        │                    ┌─────────────┐                   │
        └────────────────────┤  (cleanup)  │ ◄─────────────────┘
                             └─────────────┘
```

### Cancel Flow Detail

1. **User clicks Cancel** (main thread)
   - Set `ExportState.phase` = `Cancelling`
   - Set `CancellationToken.cancelled` = `true`
   - Update button text to "Cancelling..."
   - Do NOT block

2. **Worker checks token** (worker thread)
   - At checkpoint: `if (m_token && m_token->isCancelled()) return;`
   - Set exporter state to `Cancelled` or `Idle`
   - Thread exits

3. **advanceExport returns false** (main thread, next frame)
   - Dialog detects export finished
   - Calls cleanup (delete partial file if needed)
   - Set `ExportState.phase` = `Idle`
   - Close dialog or show status

## File Relationships

```
┌─────────────────────────────────────────────────────────────┐
│                     MeshExportDialog                         │
│  ┌─────────────────────┐   ┌─────────────────────────────┐  │
│  │ CancellationToken   │   │ ExportState*                │  │
│  │ (owned)             │   │ (reference)                 │  │
│  └─────────┬───────────┘   └─────────────────────────────┘  │
│            │                                                 │
│            │ passes pointer                                  │
│            ▼                                                 │
│  ┌─────────────────────────────────────────────────────────┐│
│  │              Active IExporter                           ││
│  │  - ManifoldDualContouringStlExporter                    ││
│  │  - DualContouringStlExporter                            ││
│  │  - MeshExporter / MeshExporter3mf                       ││
│  │                                                         ││
│  │  ┌─────────────────────┐                               ││
│  │  │ CancellationToken*  │  (borrowed, checks in loops)  ││
│  │  └─────────────────────┘                               ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

## Validation Rules

| Rule | Entity | Description |
|------|--------|-------------|
| VR-001 | CancellationToken | Once cancelled, stays cancelled until reset() |
| VR-002 | ExportState | Phase transitions: Idle→Exporting→Idle, Exporting→Cancelling→Idle |
| VR-003 | IExporter | Token must be set before beginExport() or ignored |
| VR-004 | File cleanup | Partial file deleted only if export did not complete successfully |
