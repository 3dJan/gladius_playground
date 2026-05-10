# Quickstart: Library Item Deletion with Bin Recovery

## What Changed

The library system now supports **soft-delete with bin recovery** for user-created entries.
Deleting a library entry moves it to a hidden `.bin/` folder instead of permanently removing it.
Shipped (default) entries remain protected.

## How to Use

### Delete a Library Entry (UI)

1. Open the Library Browser panel
2. Right-click on a user-created library entry thumbnail
3. Select **Delete** from the context menu
4. The entry disappears from the library and a notification confirms it was moved to the bin

Shipped entries will show a grayed-out Delete option with a tooltip explaining they are protected.

### Delete via MCP Tool

```json
{
  "method": "tools/call",
  "params": {
    "name": "delete_library_entry",
    "arguments": {
      "category": "primitives",
      "name": "my_custom_shape"
    }
  }
}
```

### Browse the Bin (UI)

1. In the Library Browser, click the **Bin** tab (last tab)
2. View all deleted entries grouped by their original category
3. If the bin is empty, a message indicates so

### Browse via MCP Tool

```json
{
  "method": "tools/call",
  "params": {
    "name": "browse_bin",
    "arguments": {}
  }
}
```

### Restore a Deleted Entry (UI)

1. Go to the **Bin** tab in the Library Browser
2. Right-click on the entry you want to restore
3. Select **Restore**
4. The entry reappears in its original category

If another entry with the same name now exists, the restored entry is automatically
renamed with a numeric suffix (e.g., `sphere_1.3mf`).

### Restore via MCP Tool

```json
{
  "method": "tools/call",
  "params": {
    "name": "restore_bin_entry",
    "arguments": {
      "category": "primitives",
      "name": "my_custom_shape"
    }
  }
}
```

### Permanently Empty the Bin (UI)

1. Go to the **Bin** tab
2. Click the **Empty Bin** button
3. Confirm the action in the dialog (this is irreversible)

### Empty via MCP Tool

```json
{
  "method": "tools/call",
  "params": {
    "name": "empty_bin",
    "arguments": {}
  }
}
```

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Bin mirrors category subfolders (`.bin/primitives/`, `.bin/csg/`) | Human-browsable on disk; handles cross-category name collisions naturally |
| Dot-prefixed `.bin/` folder | Hidden from normal library scanning by Unix convention |
| No confirmation before soft-delete | Recoverable action — bin is the safety net |
| Confirmation before permanent delete | Irreversible — prevents accidental data loss |
| Shipped detection via shipped-dir existence check | Zero overhead; works without metadata |
| Numeric suffix for name conflicts | Simple, human-readable (`sphere_1.3mf`, `sphere_2.3mf`) |

## Filesystem Layout

```
~/.local/share/gladius/library/
├── primitives/          ← active library entries
│   ├── sphere.3mf       (shipped — protected)
│   └── my_shape.3mf     (user — deletable)
├── csg/
│   └── union.3mf        (shipped — protected)
└── .bin/                 ← soft-deleted entries (hidden)
    └── primitives/
        └── old_shape.3mf
```
