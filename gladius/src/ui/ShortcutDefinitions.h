#pragma once

#include "ShortcutManager.h"

#include <string>
#include <vector>

namespace gladius::ui
{

    /**
     * @brief Static definition of a keyboard shortcut action
     *
     * Contains all metadata needed to register a shortcut action,
     * except for the callback which must be bound at runtime.
     */
    struct ShortcutDefinition
    {
        std::string id;
        std::string displayName;
        std::string description;
        ShortcutContext context;
        ImGuiKey key;
        bool ctrl = false;
        bool alt = false;
        bool shift = false;

        ShortcutCombo toCombo() const
        {
            return ShortcutCombo(key, ctrl, alt, shift);
        }
    };

    /**
     * @brief Get all shortcut definitions for the application
     *
     * Centralized list of all keyboard shortcuts. Callbacks are registered
     * separately in MainWindow::initializeShortcuts() using these definitions.
     */
    inline std::vector<ShortcutDefinition> getShortcutDefinitions()
    {
        // clang-format off
        return {
            // File operations
            {"file.new",       "New",      "Create a new model",              ShortcutContext::Global,      ImGuiKey_N, true},
            {"file.open",      "Open",     "Open an existing model",          ShortcutContext::Global,      ImGuiKey_O, true},
            {"file.save",      "Save",     "Save the current model",          ShortcutContext::Global,      ImGuiKey_S, true},
            {"file.saveAs",    "Save As",  "Save the current model with a new name", ShortcutContext::Global, ImGuiKey_S, true, false, true},

            // View operations
            {"view.resetView",        "Reset View",        "Reset the camera view",         ShortcutContext::RenderWindow, ImGuiKey_R},
            {"view.shortcuts",        "Keyboard Shortcuts","Show keyboard shortcuts dialog",ShortcutContext::Global,       ImGuiKey_K, true},
            {"view.uiScaleIncrease",  "Increase UI Scale", "Increase UI scaling (Ctrl +)",  ShortcutContext::Global,       ImGuiKey_Equal, true},
            {"view.uiScaleDecrease",  "Decrease UI Scale", "Decrease UI scaling (Ctrl -)",  ShortcutContext::Global,       ImGuiKey_Minus, true},
            {"view.uiScaleReset",     "Reset UI Scale",    "Reset UI scaling (Ctrl+Shift+0)",ShortcutContext::Global,      ImGuiKey_0, true, false, true},

            // Edit operations
            {"edit.library",   "Toggle Library Browser", "Show or hide the library browser", ShortcutContext::Global, ImGuiKey_B, true},
            {"edit.undo",      "Undo",     "Undo the last action",            ShortcutContext::ModelEditor, ImGuiKey_Z, true},
            {"edit.redo",      "Redo",     "Redo the last undone action",     ShortcutContext::ModelEditor, ImGuiKey_Y, true},
            {"edit.compile",   "Compile Model", "Compile the current model",  ShortcutContext::ModelEditor, ImGuiKey_F5},

            // Model editor operations
            {"model.compileImplicit", "Compile Implicit Function", "Manually compile the implicit function", ShortcutContext::ModelEditor, ImGuiKey_F7},
            {"model.copy",     "Copy Nodes",  "Copy selected nodes to clipboard",  ShortcutContext::ModelEditor, ImGuiKey_C, true},
            {"model.paste",    "Paste Nodes", "Paste nodes from clipboard",        ShortcutContext::ModelEditor, ImGuiKey_V, true},
            {"model.delete",   "Delete Nodes","Delete selected nodes",             ShortcutContext::ModelEditor, ImGuiKey_Delete},
            {"model.autoLayout","Auto Layout","Automatically layout nodes",        ShortcutContext::ModelEditor, ImGuiKey_L, true},
            {"model.newNode",  "New Node",    "Open node creation menu",           ShortcutContext::ModelEditor, ImGuiKey_Space},
            {"model.navigateBack", "Navigate Back", "Go to previous function",     ShortcutContext::ModelEditor, ImGuiKey_LeftBracket, true},
            {"model.navigateForward", "Navigate Forward", "Go to next function",   ShortcutContext::ModelEditor, ImGuiKey_RightBracket, true},
        };
        // clang-format on
    }

    /**
     * @brief Find a shortcut definition by ID
     * @param id The shortcut action ID
     * @return Pointer to the definition, or nullptr if not found
     */
    inline ShortcutDefinition const * findShortcutDefinition(std::string const & id)
    {
        static auto const definitions = getShortcutDefinitions();
        for (auto const & def : definitions)
        {
            if (def.id == id)
            {
                return &def;
            }
        }
        return nullptr;
    }

} // namespace gladius::ui

