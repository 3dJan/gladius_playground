# Data Model: MCP Library Tools

## Entities

### LibraryCategory

A named subdirectory within the library root. Represents a thematic grouping of library entries.

| Field | Type | Description |
|---|---|---|
| name | string | Directory name (e.g., "primitives", "lattices") |
| path | filesystem path | Absolute path to the category directory |
| entryCount | integer | Number of `.3mf` files in the directory |
| isShipped | boolean | Whether this category exists in the shipped (read-only) library |
| isUser | boolean | Whether this category exists in the user library |

**Relationships**: Contains 0..N LibraryEntry instances.

**Validation**: Name must be a valid directory name (no path separators, no `.`, no `..`).

---

### LibraryEntry

A 3MF file within a category directory, containing one or more reusable SDF/implicit functions tagged via library metadata.

| Field | Type | Description |
|---|---|---|
| name | string | File name without extension (e.g., "gyroid") |
| category | string | Parent category name |
| path | filesystem path | Absolute path to the `.3mf` file |
| description | string | Human-readable description from `gladius:library-description` metadata |
| taggedFunctionIds | list of uint32 | Resource IDs from `gladius:library-functions` metadata |
| hasMetadata | boolean | Whether the file contains valid `gladius:library-functions` metadata |
| isShipped | boolean | Whether this entry is in the shipped (read-only) library directory |

**Relationships**: Belongs to exactly 1 LibraryCategory. Contains 1..N LibraryFunction (tagged for import).

**Validation**:
- Name must be filesystem-safe (alphanumeric, hyphens, underscores)
- File must be a valid 3MF archive
- If metadata is present, tagged function IDs must reference existing resources

**State transitions**: None — library entries are immutable once written. Updates create a replacement file.

---

### LibraryFunction

A volumetric implicit function within a library entry, identified by its model resource ID.

| Field | Type | Description |
|---|---|---|
| resourceId | uint32 | Model resource ID within the 3MF file |
| name | string | Function display name |
| type | string | Function type (e.g., "ImplicitFunction", "FunctionFromImage3D") |
| inputs | list of Parameter | Input parameters with names and types |
| outputs | list of Parameter | Output ports with names and types |

**Relationships**: Belongs to 1 LibraryEntry. May depend on other LibraryFunctions (transitive closure via function call nodes).

---

### Parameter

An input or output port on a function.

| Field | Type | Description |
|---|---|---|
| name | string | Parameter name (e.g., "pos", "radius") |
| type | string | Parameter type ("float", "vec3", "matrix4", "resourceid") |
| value | any (optional) | Default value if set on a constant node |

---

### LibraryMetadata

3MF model-level metadata used to tag library entries. This is the existing format.

| Field | Type | Metadata Key | Description |
|---|---|---|---|
| libraryFunctions | string | `gladius:library-functions` | Semicolon-separated model resource IDs |
| libraryDescription | string | `gladius:library-description` | Free-text description |

---

## Data Flow

### Creation Flow
1. Agent provides: name, category, expression (or function ID), description
2. System creates/opens a temporary 3MF model
3. System creates the function (from expression or by copying from current doc)
4. System writes LibraryMetadata to the model
5. System saves the 3MF to `<user-library>/<category>/<name>.3mf`
6. System removes metadata from the source model (if exporting from active doc)

### Import Flow
1. Agent provides: category, entry name
2. System opens the library 3MF into a temporary model
3. System reads LibraryMetadata to get tagged function IDs
4. System computes selective import closure (transitive dependencies)
5. System prunes temporary model to closure only
6. System merges pruned model into active document
7. System returns new resource IDs in the active document

### Listing Flow
1. Agent provides: optional category filter
2. System scans library directory (or specific category)
3. For each 3MF file, system reads metadata (fast: model-level only, no graph parsing)
4. System returns structured listing with names, descriptions, function IDs
