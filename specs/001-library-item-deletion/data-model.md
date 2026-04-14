# Data Model: Library Item Deletion with Bin Recovery

**Date**: 2026-04-14  
**Feature**: [spec.md](spec.md) | [plan.md](plan.md)

## Entities

### LibraryEntry (existing — no schema change)

A `.3mf` file representing a reusable implicit function or primitive.

| Attribute | Type | Description |
|-----------|------|-------------|
| name | string | Filename stem (e.g., `sphere`) |
| category | string | Subdirectory name (e.g., `primitives`, `csg`) |
| filePath | `std::filesystem::path` | Absolute path to `.3mf` file on disk |
| isShipped | bool | Whether entry originates from shipped library (read-only) |
| description | string | Human-readable description (from 3MF model metadata) |
| libraryFunctions | vector\<string\> | Semicolon-separated function IDs (from 3MF model metadata) |

**Shipped detection rule** (updated per Research 1):
- `isShipped = true` if `getShippedLibraryDir() / category / (name + ".3mf")` exists on disk
- `isShipped = false` otherwise

### Bin (new — filesystem-only, no code entity)

A hidden directory `.bin/` inside the user library root that mirrors the category subfolder structure.

| Aspect | Value |
|--------|-------|
| Location | `getUserLibraryDir() / ".bin"` → `~/.local/share/gladius/library/.bin/` |
| Structure | `.bin/<category>/<name>.3mf` — mirrors active library layout |
| Visibility | Hidden from `getAvailableCategories()` by dot-prefix filter |
| Creation | Lazy — created on first soft-delete |
| Cleanup | Manual — user empties via UI or MCP tool |

### BinEntry (logical — same file format as LibraryEntry)

An entry in the bin is just a `.3mf` file under `.bin/<category>/`. No additional metadata
is stored; the category is inferred from the subfolder.

| Attribute | Type | Source |
|-----------|------|--------|
| name | string | Filename stem |
| category | string | Parent directory name under `.bin/` |
| filePath | `std::filesystem::path` | Absolute path under `.bin/` |

## Relationships

```
LibraryEntry --[soft-delete]--> BinEntry
  - Moves .3mf file from <userLib>/<category>/ to <userLib>/.bin/<category>/
  - Disambiguates filename if collision exists

BinEntry --[restore]--> LibraryEntry
  - Moves .3mf file from <userLib>/.bin/<category>/ back to <userLib>/<category>/
  - Disambiguates filename if collision exists

BinEntry --[permanent delete]--> (removed from disk)
  - fs::remove() — irreversible
```

## Filesystem Layout Example

```
~/.local/share/gladius/library/
├── primitives/
│   ├── sphere.3mf          ← shipped (synced copy)
│   ├── cube.3mf            ← shipped (synced copy)
│   └── my_custom.3mf       ← user-created (deletable)
├── csg/
│   ├── union.3mf           ← shipped
│   └── my_combiner.3mf     ← user-created
├── mechanical/
│   └── gear.3mf            ← shipped
└── .bin/                    ← hidden from library browsing
    ├── primitives/
    │   ├── old_shape.3mf   ← soft-deleted
    │   └── old_shape_1.3mf ← second deletion with same name
    └── csg/
        └── my_test.3mf     ← soft-deleted
```

## State Transitions

```
                    ┌──────────┐
          create    │  Active  │    (visible in library)
         ─────────> │  Entry   │
                    └────┬─────┘
                         │ soft-delete (move to .bin/)
                         v
                    ┌──────────┐
                    │  Binned  │    (visible only in bin tab)
                    │  Entry   │
                    └──┬───┬───┘
                       │   │
            restore    │   │ permanent delete
       (move back)     │   │ (fs::remove)
                       v   v
                 ┌────────┐  ┌─────────┐
                 │ Active │  │ Removed │
                 │ Entry  │  │ (gone)  │
                 └────────┘  └─────────┘
```

## Validation Rules

- **Soft-delete**: Rejected if `isShipped == true`
- **Restore**: Always allowed; disambiguates name on conflict
- **Permanent delete**: Requires confirmation prompt in UI; MCP tool does not require confirmation (agents are trusted)
- **Browse bin**: Always allowed; empty bin shows informational message
- **Empty bin**: Requires confirmation prompt in UI
