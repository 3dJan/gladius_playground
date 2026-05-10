# Research: Library Item Deletion with Bin Recovery

**Date**: 2026-04-14  
**Feature**: [spec.md](spec.md) | [plan.md](plan.md)

## Research 1: Shipped Entry Detection for Synced Copies

### Problem

`resolveEntryPath()` determines `isShipped` based on filesystem location:
- Found in user dir → `isShipped = false`
- Found in shipped dir → `isShipped = true`

When `syncShippedLibrary()` copies shipped files to the user directory on first launch,
the copies become indistinguishable from user-created entries. FR-013 requires these
synced copies to remain protected.

### Decision: Check if corresponding shipped file exists

When `resolveEntryPath()` finds a file in the user directory, additionally check whether
a file with the same relative path exists in the shipped directory. If it does, and the
file content is identical (same file size as a fast heuristic, or exact byte comparison for
certainty), mark it as shipped.

**Algorithm** (modify `resolveEntryPath()`):
1. Find file in user dir → `userPath`
2. Compute `shippedPath = getShippedLibraryDir() / category / fileName`
3. If `shippedPath` exists → `isShipped = true` (this is a synced copy or the original)
4. If `shippedPath` does not exist → `isShipped = false` (user-created)

Step 3 uses the simple heuristic: if the shipped dir has a file with the same name in
the same category, the user-dir copy is a synced shipped entry. This works because:
- `syncShippedLibrary()` never overwrites — user can't modify a synced file and have it
  still match the shipped name (if they did, it was their choice to shadow it)
- If a user deliberately creates an entry with the same name as a shipped entry,
  the shipped entry already exists in the shipped dir, so shadowing is intentional.
  In this case, the "is parallel to shipped" check is actually correct — the user's copy
  shadows a shipped entry, and deleting it would just reveal the shipped one underneath.
  This is acceptable behavior.

### Rationale

- Zero storage overhead (no metadata files, no database, no hashes)
- Zero migration cost (works with existing library data)
- O(1) per check (single `fs::exists()` call)
- Conservative: if in doubt, protects the entry

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|-----------------|
| Metadata sidecar file in user dir listing synced entries | Adds sync/migration complexity; must stay consistent across updates |
| Hash comparison (SHA-256 of shipped vs user file) | Slow for many entries; overkill when name+category match is sufficient |
| Marker inside 3MF metadata during sync | Requires modifying 3MF files during sync; lib3mf overhead on startup |

---

## Research 2: Hiding `.bin/` from Library Browsing

### Problem

`getAvailableCategories()` iterates all subdirectories under the user library root.
A `.bin/` folder would appear as a category.

### Decision: Skip dot-prefixed directories

Filter `getAvailableCategories()` and `getAvailableEntries()` to skip directories
whose name starts with `.` (dot). This follows the Unix hidden-file convention and
naturally excludes `.bin/` without hardcoding the name.

**Implementation**: Add a single guard in the `addCategories` lambda:
```cpp
if (name.empty() || name[0] == '.')
    continue;
```

### Rationale

- Minimal code change (one line per scan function)
- Follows platform conventions
- Future-proof: any other hidden metadata folders (`.cache`, `.tmp`) would also be excluded

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|-----------------|
| Hardcoded exclusion of `.bin` | Fragile; doesn't handle other hidden folders |
| Separate bin directory outside library root | Breaks the convention of keeping everything under the user library; complicates restore |

---

## Research 3: ImGui Right-Click Context Menu Patterns

### Problem

`ThreemfFileViewer::renderThumbnailItem()` currently handles click (select), double-click
(import), drag-drop, and hover tooltip. No right-click context menu exists.

### Decision: Use `ImGui::OpenPopup()` + `ImGui::BeginPopup()` pattern

After the `ImGui::Button()` call for each thumbnail, check for right-click and open a
popup:

```cpp
// After ImGui::Button() for the thumbnail
if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
{
    ImGui::OpenPopup(popupId);
    m_selectedForContext = &thumbnailInfo;
}

if (ImGui::BeginPopup(popupId))
{
    if (ImGui::MenuItem("Delete"))
    {
        // invoke delete callback
    }
    ImGui::EndPopup();
}
```

The popup ID must be unique per thumbnail (e.g., `fmt::format("##ctx_{}", thumbnailInfo.fileName)`).

A callback (`std::function<void(std::string const&)>`) will be passed from `LibraryBrowser`
to `ThreemfFileViewer` to handle the actual delete operation.

### Rationale

- Standard ImGui pattern used throughout the codebase
- No dependency on ImGui test engine or advanced features
- Context menu cleanly separates UI trigger from business logic via callback

---

## Research 4: Filename Disambiguation Algorithm

### Problem

When soft-deleting an entry to the bin, the same filename may already exist in the bin
(e.g., user deletes `sphere.3mf`, modifies something, creates and deletes another `sphere.3mf`).
The bin must never overwrite.

### Decision: Numeric suffix with incrementing counter

```
sphere.3mf         (first)
sphere_1.3mf       (second)
sphere_2.3mf       (third)
...
```

**Algorithm**:
1. Target path = `.bin/<category>/<name>.3mf`
2. If target path does not exist → use it
3. Else try `<name>_1.3mf`, `<name>_2.3mf`, ... up to `<name>_999.3mf`
4. If all 999 slots are taken → return error (practically impossible)

Same algorithm is reused for restore conflicts (FR-009).

### Rationale

- Human-readable filenames
- Simple implementation (~10 lines)
- Consistent with the restore conflict resolution defined in the spec

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|-----------------|
| Timestamp suffix (`sphere_20260414_153000.3mf`) | Harder to read; doesn't guarantee uniqueness within same second |
| UUID suffix (`sphere_a1b2c3.3mf`) | Not human-readable |
| Overwrite oldest | Data loss risk; violates FR-005 |

---

## Research 5: Bin Tab UI Integration in LibraryBrowser

### Problem

Users need to browse, restore, and empty the bin. Where does this UI live?

### Decision: Add a "Bin" tab in the `LibraryBrowser` tab bar

`LibraryBrowser` already uses `ImGui::BeginTabBar()` with one tab per category (each
backed by a `ThreemfFileViewer`). Add a special "Bin" tab at the end that:
- Lists all entries across all bin categories in a flat or category-grouped view
- Each entry shows a "Restore" button and a "Delete permanently" button
- An "Empty Bin" button in the tab header area

The bin tab reuses `ThreemfFileViewer` pointed at `.bin/` but with a different context
menu (restore + permanent delete instead of normal delete).

### Rationale

- Reuses existing tab and viewer infrastructure
- Keeps bin browsing within the same UI panel the user already knows
- No new windows or dialogs needed for browsing

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|-----------------|
| Separate window/dialog for bin | Extra UI complexity; breaks the single-panel library browsing pattern |
| File manager integration (open OS file manager) | Not cross-platform; no restore workflow integration |
