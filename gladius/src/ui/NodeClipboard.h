#pragma once

#include "nodes/Model.h"

#include "imgui.h"

namespace gladius::ui
{

    /**
     * @brief Manages copy/paste operations for node editor selections
     *
     * Stores a cloned model containing the copied nodes and their internal links.
     * Handles positioning of pasted nodes relative to mouse cursor with
     * offset nudging for consecutive pastes.
     */
    class NodeClipboard
    {
      public:
        NodeClipboard() = default;
        ~NodeClipboard() = default;

        // Non-copyable, movable
        NodeClipboard(NodeClipboard const &) = delete;
        NodeClipboard & operator=(NodeClipboard const &) = delete;
        NodeClipboard(NodeClipboard &&) = default;
        NodeClipboard & operator=(NodeClipboard &&) = default;

        /**
         * @brief Check if the clipboard contains nodes
         * @return true if nodes have been copied
         */
        bool hasContent() const;

        /**
         * @brief Copy the specified nodes from a model to the clipboard
         * @param sourceModel The model containing the nodes to copy
         * @param selectedNodeIds Set of node IDs to copy
         */
        void copyNodes(nodes::Model & sourceModel,
                       std::set<nodes::NodeId> const & selectedNodeIds);

        /**
         * @brief Paste clipboard contents into the target model
         * @param targetModel The model to paste nodes into
         * @param canvasPosition The canvas position where nodes should be centered
         * @return Map from clipboard node unique names to newly created nodes
         */
        std::unordered_map<std::string, nodes::NodeBase *>
        pasteNodes(nodes::Model & targetModel, ImVec2 canvasPosition);

        /**
         * @brief Update paste position tracking for offset nudging
         * @param canvasPosition The canvas position used for pasting
         *
         * Called after successful paste. If the position is the same as the
         * previous paste, consecutive paste count increases and future pastes
         * will be offset.
         */
        void updatePastePosition(ImVec2 canvasPosition);

        /**
         * @brief Get adjusted canvas position with offset for consecutive pastes
         * @param mouseCanvasPos The raw canvas position from mouse
         * @return Adjusted position with offset applied if needed
         */
        ImVec2 getAdjustedPastePosition(ImVec2 mouseCanvasPos);

        /**
         * @brief Clear the clipboard contents
         */
        void clear();

      private:
        nodes::UniqueModel m_clipboardModel;

        // Paste UX helpers for offset nudging
        bool m_hadLastPastePos{false};
        ImVec2 m_lastPasteCanvasPos{0.f, 0.f};
        int m_consecutivePasteCount{0};
        float m_pasteOffsetStep{20.f};
    };

} // namespace gladius::ui
