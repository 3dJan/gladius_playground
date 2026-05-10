# Data Model: Async Shell-Based Color Export

**Feature**: 001-async-shell-export  
**Date**: 2026-01-07  

## Entity Diagram

```text
┌─────────────────────────────────────────────────────────────────┐
│                        MeshExportDialog                         │
│  ┌─────────────────┐   ┌─────────────────┐                      │
│  │ m_shellExporter │──▶│  ShellExporter  │◀── NEW               │
│  └─────────────────┘   └────────┬────────┘                      │
│                                 │                               │
│  ┌─────────────────┐           │ implements                    │
│  │ m_exportState ──┼───────────┼─────────────────────┐          │
│  └─────────────────┘           │                     │          │
│                                ▼                     ▼          │
│                        ┌───────────────┐    ┌──────────────┐    │
│                        │   IExporter   │    │ ExportState  │    │
│                        │  (interface)  │    │  (existing)  │    │
│                        └───────────────┘    └──────────────┘    │
│                                │                                │
│  ┌─────────────────┐          │ uses                           │
│  │ m_cancellation  │──────────┼─────────────┐                   │
│  │     Token       │          │             │                   │
│  └─────────────────┘          ▼             ▼                   │
└──────────────────────────────────────────────────────────────────┘
                                │
                                │ delegates to
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                       ShellExporter                             │
│                                                                 │
│  State Machine:                                                 │
│  ┌──────┐  beginExport()  ┌─────────┐                          │
│  │ Idle │────────────────▶│ Running │                          │
│  └──────┘                 └────┬────┘                          │
│      ▲                         │                               │
│      │    ┌────────────────────┼────────────────────┐          │
│      │    │                    │                    │          │
│      │    ▼                    ▼                    ▼          │
│  ┌───────────┐          ┌───────────┐        ┌──────────┐      │
│  │ Cancelled │          │ Completed │        │  Failed  │      │
│  └───────────┘          └───────────┘        └──────────┘      │
│                                                                 │
│  Members:                                                       │
│  ├── m_state: atomic<State>                                    │
│  ├── m_progress: atomic<double>                                │
│  ├── m_statusMessage: string                                   │
│  ├── m_errorMessage: string                                    │
│  ├── m_exportFuture: future<void>                              │
│  ├── m_targetFile: path                                        │
│  ├── m_config: ShellExportConfig                               │
│  └── m_cancellationToken: CancellationToken*                   │
│                                                                 │
│  Relationships:                                                 │
│  ├──▶ ShellGenerator (creates shells)                          │
│  ├──▶ MeshWriter3mf (writes 3MF)                               │
│  └──▶ ComputeCore (GPU compute access)                         │
└─────────────────────────────────────────────────────────────────┘
```

## New Entities

### ShellExporter

Encapsulates the async shell export operation, implementing the `IExporter` interface.

| Attribute | Type | Description |
|-----------|------|-------------|
| m_state | `std::atomic<State>` | Current export state (Idle/Running/Completed/Failed/Cancelled) |
| m_progress | `std::atomic<double>` | Progress 0.0–1.0 |
| m_statusMessage | `std::string` | Current status for UI (e.g., "Generating shell 2/5...") |
| m_errorMessage | `std::string` | Error details if state is Failed |
| m_exportFuture | `std::future<void>` | Handle to background thread |
| m_targetFile | `std::filesystem::path` | Output file path |
| m_config | `ShellExportConfig` | Export configuration (materials, LUTs, options) |
| m_document | `Document const*` | Document for thumbnail and metadata |
| m_logger | `events::SharedLogger` | Event logging |

### ShellExportConfig

Groups all parameters needed for shell export (avoids long parameter lists).

| Attribute | Type | Description |
|-----------|------|-------------|
| filamentStack | `FilamentStack` | Ordered filament materials (bottom to top) |
| precomputedLuts | `std::vector<std::vector<float>>` | Thickness LUTs per layer |
| lutResolution | `int` | LUT grid resolution (e.g., 16) |
| thicknessConstraints | `ThicknessConstraints` | Min/max thickness |
| mdcOptions | `ManifoldDualContouringOptions` | Mesh extraction options |

## State Transitions

| Current State | Event | Next State | Action |
|--------------|-------|------------|--------|
| Idle | `beginExport()` | Running | Launch async worker |
| Running | Worker completes | Completed | Set progress to 1.0 |
| Running | Worker throws | Failed | Store error message |
| Running | `isCancellationRequested()` | Cancelled | Clean exit, no file |
| Completed | `advanceExport()` returns false | (terminal) | UI shows success |
| Failed | `advanceExport()` returns false | (terminal) | UI shows error |
| Cancelled | `advanceExport()` returns false | (terminal) | UI shows cancelled |
| Any terminal | `beginExport()` | Running | Reset and start new |

## Modified Entities

### MeshExportDialog

| Modification | Description |
|--------------|-------------|
| Add `m_shellExporter` | New member: `io::ShellExporter` instance |
| Modify `exportShellsTo3mf()` | Replace sync code with `m_shellExporter.beginExport()` |
| Modify `render()` | Use `m_shellExporter.getProgress()` for progress bar |
| Modify `startExport()` | Set `m_activeExporter = &m_shellExporter` for shell exports |

## Validation Rules

| Rule | Entity | Description |
|------|--------|-------------|
| VR-001 | ShellExporter | `beginExport()` requires non-empty filament stack |
| VR-002 | ShellExporter | `beginExport()` requires precomputed LUTs if LUT resolution > 1 |
| VR-003 | ShellExporter | Target file path must be writable |
| VR-004 | ShellExportConfig | `lutResolution` must be ≥ 2 if using variable thickness |
