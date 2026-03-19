#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace gladius::ui
{
    /// A menu bar helper that auto-collapses items into a "..." overflow menu
    /// when the available horizontal space is insufficient.
    ///
    /// Usage:
    ///   OverflowMenuBar overflow;
    ///   overflow.begin("myBarId");
    ///   overflow.item("Label1", [&] { if (ImGui::MenuItem("Label1")) { ... } });
    ///   overflow.item("Label2", [&] { if (ImGui::MenuItem("Label2")) { ... } });
    ///   overflow.end();
    ///
    /// Items whose measured width (from the previous frame) would exceed the
    /// available bar width are rendered inside a "..." dropdown instead.
    class OverflowMenuBar
    {
    public:
        /// Begin a new overflow menu bar pass. Call after ImGui::BeginMenuBar().
        /// @param id  A unique string id used to persist width measurements across frames.
        void begin(char const * id);

        /// Register a menu bar item.
        /// @param label   Display label (used for width estimation on the first frame).
        /// @param render  Callback that renders the item. Called exactly once per frame,
        ///                either inline in the bar or inside the overflow dropdown.
        void item(char const * label, std::function<void()> render);

        /// Finish the overflow menu bar. Renders the "..." overflow dropdown if needed.
        /// Call before ImGui::EndMenuBar().
        void end();

    private:
        struct ItemEntry
        {
            std::string label;
            std::function<void()> render;
        };

        /// Per-bar persistent state (keyed by bar id) surviving across frames.
        struct BarState
        {
            std::vector<float> itemWidths; ///< Measured widths from last frame
        };

        std::vector<ItemEntry> m_items;
        ImGuiID m_barId = 0;

        static std::unordered_map<ImGuiID, BarState> s_barStates;
    };

} // namespace gladius::ui
