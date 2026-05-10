# Data Model: MDC Export Progress

**Date**: 2026-01-06

## Overview

This feature introduces minimal new data structures - primarily callback types and progress tracking state.

## New Types

### MeshGenerationProgressCallback

A callback type for receiving progress updates during mesh generation.

```
Type: Function callback
Parameters:
  - progress: float (0.0 to 1.0)
  - phase: string (optional human-readable phase name)
Thread Safety: Must be safe to call from background thread
Lifetime: Valid for duration of generateMesh() call
```

### Export Phases

The progress range [0.0, 1.0] is divided into phases:

| Phase | Start | End | Description |
|-------|-------|-----|-------------|
| Initialization | 0.00 | 0.05 | Bbox computation, kernel loading |
| Octree Construction | 0.05 | 0.25 | Building octree structure |
| Mesh Generation | 0.25 | 0.65 | Vertex and index generation |
| Post-processing | 0.65 | 0.80 | Sharp features, simplification |
| Color Sampling | 0.80 | 0.90 | Per-face color sampling (3MF only) |
| File Writing | 0.90 | 1.00 | STL/3MF file output |

## Modified Entities

### ManifoldDualContouringGpu

**New members:**
- `m_progressCallback`: Stored callback function
- Phase-relative progress calculation in each major method

### ManifoldDualContouringStlExporter

**Modified members:**
- `m_progress`: Already exists (std::atomic<double>)
- Wire callback from ManifoldDualContouringGpu to update m_progress

## State Diagram

```
[Idle] --beginExport()--> [Initializing (0-5%)]
                              |
                              v
                         [Building Octree (5-25%)]
                              |
                              v
                         [Generating Mesh (25-65%)]
                              |
                              v
                         [Post-processing (65-80%)]
                              |
                              v
                         [Color Sampling (80-90%)] (3MF only)
                              |
                              v
                         [Writing File (90-100%)]
                              |
                              v
                         [Completed/Failed]
```

## Invariants

1. Progress MUST be monotonically increasing (never go backwards)
2. Progress MUST reach 1.0 only on successful completion
3. Progress MUST remain at last value on error (not reset to 0)
4. Callbacks MUST NOT throw exceptions
