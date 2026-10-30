#pragma once

#include "../nodes/Assembly.h"
#include "FileDialogService.h"
#include "LinkColors.h"
#include "NumericInputOverlay.h"
#include "PinHitboxGeometry.h"
#include "Style.h"

#include <imgui.h>
#include <map>
#include <optional>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace gladius::nodes
{
    class FunctionGradient;
    class NormalizeDistanceField;
}

namespace gladius::ui
{
    class ExportState;
    class ModelEditor;

    std::string typeToString(std::type_index typeIndex);
    using ColumnWidths = std::array<float, 8>;

    /**
     * @brief Structure that holds information about a group of nodes
     */
    struct NodeGroup
    {
        std::string tag;                  ///< The tag that all nodes in this group share
        std::vector<nodes::NodeId> nodes; ///< The nodes that belong to this group
        ImVec2 minBound;                  ///< The minimum bound of the group (top-left)
        ImVec2 maxBound;                  ///< The maximum bound of the group (bottom-right)
        ImVec4 color;                     ///< The color for this group
    };

    class NodeView : public nodes::Visitor
    {
      public:
        NodeView();
        ~NodeView() override = default;
        void visit(nodes::NodeBase & baseNode) override;
        void visit(nodes::Begin & beginNode) override;
        void visit(nodes::End & endNode) override;
        void visit(nodes::ConstantScalar & constantScalarNode) override;
        void visit(nodes::ConstantVector & constantVectorNode) override;
        void visit(nodes::ConstantMatrix & constantMatrixNode) override;
        void visit(nodes::Transformation & transformationNode) override;

        void visit(nodes::Resource & resourceNode) override;

        void setModelEditor(ModelEditor * editor);

        /// @brief Set the export state for blocking input during export
        /// @param state Pointer to ExportState (may be nullptr)
        void setExportState(ExportState * state);

        [[nodiscard]] auto haveParameterChanged() const -> bool;
        [[nodiscard]] auto hasModelChanged() const -> bool;
        [[nodiscard]] bool hasNumericInputEditChangedThisFrame() const;
        void clearPerFrameFlags();
        void drawNumericInputOverlay();

        void setAssembly(nodes::SharedAssembly assembly);
        void reset();

        void setCurrentModel(nodes::SharedModel model);

        void setResourceNodesVisible(bool visible);
        [[nodiscard]] bool areResourceNodesVisible() const;

        void setUiScale(float scale);
        [[nodiscard]] float getUiScale() const;

        bool columnWidthsAreInitialized() const;

        /// @brief Clear cached column widths (call after graph rebuild from code sync)
        void clearColumnWidths();

        /**
         * @brief Updates the node group mapping based on current model
         */
        void updateNodeGroups();

        /**
         * @brief Renders all node groups
         */
        void renderNodeGroups();

        /// @brief Clear the transient connection preview before processing the current popup.
        void clearConnectionPreview();

        /// @brief Render the focused candidate connection inside the active node-editor context.
        void renderConnectionPreview();

        /**
         * @brief Find a node by ID in the current model
         * @param nodeId The ID of the node to find
         * @return Pointer to the found node or nullptr if not found
         */
        nodes::NodeBase * findNodeById(nodes::NodeId nodeId) const;

        /**
         * @brief Replace the tag of all nodes in a group
         * @param oldTag The current tag to replace
         * @param newTag The new tag to assign to all nodes in the group
         * @return true if the tag was successfully replaced, false otherwise
         */
        bool replaceGroupTag(const std::string & oldTag, const std::string & newTag);

        /**
         * @brief Get access to the node groups map
         * @return const reference to the node groups map
         */
        const std::unordered_map<std::string, NodeGroup> & getNodeGroups() const;

        /**
         * @brief Checks if the provided tag is already assigned to a group
         * @param tag The tag to check
         * @return Whether the tag exists in the current groups
         */
        [[nodiscard]] bool hasGroup(const std::string & tag) const;

        /**
         * @brief Gets all nodes that belong to the same group as the specified node
         * @param nodeId The ID of the node to find group members for
         * @return Vector of node IDs in the same group, or empty vector if not in a group
         */
        std::vector<nodes::NodeId> getNodesInSameGroup(nodes::NodeId nodeId) const;

        /**
         * @brief Handles group dragging via header/border areas
         * Should be called before rendering groups to handle mouse input
         */
        void handleGroupDragging();

        /**
         * @brief Checks if selection rectangle should be suppressed due to group dragging
         * @return true if selection rectangle should be suppressed
         */
        bool shouldSuppressSelectionRect() const;

        /**
         * @brief Alternative: Handle group dragging with modifier key (Ctrl+drag anywhere in group)
         * @param requireModifier If true, requires Ctrl key to be held for group dragging
         */
        void handleGroupDraggingWithModifier(bool requireModifier = true);

        /**
         * @brief Alternative: Handle group dragging with right mouse button
         * Right-click and drag anywhere in group to move entire group
         */
        void handleGroupDraggingRightClick();

        /**
         * @brief Handles double-click on group rectangles to select all nodes in the group
         * @param groupTag The tag of the group that was double-clicked
         */
        void handleGroupClick(const std::string & groupTag);

        /**
         * @brief Checks for double-clicks on group rectangles and returns the group tag if found
         * @return The tag of the group that was double-clicked, or empty string if none
         */
        std::string checkForGroupClick() const;

        /**
         * @brief Checks if the mouse is over a group header or border area (for dragging)
         * @param mousePos The mouse position in screen coordinates
         * @return The tag of the group under the mouse header/border, or empty string if none
         */
        std::string getGroupUnderMouseHeader(const ImVec2 & mousePos) const;

        /**
         * @brief Checks if the mouse is over the interior (content area) of any group
         * @param mousePos The mouse position in screen coordinates
         * @return true if mouse is over group interior (should allow selection rectangle)
         */
        bool isMouseOverGroupInterior(const ImVec2 & mousePos) const;

      private:
        void show(nodes::NodeBase & node);
        void header(nodes::NodeBase & node);
        void content(nodes::NodeBase & node);
        void footer(nodes::NodeBase & node);
        void inputControls(nodes::NodeBase & node, nodes::ParameterMap::reference parameter);
        void showLinkAssignmentMenu(nodes::ParameterMap::reference parameter);
        void showOutputLinkAssignmentMenu(nodes::Port const & port);
        void setConnectionPreview(nodes::PortId sourcePortId,
                nodes::ParameterId targetParameterId);
        void showInputAndOutputs(nodes::NodeBase & node);
        void outputPins(nodes::NodeBase & node);
        void viewInputNode(nodes::NodeBase & node);

        ImVec4 typeToColor(std::type_index tyepIndex) const;

        /// Compute the visual color for a pin, applying drag-state
        /// highlighting/dimming when a link is being dragged.
        ImVec4 pinColorForDragState(nodes::PortId pinId,
                                    bool isInput,
                                    std::type_index typeIndex) const;

        /// Return the visual state of a pin during a link drag.
        PinVisualState pinVisualState(nodes::PortId pinId, bool isInput) const;

        /// Push ImGui button style colors for a pin's drag visual state.
        /// Returns the number of style colors pushed (call PopStyleColor with this count).
        int pushPinButtonStyle(PinVisualState state, ImVec4 baseColor) const;

        /// Show a tooltip with port name and type when hovering a compatible pin during drag.
        void showPinDragTooltip(nodes::PortId pinId,
              std::string const & name,
              std::type_index typeIndex,
              PinVisualState state) const;

        /// Compute pin hit, pivot, and menu-button geometry for the current table cell.
        PinHitboxGeometry pinHitboxGeometry(bool isInput) const;

        /// Draw the visible, type-colored port marker independently of its menu button.
        void drawPinMarker(PinHitboxGeometry const & geometry,
               ImVec4 color,
               PinVisualState state,
               bool isHovered) const;

        /// Register independent drag-target and visible-anchor rectangles with the node editor.
        void registerPinHitbox(PinHitboxGeometry const & geometry, bool isInput) const;

        /// Whether the node editor currently reports this pin as hovered.
        bool isPinHovered(nodes::PortId pinId) const;

        bool viewString(nodes::NodeBase const & node,
                        nodes::ParameterMap::reference parameter,
                        nodes::VariantType & val);

        bool editFloatWithNumericOverlay(nodes::NodeBase const & node,
                                         nodes::ParameterMap::reference parameter,
                                         char const * label,
                                         float * value,
                                         int componentIndex,
                                         bool angleInDegrees);
        void clearNumericInputTransaction();

        void viewFloat(nodes::NodeBase const & node,
                       nodes::ParameterMap::reference parameter,
                       nodes::VariantType & val);

        void viewFloat3(nodes::NodeBase const & node,
                        nodes::ParameterMap::reference parameter,
                        nodes::VariantType & val);

        /// Renders a ConstantVector node's X/Y/Z parameters as a grouped vector/color widget.
        void viewConstantVector(nodes::ConstantVector & node);

        void viewMatrix(nodes::NodeBase const & node,
                        nodes::ParameterMap::reference parameter,
                        nodes::VariantType & val);

        void viewResource(nodes::NodeBase & node,
                          nodes::ParameterMap::reference parameter,
                          nodes::VariantType & val);

        void viewInt(nodes::NodeBase const & node,
                     nodes::ParameterMap::reference parameter,
                     nodes::VariantType & val);

        void functionGradientControls(nodes::FunctionGradient & node);
        void functionCallControls(nodes::FunctionCall & node);
        void normalizeDistanceFieldControls(nodes::NormalizeDistanceField & node);

        /// Helper to create a FunctionGradient node from a FunctionCall node
        void createFunctionGradientFromCall(nodes::FunctionCall & node);
        /// Helper to create a NormalizeDistanceField node from a FunctionCall node
        void createNormalizeFromCall(nodes::FunctionCall & node);

        /**
         * @brief Log an error message to the document logger or stderr
         * @param message The error message to log
         */
        void logError(const std::string & message);

        bool typeControl(std::string const & label, std::type_index & typeIndex);

        /**
         * @brief Calculates the bounds of a node group
         * @param group The group to calculate bounds for
         */
        void calculateGroupBounds(NodeGroup & group);

        /**
         * @brief Calculate group rectangle with padding and header
         * @param group The group to calculate bounds for
         * @param minOut Output minimum bounds (top-left)
         * @param maxOut Output maximum bounds (bottom-right)
         * @return true if the group has valid nodes and bounds were calculated
         */
        bool calculateGroupRect(const NodeGroup & group, ImVec2 & minOut, ImVec2 & maxOut) const;

        /**
         * @brief Generate a consistent color for a group based on its tag
         * @param tag The group tag to generate color for
         * @return RGBA color for the group
         */
        ImVec4 generateGroupColor(const std::string & tag) const;

        struct ConnectionPreview
        {
            nodes::PortId sourcePortId{};
            nodes::ParameterId targetParameterId{};
        };

        std::optional<ConnectionPreview> m_connectionPreview;
        int m_currentLinkId = 0;
        bool m_parameterChanged{false};
        bool m_modelChanged{false};

        nodes::SharedAssembly m_assembly;
        nodes::SharedModel m_currentModel;

        struct ActiveNumericInputTarget
        {
            nodes::NodeId nodeId{};
            nodes::ParameterId parameterId{};
            int componentIndex{0};
            bool angleInDegrees{false};
        };

        ModelEditor * m_modelEditor{nullptr};
        ExportState * m_exportState{nullptr};
        std::optional<ActiveNumericInputTarget> m_numericInputTarget;
        NumericInputOverlay m_numericInputOverlay;
        std::optional<nodes::Assembly> m_numericInputInitialAssembly;
        float m_numericInputInitialValue{0.f};
        bool m_numericInputInitialParameterModifiable{true};
        bool m_numericInputEditChangedThisFrame{false};
        bool m_showContextMenu{false};

        bool m_showLinkAssignmentMenu{false};
        bool m_showOutputLinkAssignmentMenu{false};
        bool m_popStyle{false};

        bool m_resoureIdNodesVisible{false};

        struct NewChannelProperties
        {
            nodes::ParameterName name = {"new"};
            std::type_index typeIndex = typeid(float);
            bool expanded = false;
        };

        std::unordered_map<nodes::NodeId, NewChannelProperties>
          m_newChannelProperties; // used for persistence of the new parameter field in the begin
                                  // node

        std::unordered_map<nodes::NodeId, NewChannelProperties> m_newOutputChannelProperties;

        NodeTypeToColor m_nodeTypeToColor;

        float m_uiScale = 1.0f;

        std::unordered_map<nodes::NodeId, ColumnWidths> m_columnWidths;

        /// Storage for node groups organized by tag
        std::unordered_map<std::string, NodeGroup> m_nodeGroups;
        bool m_nodeGroupsDirty{true}; ///< Rebuild groups on next updateNodeGroups() call

        /// Tag editing state
        std::string m_editingTag;
        std::string m_editingTagBuffer;
        bool m_isEditingTag = false;

        std::string m_lowerGradientMessage;
        bool m_lowerGradientMessageIsError{false};

        // Status messaging for NormalizeDistanceField lowering
        std::string m_lowerNormalizeMessage;
        bool m_lowerNormalizeMessageIsError{false};

        /// Group node position tracking for group movement (stores positions of group nodes, not
        /// individual model nodes)
        std::unordered_map<nodes::NodeId, ImVec2> m_previousNodePositions;
        bool m_skipGroupMovement =
          false; // Flag to prevent infinite loops during programmatic movement

        /// Double-click group selection state
        std::string m_pendingGroupSelection;
        bool m_hasPendingGroupSelection = false;
        int m_groupSelectionFrame = 0; // Frame counter for deferred selection

        /// Group dragging state
        std::string m_draggingGroup;
        bool m_isDraggingGroup = false;
        ImVec2 m_groupDragStartPos;

        /// Async file dialog for filename parameters
        AsyncFileDialog m_asyncFileDialog;
        nodes::NodeId m_asyncFileDialogNodeId{0};  ///< Node whose filename is being edited
        std::string m_asyncFileDialogParamName;    ///< Parameter name being edited

        ColumnWidths & getOrCreateColumnWidths(nodes::NodeId nodeId);
    };
} // namespace gladius::ui
