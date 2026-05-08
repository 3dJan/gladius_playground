#pragma once

#include "GamepadState.h"
#include "GamepadActionMap.h"

#include "../nodes/nodesfwd.h"  // Required for ::gladius::nodes namespace alias
#include "imguinodeeditor.h"

#include <imgui.h>
#include <string>
#include <vector>

namespace gladius::ui
{
    // Forward declarations for node editor types
    namespace nodes = ::gladius::nodes;
    namespace ed = ax::NodeEditor;
    /**
     * @brief Direction for node navigation.
     */
    enum class NavigationDirection {
        Up,
        Down,
        Left,
        Right
    };

    /**
     * @brief Manages node focus and selection for gamepad navigation.
     *
     * This class tracks which node has keyboard/gamepad focus and provides
     * spatial-aware navigation (nearest node in a given direction).
     * It integrates with imguinodeeditor's selection system.
     *
     * Integration with node editor:
     * - On Select action → calls ed::SelectNode() and ed::NavigateToSelection()
     * - On Delete action → deletes focused/selected nodes
     * - On Copy action → copies focused/selected nodes
     */
    class NodeFocusManager {
      public:
        NodeFocusManager();
        ~NodeFocusManager() = default;

        // Non-copyable
        NodeFocusManager(const NodeFocusManager &) = delete;
        NodeFocusManager & operator=(const NodeFocusManager &) = delete;

        /**
         * @brief Update the manager's knowledge of node positions.
         *        Should be called when nodes are added, moved, or removed.
         */
        void updateNodePositions();

        // --- Focus management ---

        /**
         * @brief Set the currently focused node.
         * @param nodeId The node to focus (0 = no focus)
         */
        void setFocusedNode(nodes::NodeId nodeId);

        /**
         * @brief Get the currently focused node.
         * @return The focused node ID, or 0 if none
         */
        [[nodiscard]] nodes::NodeId focusedNode() const;

        /**
         * @brief Check if any node has focus.
         * @return true if a node is focused
         */
        [[nodiscard]] bool hasFocus() const;

        /**
         * @brief Clear focus.
         */
        void clearFocus();

        // --- Navigation ---

        /**
         * @brief Navigate focus in a given direction (spatial).
         *        Finds the nearest node in the specified direction from the current focus.
         * @param dir The navigation direction
         */
        void navigateFocus(NavigationDirection dir);

        /**
         * @brief Navigate focus using gamepad actions.
         * @param gamepad Current gamepad state
         * @param actionMap Current action map
         */
        void navigateFocus(GamepadState const & gamepad, GamepadActionMap const & actionMap);

        // --- Selection management ---

        /**
         * @brief Select a node (adds to selection).
         * @param nodeId The node to select
         * @param additive If true, adds to existing selection; otherwise clears first
         * @param editorContext The node editor context
         */
        void selectNode(nodes::NodeId nodeId, ed::EditorContext * editorContext, bool additive = false);

        /**
         * @brief Deselect a specific node.
         * @param nodeId The node to deselect
         * @param editorContext The node editor context
         */
        void deselectNode(nodes::NodeId nodeId, ed::EditorContext * editorContext);

        /**
         * @brief Clear all selections.
         * @param editorContext The node editor context
         */
        void clearSelection(ed::EditorContext * editorContext);

        /**
         * @brief Toggle selection of a node.
         * @param nodeId The node to toggle
         * @param editorContext The node editor context
         */
        void toggleNodeSelection(nodes::NodeId nodeId, ed::EditorContext * editorContext);

        /**
         * @brief Select all visible nodes.
         * @param editorContext The node editor context
         */
        void selectAll(ed::EditorContext * editorContext);

        /**
         * @brief Deselect all nodes.
         * @param editorContext The node editor context
         */
        void deselectAll(ed::EditorContext * editorContext);

        // --- Query ---

        /**
         * @brief Get all selected node IDs.
         * @return Vector of selected node IDs
         */
        [[nodiscard]] std::vector<nodes::NodeId> selectedNodes() const;

        /**
         * @brief Check if a node is selected.
         * @param nodeId The node to check
         * @return true if the node is selected
         */
        [[nodiscard]] bool isNodeSelected(nodes::NodeId nodeId) const;

        /**
         * @brief Get the number of selected nodes.
         * @return Number of selected nodes
         */
        [[nodiscard]] size_t selectionCount() const;

        /**
         * @brief Check if there are any selected nodes.
         * @return true if selection is non-empty
         */
        [[nodiscard]] bool hasSelection() const;

        /**
         * @brief Get the center position of a node (for spatial navigation).
         * @param nodeId The node ID
         * @return The node's center position, or (0,0) if not found
         */
        [[nodiscard]] ImVec2 getNodeCenter(nodes::NodeId nodeId) const;

      private:
        nodes::NodeId m_focusedNode{0};
        std::vector<nodes::NodeId> m_selection;

        // Spatial index for directional navigation
        struct NodePosition {
            nodes::NodeId id;
            ImVec2 center;
            ImVec2 size;
        };
        std::vector<NodePosition> m_nodePositions;

        /**
         * @brief Find the nearest node in a given direction.
         * @param from The source node ID (0 = center of screen)
         * @param dir The navigation direction
         * @return The nearest node ID, or 0 if none found
         */
        nodes::NodeId nearestNodeInDirection(nodes::NodeId from, NavigationDirection dir) const;

        /**
         * @brief Get node position from the editor context.
         * @param nodeId The node ID
         * @return The node's position, or (0,0) if not found
         */
        [[nodiscard]] ImVec2 getNodePosition(nodes::NodeId nodeId) const;

        /**
         * @brief Get node size from the editor context.
         * @param nodeId The node ID
         * @return The node's size, or (0,0) if not found
         */
        [[nodiscard]] ImVec2 getNodeSize(nodes::NodeId nodeId) const;
    };

    /**
     * @brief Utility function to convert NavigationDirection to string.
     */
    [[nodiscard]] std::string navigationDirectionToString(NavigationDirection dir);

} // namespace gladius::ui
