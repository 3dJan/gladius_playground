# Data Model: Function Deduplication

**Feature**: 014-dedupe-functions  
**Date**: 2026-01-24

## Core Entities

### FunctionalEquality

Compares two `Model` instances for mathematical equivalence.

```
FunctionalEquality
├── computeHash(Model) → size_t
├── areEqual(Model, Model) → bool
└── compareNodes(NodeBase, NodeBase, NodeMapping) → bool
```

**Key Properties**:
- Ignores: node IDs, node names, display names, port IDs
- Compares: node types, graph topology, constant values (with epsilon), resource references
- Uses topological ordering for deterministic comparison

### FunctionDeduplicator

Orchestrates the deduplication process across an Assembly.

```
FunctionDeduplicator
├── findDuplicateGroups(Assembly) → vector<DuplicateGroup>  
├── deduplicate(Assembly, History) → DeduplicationResult
└── selectCanonical(DuplicateGroup) → ResourceId
```

**DuplicateGroup**:
```
DuplicateGroup
├── members: vector<ResourceId>     // All functions in this equivalence class
├── canonical: ResourceId           // The one to keep
└── toRemove: vector<ResourceId>    // Members minus canonical
```

**DeduplicationResult**:
```
DeduplicationResult
├── removedCount: size_t            // Number of functions removed
├── updatedReferences: size_t       // Number of references updated
├── groups: vector<DuplicateGroup>  // For reporting/UI
```

### ReferenceUpdater

Updates all references when a function is removed.

```
ReferenceUpdater
├── collectReferences(Assembly, ResourceId) → ReferenceSet
├── updateReferences(Assembly, ResourceId oldId, ResourceId newId) → void
└── updateExternalReferences(Document, oldId, newId) → void
```

**Reference Types**:
1. `FunctionCall` nodes in any Model
2. `FunctionGradient` nodes in any Model  
3. Level set `functionid` attributes (via Lib3MF)
4. VolumeData color/property function references (via Lib3MF)

## Relationships

```
┌─────────────────┐
│    Assembly     │
│  (Models map)   │
└────────┬────────┘
         │ contains
         ▼
┌─────────────────┐     ┌───────────────────┐
│      Model      │────▶│  FunctionalEquality │
│ (function graph)│     │   (comparison)      │
└────────┬────────┘     └───────────────────┘
         │ contains
         ▼
┌─────────────────┐
│    NodeBase     │
│(Addition, etc.) │
└────────┬────────┘
         │ may be
         ▼
┌─────────────────┐
│  FunctionCall   │──────▶ references ResourceId
└─────────────────┘
```

## State Transitions

### Deduplication Flow

```
Initial State                    After Deduplication
─────────────────────────────    ─────────────────────────────
Assembly                         Assembly
├── func_A (ResourceId=1)        ├── func_A (ResourceId=1) ✓ KEPT
│   └── hash: 0xABC              │
├── func_B (ResourceId=2)        └── func_B (ResourceId=2) ✓ KEPT
│   └── hash: 0xDEF                  (FunctionCall updated: 3→1)
├── func_C (ResourceId=3)        
│   └── hash: 0xABC  ← same!     func_C REMOVED
└── Model with FunctionCall→3    
```

## Validation Rules

1. **Assembly model protection**: `assembly.getAssemblyModelId()` must never be removed
2. **Reference integrity**: No dangling `FunctionCall` references after deduplication
3. **Determinism**: Same input assembly always produces same output
4. **Idempotence**: Running deduplication twice produces same result as once

## Hash Computation

### Included in Hash
| Element | Hash Contribution |
|---------|-------------------|
| Node count | Direct value |
| Node type (per node) | Type name hash |
| Input port count | Direct value |
| Constant value | Float hash (quantized) |
| Resource reference | ResourceId value |

### Excluded from Hash
| Element | Reason |
|---------|--------|
| NodeId | Implementation detail |
| Node name | Decorational |
| Display name | Decorational |
| PortId | Implementation detail |
| Function name | Decorational |
