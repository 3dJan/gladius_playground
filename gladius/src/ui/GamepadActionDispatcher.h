#pragma once

#include "GamepadActionMap.h"
#include "NodeFocusManager.h"
#include "CanvasPanController.h"

#include <string>

namespace gladius::ui
{

    class ModelEditor;

    /// @brief Routes gamepad actions to appropriate ModelEditor methods.
    /// @details Acts as the bridge between gamepad input and editor operations.
    ///         Each dispatch method maps a GamepadAction to the corresponding
    ///         ModelEditor operation.
    class GamepadActionDispatcher
    {
      public:
        GamepadActionDispatcher() = default;
        ~GamepadActionDispatcher() = default;

        /// Dispatch a single action to the editor.
        /// @param action The gamepad action to dispatch
        /// @param editor The model editor to route the action to
        void dispatch(GamepadAction action, ModelEditor & editor);

        /// Dispatch a sequence of actions (for combo-based operations).
        /// @param actions Vector of actions to dispatch in order
        /// @param editor The model editor to route actions to
        void dispatchSequence(std::vector<GamepadAction> const & actions, ModelEditor & editor);

        /// Update the focus and pan controllers based on gamepad input.
        /// @param gamepad Current gamepad state
        /// @param actionMap Current action mapping configuration
        void update(GamepadState & gamepad, GamepadActionMap const & actionMap, ModelEditor & editor);

      private:
        // Action handlers - each handles a category of actions
        void handleNavigation(GamepadAction action, ModelEditor & editor);
        void handleSelection(GamepadAction action, ModelEditor & editor);
        void handleEditorActions(GamepadAction action, ModelEditor & editor);
        void handleMenuActions(GamepadAction action, ModelEditor & editor);
        void handleNavigationHistory(GamepadAction action, ModelEditor & editor);

        // Helper methods
        void selectFocusedNode(ModelEditor & editor);
        void deselectAll(ModelEditor & editor);
        void openNodeMenu(ModelEditor & editor);
        void deleteSelectedNodes(ModelEditor & editor);
        void centerView(ModelEditor & editor);
        void createNode(ModelEditor & editor);

        // State
        NodeFocusManager m_focusManager;
        CanvasPanController m_canvasPanController;
    };

} // namespace gladius::ui
