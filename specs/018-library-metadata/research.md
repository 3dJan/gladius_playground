# Research: Library Metadata for Selective Function Import

**Date**: 2026-02-14  
**Feature**: 018-library-metadata

## R1: lib3mf Metadata API

### Decision
Use model-level `CMetaDataGroup` to store library metadata under the `gladius` namespace.

### Rationale
- `CModel::GetMetaDataGroup()` is available on every model instance, including those opened during thumbnail scanning.
- `CMetaDataGroup::AddMetaData(namespace, name, value, type, mustPreserve)` supports custom namespaces natively.
- `GetMetaDataByKey(namespace, name)` **throws** if key not found (does NOT return nullptr). Must wrap in try/catch.
- Existing usage in Writer3mf already writes `("", "Application", "Gladius", "string", true)`.
- Model-level metadata persists in the 3MF file without requiring any 3MF extension.

### Alternatives Considered
- **Per-object metadata**: `CObject::GetMetaDataGroup()` exists, but implicit functions inherit from `CResource`, not `CObject`, so they don't have metadata groups.
- **Sidecar files**: `.json` alongside `.3mf` files. Rejected — adds complexity, risks going out of sync.

## R2: Resource ID Types in lib3mf

### Decision
Use `GetModelResourceID()` for the `gladius:library-functions` metadata values.

### Rationale
| Method | Scope | Stability |
|--------|-------|-----------|
| `GetModelResourceID()` | Within a single 3MF model | Stable across re-saves |
| `GetUniqueResourceID()` | Package-wide (container) | Changes on merge/re-open |
| `GetResourceID()` | Package-wide (deprecated alias) | Changes on merge/re-open |

- Model resource IDs correspond to the `<object id="...">` in the 3MF XML. They are user-facing and stable.
- Container-scoped "unique" resource IDs are assigned internally by lib3mf and shift during merge operations.
- Gladius internally uses `ResourceId` which maps to model resource IDs via `ResourceIdUtil.h`.

### Alternatives Considered
- **UniqueResourceID**: Not stable across file operations.
- **Display names**: Not required to be unique in 3MF.
- **UUID**: Not natively used by implicit functions in 3MF.

## R3: Selective Import Strategy

### Decision
Build `ResourceDependencyGraph` on the **source** model before merge, compute the dependency closure, then filter during `loadImplicitFunctionsFiltered`.

### Rationale
- `ResourceDependencyGraph` takes any `Lib3MF::PModel` — it works on source models, not just the target.
- `getAllRequiredResources()` computes transitive dependencies including image stacks and function-to-function references.
- Current `merge()` flow: MergeFromModel → detect duplicates → remove duplicates → loadImplicitFunctionsFiltered → rebuildResourceDependencyGraph.
- For selective import, we extend this: read metadata → build dep graph on source → compute closure → merge → remove non-closure resources in addition to duplicates → filter loading.
- The existing "skip by ID set" pattern in `loadImplicitFunctionsFiltered` can be extended from "skip duplicates" to "skip anything not in the dependency closure."

### Alternatives Considered
- **Pre-merge filtering** (remove from source model before merge): Risky — lib3mf's MergeFromModel may have undocumented behavior with partially stripped models.
- **Post-merge removal only**: The current removeUnusedResources() approach removes resources not used by any build item. Since we don't import build items in selective mode, the function resources won't be "unused" from lib3mf's perspective (they're not referenced by build items). We need explicit filtering based on our computed closure.

## R4: Export Wizard — Working Copy Strategy

### Decision
Use serialize-to-buffer + deserialize for creating an independent working copy of the 3MF model.

### Rationale
```cpp
auto writer = originalModel->QueryWriter("3mf");
std::vector<Lib3MF_uint8> buffer;
writer->WriteToBuffer(buffer);
auto workingCopy = wrapper->CreateModel();
auto reader = workingCopy->QueryReader("3mf");
reader->ReadFromBuffer(buffer);
// workingCopy is fully independent — mutate freely
```
- No disk I/O (in-memory buffer).
- Faithful deep copy including metadata and attachments.
- The working copy can be freely mutated (remove resources, add metadata) without affecting the live document.
- Writer3mf::save() mutates the live model via updateModel() — this confirms we cannot use it directly for "save without modifying original."

### Alternatives Considered
- **Save to temp file + reload**: Works but involves unnecessary disk I/O.
- **MergeFromModel into empty model**: May not copy all metadata/attachments identically.
- **Targeted clone (selective copy)**: Most flexible but highest complexity; unnecessary when we can just clone + prune.

## R5: Metadata Reading During Scan

### Decision
Read metadata eagerly in `ThreemfFileViewer::extractThumbnail()` (or a companion method) since the model is already loaded.

### Rationale
- `extractThumbnail()` already calls `reader->ReadFromFile(filePath)` and has the model open.
- Adding `model->GetMetaDataGroup()->GetMetaDataByKey("gladius", "library-functions")` is one API call.
- The `ThreemfFileInfo` struct needs two new fields: `std::string description` and `std::vector<std::string> libraryFunctionNames`.
- Function names for display are resolved by iterating model functions and matching model resource IDs from the metadata to `GetDisplayName()`.

### Alternatives Considered
- **Lazy loading on hover**: Adds complexity for negligible benefit since files are already opened.

## R6: UI Layout for Metadata Display

### Decision
Extend the file card height from `m_thumbnailSize + 40` to `m_thumbnailSize + 70` (approximately) to accommodate description text below the filename. Use tooltips for long descriptions.

### Rationale
- Current layout: thumbnail (m_thumbnailSize) + 40px for filename.
- Adding 1-2 lines of description text requires ~30px more.
- Function names can be shown as small labels or integrated into the description area.
- Descriptions truncated at 80 characters with `...`; full text in tooltip via `ImGui::SetItemTooltip()`.

### Alternatives Considered
- **Popup on hover**: More complex, delays information access.
- **Separate details panel**: Overengineered for the current use case.
