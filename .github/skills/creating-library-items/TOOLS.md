# Gladius MCP Tool Reference

Complete list of Gladius MCP tools. Load this file when you need the exact tool name for an
operation. Availability of the OpenCL-backed tools depends on the build variant
— see [SKILL.md](SKILL.md) "Build variants: pure WebGPU MCP builds".

## Complete Tool Reference

### Document Management

| Tool | Purpose |
|------|---------|
| `get_status` | Server status, loaded document info, `server_time` for change tracking |
| `create_document` | New document from template |
| `open_document` | Open an existing `.3mf` file |
| `save_document` | Save current document |
| `save_document_as` | Save current document to a new path |
| `get_3mf_structure` | Full document structure: resources, build items, meshes, function arguments, output types, snippet previews, constants |

### Function Authoring

| Tool | Purpose |
|------|---------|
| `get_program_snippet` | Read all functions as GLSL-like code |
| `set_program_snippet` | Write/update multiple functions at once |
| `get_function_snippet` | Read one function's code by ID |
| `set_function_snippet` | Write one function's code by ID |
| `create_function_from_snippet` | Create new function from multi-line code |
| `create_function_from_expression` | Create from a single math expression |
| `create_image3d_function` | Create a function from 3D image data (FunctionFromImage3D) |
| `create_levelset` | Create a new levelset resource |
| `modify_levelset` | Modify levelset properties |
| `set_parameter` | Set a function parameter value |
| `create_volumetric_color` | Create a volumetric color function |
| `create_volumetric_property` | Create a volumetric property function |

### Evaluation & Validation

| Tool | Purpose |
|------|---------|
| `evaluate_function` | Run a function on sample points via OpenCL (returns float or vec3 results) |
| `validate_model` | Check graph connectivity + OpenCL compilation |
| `get_model_bounding_box` | Get the model's world-space bounding box |

### Rendering

| Tool | Purpose |
|------|---------|
| `render_to_file` | Render to PNG with auto camera |
| `render_with_camera` | Render with explicit camera position |
| `get_optimal_camera_position` | Compute camera position that frames the model |
| `generate_thumbnail` | Generate a 256px preview thumbnail |
| `capture_screenshot` | Capture the current viewport (UI mode only) |

### Build Items

| Tool | Purpose |
|------|---------|
| `set_build_item_object` | Set which resource a build item references |
| `set_build_item_transform` | Set a build item's transformation matrix |

### Library Management

| Tool | Purpose |
|------|---------|
| `create_library_entry` | One-step: create entry from snippet with scaffold, thumbnail, and metadata |
| `export_to_library` | Export current document's function to library |
| `import_library_entry` | Import a library entry into the current document |
| `list_library` | List categories/entries with optional query filter |
| `get_library_entry_info` | Inspect entry metadata, function info, tags, and snippet |
| `set_library_metadata` | Update entry description and tags (pass `category`/`name` to target a library entry directly) |
| `delete_library_entry` | Remove a library entry |

### Change Tracking

| Tool | Purpose |
|------|---------|
| `get_changes_since` | List resources changed since a given ISO-8601 timestamp |

### Utility

| Tool | Purpose |
|------|---------|
| `remove_unused_resources` | Clean up unreferenced resources |

### UI Interaction (UI mode only)

| Tool | Purpose |
|------|---------|
| `ui_click` | Click a UI element by name |
| `ui_dump_windows` | List all open UI windows |
| `ui_dump_items` | List UI items in a window |
