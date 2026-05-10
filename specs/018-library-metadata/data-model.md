# Data Model: Library Metadata for Selective Function Import

**Date**: 2026-02-14  
**Feature**: 018-library-metadata

## Entities

### LibraryMetadata (stored in 3MF model-level metadata)

| Field | Type | 3MF Key | Description |
|-------|------|---------|-------------|
| libraryFunctions | string | `gladius:library-functions` | Semicolon-separated model resource IDs (e.g., `"5;12"`) |
| libraryDescription | string | `gladius:library-description` | Free-text description |

**Storage**: 3MF model-level `MetaDataGroup`. Written via `CMetaDataGroup::AddMetaData("gladius", key, value, "xs:string", true)`.

**Reading**: Via `CMetaDataGroup::GetMetaDataByKey("gladius", key)`. Throws if not found — use try/catch.

### ThreemfFileInfo (in-memory, extends existing struct)

| Field | Type | Source | Description |
|-------|------|--------|-------------|
| filePath | `std::filesystem::path` | Existing | Path to the 3MF file |
| fileName | `std::string` | Existing | File name without path |
| thumbnailTextureId | `unsigned int` | Existing | OpenGL texture ID |
| thumbnailData | `std::vector<unsigned char>` | Existing | Raw PNG data |
| thumbnailWidth | `unsigned int` | Existing | Thumbnail width |
| thumbnailHeight | `unsigned int` | Existing | Thumbnail height |
| hasThumbnail | `bool` | Existing | Has thumbnail? |
| thumbnailLoaded | `bool` | Existing | Thumbnail loaded? |
| **description** | `std::string` | **New** | From `gladius:library-description` |
| **libraryFunctionNames** | `std::vector<std::string>` | **New** | Display names resolved from resource IDs |
| **hasLibraryMetadata** | `bool` | **New** | Whether `gladius:library-functions` was found |

### LibraryExportConfig (new, used by export wizard)

| Field | Type | Description |
|-------|------|-------------|
| selectedFunctionIds | `std::vector<ResourceId>` | Model resource IDs of functions to export |
| description | `std::string` | User-entered description |
| categoryName | `std::string` | Target subfolder name |
| buildItemIndex | `int` | Which build item to keep as example (-1 = auto) |
| fileName | `std::string` | Output file name (without path) |

## Relationships

```
LibraryMetadata (in 3MF file)
  └──► identifies functions by ModelResourceID
         └──► ResourceDependencyGraph resolves transitive dependencies
                └──► dependency closure = set of resource IDs to import

ThreemfFileInfo (in-memory scan result)
  └──► libraryFunctionNames = display names resolved from LibraryMetadata IDs
  └──► hasLibraryMetadata gates selective vs. full merge behavior

LibraryExportConfig (wizard input)
  └──► selectedFunctionIds → written as LibraryMetadata.libraryFunctions
  └──► description → written as LibraryMetadata.libraryDescription
  └──► categoryName → determines output subdirectory under library root
```

## Validation Rules

- Model resource IDs in `gladius:library-functions` MUST reference existing implicit functions or FunctionFromImage3D resources in the file. If a referenced ID does not exist, fall back to full merge and log a warning.
- Description is optional. Empty string or missing key means no description.
- Semicolons in `gladius:library-functions` separate integer IDs. Whitespace around IDs is trimmed.
- At least one function ID MUST be present in `gladius:library-functions` for selective import to activate.

## State Transitions

```
3MF file on disk
  ──[scan directory]──► ThreemfFileInfo (with metadata fields populated)
  ──[double-click]──► merge() or mergeSelective() depending on hasLibraryMetadata

Document (live)
  ──[export wizard]──► working copy (buffer clone)
  ──[remove unused]──► pruned copy
  ──[add metadata]──► stamped copy
  ──[write to file]──► library .3mf file on disk
```
