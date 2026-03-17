#pragma once

#include "../ExpressionToGraphConverter.h"
#include "../nodes/History.h"
#include "BeamLatticeView.h"
#include "CodeView.h"
#include "ExportState.h"
#include "ExpressionDialog.h"
#include "FunctionFromImage3DView.h"
#include "FunctionNavigationHistory.h"
#include "LibraryBrowser.h"
#include "LibraryDragPayload.h"
#include "LinkDragState.h"
#include "NodeClipboard.h"
#include "ValidationOverlay.h"
#include "NodeLayoutEngine.h"
#include "NodeView.h"
#include "imguinodeeditor.h"

#include <filesystem>
#include <string>
#include <typeindex>
#include <vector>

#include "Outline.h"
#include "ResourceView.h"
#include "Style.h"
#include "compute/ComputeCore.h"
#include "nodes/Assembly.h"
#include "nodes/FunctionExtractor.h"
#include "nodes/Model.h"
#include "nodes/nodesfwd.h"

namespace gladius
{
    class Document;
}

namespace ed = ax::NodeEditor;

namespace gladius::ui
{

    using PopupMenuFunction = std::function<void()>;

    class ModelEditor
    {
      public:
        ModelEditor();
        ~ModelEditor();
        void resetEditorContext();
        bool showAndEdit();

        void triggerNodePositionUpdate();

        void showPopupMenu(PopupMenuFunction popupMenuFunction);
        void closePopupMenu();

        [[nodiscard]] nodes::SharedModel currentModel() const;

        void setDocument(std::shared_ptr<Document> document);

        /**
         * @brief Get the current document
         * @return Shared pointer to the current document
         */
        [[nodiscard]] std::shared_ptr<Document> getDocument() const
        {
            return m_doc;
        }

        /// @brief Set the export state for blocking UI modifications during export
        void setExportState(ExportState * state);

        [[nodiscard]] bool modelWasModified() const;

        [[nodiscard]] bool isCompileRequested() const;

        /// Access the current link drag state for port compatibility rendering.
        [[nodiscard]] LinkDragState const & linkDragState() const { return m_linkDragState; }

        /// Non-const access so NodeView can record the drag source pin when the mouse is pressed.
        [[nodiscard]] LinkDragState & mutableLinkDragState() { return m_linkDragState; }

        void markModelAsModified();
        void markModelAsUpToDate();
        void setVisibility(bool visible);
        [[nodiscard]] bool isVisible() const;
        void createUndoRestorePoint(const std::string & description);
        void resetUndo();

        [[nodiscard]] bool primitveDataNeedsUpdate() const;
        void invalidatePrimitiveData();
        void markPrimitiveDataAsUpToDate();

        // Library browser methods
        void setLibraryRootDirectory(const std::filesystem::path & directory);
        void toggleLibraryVisibility();
        void setLibraryVisibility(bool visible);
        [[nodiscard]] bool isLibraryVisible() const;
        void refreshLibraryDirectories();
        void renderLibraryBrowser();

        /// Focus management for keyboard-driven workflow
        void requestNodeFocus(nodes::NodeId nodeId);
        /// Focus node and switch to its function if different from current
        void requestNodeFocus(nodes::NodeId nodeId, nodes::ResourceId modelId);
        [[nodiscard]] bool shouldFocusNode(nodes::NodeId nodeId) const;
        void clearNodeFocus();

        // Public methods for keyboard shortcuts
        void requestManualCompile();
        void autoLayoutNodes(float distance = 200.0f);
        void showCreateNodePopup();
        void showExpressionDialog();

        /**
         * @brief Handle creation of a function from mathematical expression
         * @param functionName The name for the new function
         * @param expression The mathematical expression
         */
        void onCreateFunctionFromExpression(std::string const & functionName,
                                            std::string const & expression,
                                            std::vector<FunctionArgument> const & arguments,
                                            FunctionOutput const & output);

        /**
         * @brief Switch to a specific function by its ResourceId
         * @param functionId The ResourceId of the function to switch to
         * @return true if the function was found and switched to, false otherwise
         */
        bool switchToFunction(nodes::ResourceId functionId);

        /**
         * @brief Navigate to a function and record the navigation in history.
         *        Use this instead of switchToFunction() for user-triggered navigation.
         * @param functionId The ResourceId of the function to navigate to
         * @param sourceNodeId Optional: the node that triggered navigation (for view restoration)
         */
        bool navigateToFunction(nodes::ResourceId functionId, nodes::NodeId sourceNodeId = 0);

        // Navigation history controls
        bool canGoBack() const;
        bool canGoForward() const;
        bool goBack();
        bool goForward();

        /**
         * @brief Check if mouse is hovering over the model editor
         * @return true if the model editor is being hovered
         */
        bool isHovered() const;

        /**
         * @brief Check if the current model is a FunctionFromImage3D
         * @return true if the model contains an ImageSampler node
         */
        bool isFunctionFromImage3D() const;

        /**
         * @brief Refresh the assembly from the current document.
         *        Call this after external modifications to the assembly (e.g., creating functions
         *        via ResourceView).
         */
        void refreshAssembly();

        /// Tab mode for FunctionFromImage3D functions
        enum class TabMode
        {
            Graph = 0,      ///< Normal graph view
            Properties = 1, ///< FunctionFromImage3D properties panel
            Code = 2        ///< Code view (GLSL-like snippet editor)
        };

      private:
        // Extraction helper
        void extractSelectedNodesToFunction(const std::string & functionName);

        // Copy/Paste helpers
        void copySelectionToClipboard();
        void pasteClipboardAtMouse();
        bool hasClipboard() const;

        void readBackNodePositions();
        void autoLayout();
        void applyNodePositions();
        bool updateInitialAutoLayoutReadiness();
        void placeTransformation(nodes::NodeBase & createdNode,
                                 std::vector<ed::NodeId> & selection) const;
        void placeBoolOp(nodes::NodeBase & createdNode, std::vector<ed::NodeId> & selection) const;
        void defaultPlacement(nodes::NodeBase & createdNode,
                              std::vector<ed::NodeId> & selection) const;
        void placeNode(nodes::NodeBase & node);
        void onCreateNode();
        void onDeleteNode();
        void toolBox();
        bool isNodeSelected(nodes::NodeId nodeId);
        void switchModel();
        void outline();
        void resourceOutline();
        void functionOutline();
        void newModelDialog();
        void onQueryNewNode();
        void createNodePopup(nodes::PortId srcPortId, ImVec2 mousePos);

        void invalidateEverything();
        void setAssembly(nodes::SharedAssembly assembly);

        void functionToolBox(ImVec2 mousePos);

        // New function creation methods
        nodes::Model & createLevelsetFunction(std::string const & name);
        nodes::Model & copyExistingFunction(nodes::Model const & sourceModel,
                                            std::string const & name);
        void meshResourceToolBox(ImVec2 mousePos);
        void beamLatticeResourceToolBox(ImVec2 mousePos);
        void showDeleteUnusedResourcesDialog();
        void validate();

        /// @brief Handle drag-and-drop from the library browser onto the node editor canvas.
        void handleLibraryDrop();

        /// @brief Create a FunctionCall node at the current cursor position.
        /// @param functionId The resource ID of the function to call.
        /// @param sourceModel The model providing inputs/outputs for the FunctionCall.
        void createFunctionCallNodeAtCursor(nodes::ResourceId functionId,
                                            nodes::SharedModel const & sourceModel);

        void undo();
        void redo();

        // Helper method to check if a string matches the current filter
        bool matchesNodeFilter(const std::string & text) const;

        void pushNodeColor(nodes::NodeBase & node);
        void popNodeColor(nodes::NodeBase & node);

        /// Returns the editor context for the given function, creating one if needed.
        ed::EditorContext * getOrCreateEditorContext(nodes::ResourceId functionId);

        /// Returns the editor context for the current model, or nullptr if no model is set.
        ed::EditorContext * getCurrentEditorContext();

        bool m_visible = false;
        std::unordered_map<nodes::ResourceId, ed::EditorContext *> m_editorContexts;
        std::set<nodes::ResourceId> m_visitedFunctions;  ///< Track first-time visits for NavigateToContent
        int m_pendingCenterViewFrames = 0;  ///< Frame countdown before requesting center view
        bool m_pendingCenterViewRequest = false; ///< Execute via same path as toolbar Center View
        bool m_dirty{true};
        bool m_parameterDirty{false};
        bool m_primitiveDataDirty{false};
        bool m_nodePositionsNeedUpdate{false};
        bool m_pendingPasteRequest{false};
        float m_nodeDistance = 50.f;
        float m_scale = 0.5f;
        std::string m_newModelName{"New_Part"};
        bool m_showAddModel{false};

        // New function dialog options
        enum class FunctionType
        {
            Empty = 0,
            CopyExisting = 1,
            LevelsetTemplate = 2,
            WrapExisting = 3
        };

        FunctionType m_selectedFunctionType{FunctionType::Empty};
        int m_selectedSourceFunctionIndex{0};

        enum class LayoutStrategyChoice
        {
            Auto = 0,
            OptimizedLayeredMedian,
            BalancedGridCompact,
            MedianSweepTightY,
            LayeredStackClassic,
            LayeredRowSweep,
            ForceRefinedHybrid
        };

        struct LayoutStrategyDescriptor
        {
            LayoutStrategyChoice choice;
            const char * displayName;
        };

        [[nodiscard]] static std::vector<LayoutStrategyDescriptor> layoutStrategyDescriptors();
        [[nodiscard]] static NodeLayoutEngine::LayoutStrategy
        makeLayoutStrategy(LayoutStrategyChoice choice);
        [[nodiscard]] static const char * layoutStrategyLabel(LayoutStrategyChoice choice);

        LayoutStrategyChoice m_selectedLayoutStrategy{LayoutStrategyChoice::Auto};

        nodes::SharedAssembly m_assembly;
        nodes::SharedModel m_currentModel;

        static void noOp() {};
        PopupMenuFunction m_popupMenuFunction = noOp;
        NodeView m_nodeViewVisitor;
        LinkDragState m_linkDragState;

        bool m_modelWasModified{false};
        bool m_outlineRenaming{true};
        bool m_showCreateNodePopUp{false};
        bool m_showExtractDialog{false};
        bool m_extractDialogInitialized{false};  // Track extract dialog initialization state
        std::string m_extractFunctionName{"ExtractedFunction"};

        // Extraction name editing state
        struct ExtractNameEntry
        {
            std::string key;                      // stable key (unique port name)
            std::string name;                     // editable name
            std::type_index type = typeid(float); // for potential future display
        };
        std::vector<ExtractNameEntry> m_extractInputNames;  // proposed + edited
        std::vector<ExtractNameEntry> m_extractOutputNames; // proposed + edited

        nodes::History m_history;

        bool m_stateApplyingUndo = false;

        bool m_autoCompile = true;
        bool m_isManualCompileRequested = false;
        bool m_outlineNodeColorLines = true;

        std::shared_ptr<Document> m_doc;

        ResourceView m_resourceView;
        BeamLatticeView m_beamLatticeView;

        Outline m_outline;
        ValidationOverlay m_validationOverlay;

        NodeTypeToColor m_nodeTypeToColor;
        float m_uiScale = 1.0f;

        // Confirmation dialog for removing unused resources
        bool m_showDeleteUnusedResourcesConfirmation = false;
        std::vector<Lib3MF::PResource> m_unusedResources;

        // Node filtering
        std::string m_nodeFilterText;

        // Library browser
        LibraryBrowser m_libraryBrowser;

        // Expression dialog
        ExpressionDialog m_expressionDialog;

        /// Focus management for keyboard-driven workflow
        nodes::NodeId m_nodeToFocus{0};
        bool m_shouldFocusNode{false};

        /// Group assignment dialog state
        bool m_showGroupAssignmentDialog{false};

        // Clipboard for copy/paste of nodes
        NodeClipboard m_clipboard;

        // Navigation history for back/forward through functions
        FunctionNavigationHistory m_navHistory;

        // Defer selection clearing to when an editor context is active
        bool m_pendingClearSelection{false};

        // One-time initial auto layout helper state.
        // The first auto layout for a function is executed only after node
        // sizes have been measured and remained stable across consecutive frames.
        // A max-wait frame limit ensures the layout always runs, even if
        // measured sizes never fully converge.
        bool m_pendingInitialAutoLayout{false};
        int m_initialAutoLayoutStableFrames{0};
        int m_initialAutoLayoutWaitFrames{0};
        std::unordered_map<nodes::NodeId, ImVec2> m_initialAutoLayoutSizeSnapshot;

        // Export state for blocking UI modifications during export
        ExportState * m_exportState{nullptr};

        // FunctionFromImage3D UI
        FunctionFromImage3DView m_functionFromImage3DView;
        TabMode m_currentTabMode{TabMode::Graph};
        bool m_forceCodeTab{false}; ///< Force-select Code tab on next frame ("Stay in Code")

        // Code view for snippet editing
        CodeView m_codeView;
    };

    std::vector<ed::NodeId> selectedNodes(ed::EditorContext * editorContext);
} // namespace gladius::ui
