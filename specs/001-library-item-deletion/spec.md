# Feature Specification: Library Item Deletion with Bin Recovery

**Feature Branch**: `001-library-item-deletion`  
**Created**: 2025-04-14  
**Status**: Draft  
**Input**: User description: "As a user I want to be able to delete items from the 3mf library. The removed items should be moved to a bin folder, just in case I regret my decision. The deletion should only affect the library stored in my user profile (not the 3mf files shipped with gladius that are copied initially)"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Delete a User Library Entry (Priority: P1)

As a user, I want to delete a library entry I previously created or customized so that my library stays organized and free of items I no longer need. The entry should be moved to a bin folder rather than permanently destroyed, so I can recover it if I change my mind.

**Why this priority**: This is the core feature — without the ability to delete entries safely, the library grows indefinitely and becomes hard to manage. The soft-delete to a bin folder is the main safety net that differentiates this from a destructive delete.

**Independent Test**: Can be fully tested by creating a user library entry, deleting it, verifying it disappears from the library listing, and confirming the file exists in the bin folder.

**Acceptance Scenarios**:

1. **Given** a user-created library entry exists in a category, **When** the user deletes the entry, **Then** the entry is removed from the library listing and moved to a bin folder within the user library directory.
2. **Given** a user-created library entry exists, **When** the user deletes it, **Then** the application shows a brief confirmation indicating the entry was moved to the bin.
3. **Given** the bin folder does not yet exist, **When** the user deletes their first entry, **Then** the bin folder is created automatically and the entry is moved into it.
4. **Given** an entry with the same filename already exists in the bin, **When** the user deletes another entry with that name, **Then** the existing bin entry is not overwritten (the new entry is stored with a disambiguated name).

---

### User Story 2 - Shipped Library Entries Are Protected (Priority: P1)

As a user, I want shipped (default) library entries to be protected from deletion so that I cannot accidentally remove the built-in library items that came with Gladius.

**Why this priority**: Equal priority to deletion itself — protecting shipped content is a hard constraint from the user's requirement. Without it, users could break their default library.

**Independent Test**: Can be tested by attempting to delete a shipped library entry and verifying it is rejected with a clear message.

**Acceptance Scenarios**:

1. **Given** a shipped library entry, **When** the user attempts to delete it, **Then** the system refuses and displays a message explaining that shipped entries cannot be deleted.
2. **Given** a shipped entry that was synced to the user profile on first launch, **When** the user attempts to delete it, **Then** the system recognizes it as a shipped entry and prevents deletion.

---

### User Story 3 - Restore a Deleted Library Entry (Priority: P2)

As a user, I want to restore a previously deleted library entry from the bin back to its original category so that I can recover items I deleted by mistake.

**Why this priority**: Natural complement to deletion — the bin is only useful if items can be recovered. However, the bin itself is already a safety net (users can manually move files back), so a UI-driven restore is a secondary convenience.

**Independent Test**: Can be tested by deleting a user entry, then restoring it from the bin, and verifying it reappears in the original library category.

**Acceptance Scenarios**:

1. **Given** a deleted entry exists in the bin, **When** the user restores it, **Then** the entry reappears in its original library category.
2. **Given** a deleted entry exists in the bin and a new entry with the same name now exists in that category, **When** the user restores it, **Then** the system restores it with a numeric suffix (e.g., `sphere_1.3mf`) so both entries coexist.

---

### User Story 4 - Browse Bin Contents (Priority: P2)

As a user, I want to see what items are in my bin so that I know what I can restore or what is consuming space.

**Why this priority**: Supports the restore workflow; users need visibility into what's in the bin before they can choose to restore or permanently discard items.

**Independent Test**: Can be tested by deleting several entries and browsing the bin to confirm all deleted items are listed.

**Acceptance Scenarios**:

1. **Given** the bin contains one or more deleted entries, **When** the user opens or browses the bin, **Then** all deleted entries are listed with their original category and name.
2. **Given** the bin is empty, **When** the user browses the bin, **Then** a message indicates the bin is empty.

---

### User Story 5 - Permanently Discard Bin Contents (Priority: P3)

As a user, I want to permanently delete items from the bin (or empty the entire bin) to free up disk space when I am certain I no longer need them.

**Why this priority**: Lower priority quality-of-life feature. The bin may accumulate items over time, but disk space pressure is typically low for these small 3MF files.

**Independent Test**: Can be tested by emptying the bin and verifying the files are permanently removed from disk.

**Acceptance Scenarios**:

1. **Given** the bin contains entries, **When** the user permanently deletes a single entry from the bin, **Then** it is removed from disk.
2. **Given** the bin contains entries, **When** the user empties the entire bin, **Then** all entries are permanently removed.
3. **Given** the user triggers a permanent delete, **When** the action executes, **Then** a confirmation prompt is shown before irreversible deletion.

---

### Edge Cases

- What happens when the filesystem cannot move the entry to the bin (e.g., permission error or disk full)? The operation should fail gracefully, the original file should remain intact, and an error message should be shown.
- What happens when the bin folder is manually deleted by the user outside the app? The system should recreate it as needed — no crash or error on next delete.
- What happens when the same entry name exists in the bin from a different category? The bin should preserve category context (e.g., via subfolder or filename prefix) to avoid collisions.
- What happens when a user synced shipped entry is deleted? It should be treated as a shipped entry and protected from deletion.

## Clarifications

### Session 2026-04-14

- Q: How should the bin organize entries to track their origin category? → A: Mirror category subfolders inside the bin (e.g., `bin/primitives/sphere.3mf`, `bin/mechanical/gear.3mf`). This is human-browsable and naturally handles cross-category name collisions.
- Q: How does the user trigger deletion in the library browser? → A: Right-click context menu on an entry in the library browser. This follows standard desktop application conventions.
- Q: Should the existing `delete_library_entry` MCP tool change behavior, and are new MCP tools needed for bin operations? → A: Modify the existing `delete_library_entry` to perform soft-delete (move to bin). Add new MCP tools for bin browse, restore, and empty operations.
- Q: What does "keep both" mean concretely when restoring an entry that conflicts with an existing one? → A: Rename the restored entry with a numeric suffix (e.g., `sphere_1.3mf`) so both entries coexist.
- Q: Is a confirmation prompt needed before soft-delete, and what form does the post-delete notification take? → A: No confirmation before soft-delete (since it is recoverable). Show a brief inline notification after deletion indicating the entry was moved to the bin.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST allow users to delete user-created library entries via a right-click context menu in the library browser.
- **FR-002**: System MUST move deleted entries to a bin folder within the user's library directory instead of permanently removing them. No confirmation prompt is needed before soft-delete; a brief inline notification is shown after the entry is moved.
- **FR-003**: System MUST prevent deletion of shipped (default) library entries, displaying a clear explanation to the user.
- **FR-004**: System MUST preserve the original category of each binned entry by mirroring the category subfolder structure inside the bin (e.g., `bin/primitives/`, `bin/mechanical/`).
- **FR-005**: System MUST handle filename collisions in the bin by disambiguating filenames (never overwriting previously binned items).
- **FR-006**: System MUST create the bin folder automatically when the first entry is deleted, requiring no manual setup.
- **FR-007**: System MUST allow users to browse the contents of the bin to see previously deleted entries.
- **FR-008**: System MUST allow users to restore a binned entry back to its original library category.
- **FR-009**: System MUST handle restore conflicts (when an entry with the same name already exists in the target category) by renaming the restored entry with a numeric suffix (e.g., `sphere_1.3mf`) so both entries coexist.
- **FR-010**: System MUST allow users to permanently delete individual entries from the bin.
- **FR-011**: System MUST allow users to empty the entire bin at once.
- **FR-012**: System MUST show a confirmation prompt before any permanent (irreversible) deletion from the bin.
- **FR-013**: System MUST treat shipped entries that were synced to the user profile on first launch as shipped (protected) entries.
- **FR-014**: System MUST fail gracefully if the filesystem operation (move/delete) fails, keeping the original file intact and displaying an error message.

### Key Entities

- **Library Entry**: A .3mf file in the library representing a reusable function or primitive. Has a name, category, and metadata (description, tags, function signatures). Can be either shipped (read-only) or user-created (deletable).
- **Bin**: A special folder (e.g., `.bin/`) within the user's library directory that holds soft-deleted library entries. Internally mirrors the category subfolder structure to preserve origin context. Items in the bin are not visible in the normal library listing.
- **Category**: A grouping folder for library entries (e.g., "primitives", "csg", "mechanical"). Entries are organized by category in both the active library and the bin.

## Assumptions

- The bin is stored locally within the user's library directory (alongside the existing category folders). This folder is hidden from normal library browsing.
- The bin does not have a size limit or automatic expiry — users manage it manually via browse/restore/empty actions.
- When Gladius copies shipped entries to the user profile during first launch (`syncShippedLibrary`), those synced copies retain their "shipped" identity and remain protected from deletion.
- Deletion and restore actions are available both through the UI (library browser context menu) and through the MCP tool interface (for agent-driven workflows). The existing `delete_library_entry` MCP tool will be modified to perform soft-delete. New MCP tools will be added for bin browse, restore, and empty operations.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can delete a user-created library entry in under 3 seconds from the library browser.
- **SC-002**: 100% of deleted entries are recoverable from the bin until the user explicitly empties the bin or permanently deletes them.
- **SC-003**: Shipped library entries are never removed or moved — all deletion attempts on shipped entries are rejected.
- **SC-004**: Users can restore a binned entry to its original category in under 3 seconds.
- **SC-005**: No data loss occurs due to filename collisions in the bin — every binned entry is preserved with a unique name.
