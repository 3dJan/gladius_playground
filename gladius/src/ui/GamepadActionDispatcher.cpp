#include "GamepadActionDispatcher.h"

#include "ModelEditor.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <nodes/Model.h>

namespace gladius::ui
{

namespace
{

/// @brief Check if a popup/menu is currently open in ImGui.
/// @return true if a popup is open
bool isPopupOpen()
{
    // Check if any popup is open by checking for active popup ID
    // ImGui stores the current popup ID in the context when a popup is open
    ImGuiContext & g = *GImGui;
    return g.OpenPopupStack.size() > 0;
}

/// @brief Check if the focused node is valid and exists.
/// @param focusManager The node focus manager
/// @return true if a node is focused
bool hasValidFocus(NodeFocusManager const & focusManager)
{
    return focusManager.focusedNode() != 0;
}

} // anonymous namespace

void GamepadActionDispatcher::dispatch(GamepadAction action, ModelEditor & editor)
{
    // Route to appropriate handler based on action category
    switch (action)
    {
    // Navigation actions (D-pad, analog movement)
    case GamepadAction::NavigateUp:
    case GamepadAction::NavigateDown:
    case GamepadAction::NavigateLeft:
    case GamepadAction::NavigateRight:
        handleNavigation(action, editor);
        break;

    // Selection actions (X, Circle, etc.)
    case GamepadAction::Select:
    case GamepadAction::Deselect:
    case GamepadAction::ToggleSelect:
    case GamepadAction::Confirm:
        handleSelection(action, editor);
        break;

    // Editor actions (combos for undo, redo, compile, etc.)
    case GamepadAction::Undo:
    case GamepadAction::Redo:
    case GamepadAction::Compile:
    case GamepadAction::Copy:
    case GamepadAction::Paste:
    case GamepadAction::Delete:
    case GamepadAction::AutoLayout:
    case GamepadAction::CreateNode:
    case GamepadAction::ExtractFunction:
        handleEditorActions(action, editor);
        break;

    // Menu actions
    case GamepadAction::OpenMenu:
    case GamepadAction::CenterView:
        handleMenuActions(action, editor);
        break;

    // Navigation history
    case GamepadAction::NavigateBack:
    case GamepadAction::NavigateForward:
        handleNavigationHistory(action, editor);
        break;

    case GamepadAction::Count:
        // Invalid action, do nothing
        break;
    }
}

void GamepadActionDispatcher::dispatchSequence(std::vector<GamepadAction> const & actions,
                                               ModelEditor & editor)
{
    for (auto const & action : actions)
    {
        dispatch(action, editor);
    }
}

void GamepadActionDispatcher::update(GamepadState & gamepad,
                                     GamepadActionMap const & actionMap,
                                     ModelEditor & editor)
{
    // Refresh node spatial index each frame so directional navigation is accurate
    m_focusManager.updateNodePositions();

    // Update canvas panning based on left stick
    m_canvasPanController.update(gamepad, ImGui::GetIO().DeltaTime);
    m_canvasPanController.applyPan();

    // Update zoom based on right stick/triggers
    m_canvasPanController.updateZoom(gamepad);

    // Update node focus based on D-pad input
    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateLeft)))
    {
        m_focusManager.navigateFocus(NavigationDirection::Left);
    }
    else if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateRight)))
    {
        m_focusManager.navigateFocus(NavigationDirection::Right);
    }
    else if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateUp)))
    {
        m_focusManager.navigateFocus(NavigationDirection::Up);
    }
    else if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateDown)))
    {
        m_focusManager.navigateFocus(NavigationDirection::Down);
    }

    // Dispatch single button actions
    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Select)))
    {
        dispatch(GamepadAction::Select, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::ToggleSelect)))
    {
        dispatch(GamepadAction::ToggleSelect, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Confirm)))
    {
        dispatch(GamepadAction::Confirm, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Cancel)))
    {
        dispatch(GamepadAction::Cancel, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::OpenMenu)))
    {
        dispatch(GamepadAction::OpenMenu, editor);
    }

    // Check for combo-based actions (shoulder + face button)
    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Undo)))
    {
        dispatch(GamepadAction::Undo, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Redo)))
    {
        dispatch(GamepadAction::Redo, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Compile)))
    {
        dispatch(GamepadAction::Compile, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Copy)))
    {
        dispatch(GamepadAction::Copy, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Paste)))
    {
        dispatch(GamepadAction::Paste, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::Delete)))
    {
        dispatch(GamepadAction::Delete, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::AutoLayout)))
    {
        dispatch(GamepadAction::AutoLayout, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::CreateNode)))
    {
        dispatch(GamepadAction::CreateNode, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::ExtractFunction)))
    {
        dispatch(GamepadAction::ExtractFunction, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::CenterView)))
    {
        dispatch(GamepadAction::CenterView, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateBack)))
    {
        dispatch(GamepadAction::NavigateBack, editor);
    }

    if (gamepad.isButtonPressed(actionMap.getPrimaryButton(GamepadAction::NavigateForward)))
    {
        dispatch(GamepadAction::NavigateForward, editor);
    }

    // Start / Forward button always opens the Quick Reference overlay
    if (gamepad.isButtonPressed(GamepadButton::Forward))
    {
        editor.requestGamepadQuickReference();
    }
}

void GamepadActionDispatcher::handleNavigation(GamepadAction action, ModelEditor & editor)
{
    // Navigation is handled in update() via m_focusManager.navigateFocus()
    (void)action;
    (void)editor;
}

void GamepadActionDispatcher::handleSelection(GamepadAction action, ModelEditor & editor)
{
    switch (action)
    {
    case GamepadAction::Select:
        if (hasValidFocus(m_focusManager))
        {
            selectFocusedNode(editor);
        }
        break;

    case GamepadAction::Deselect:
        deselectAll(editor);
        break;

    case GamepadAction::ToggleSelect:
        if (hasValidFocus(m_focusManager))
        {
            nodes::NodeId focused = m_focusManager.focusedNode();
            // Check if already selected
            std::vector<ed::NodeId> selected = editor.getSelectedNodes();
            bool isSelected = false;
            for (auto const & s : selected)
            {
                if (s.Get() == static_cast<uint64_t>(focused))
                {
                    isSelected = true;
                    break;
                }
            }

            if (isSelected)
            {
                deselectAll(editor);
            }
            else
            {
                selectFocusedNode(editor);
            }
        }
        break;

    case GamepadAction::Confirm:
        // Confirm action - could open properties or confirm dialog
        if (hasValidFocus(m_focusManager))
        {
            openNodeMenu(editor);
        }
        break;

    default:
        break;
    }
}

void GamepadActionDispatcher::handleEditorActions(GamepadAction action, ModelEditor & editor) {
    switch (action) {
    case GamepadAction::Undo:
        editor.undo();
        showActionToast("Undo");
        break;

    case GamepadAction::Redo:
        editor.redo();
        showActionToast("Redo");
        break;

    case GamepadAction::Compile:
        editor.requestManualCompile();
        showActionToast("Compile");
        break;

    case GamepadAction::Copy:
        editor.copySelectionToClipboard();
        showActionToast("Copy");
        break;

    case GamepadAction::Paste:
        editor.pasteClipboardAtMouse();
        showActionToast("Paste");
        break;

    case GamepadAction::Delete:
        deleteSelectedNodes(editor);
        showActionToast("Delete");
        break;

    case GamepadAction::AutoLayout:
        editor.autoLayoutNodes();
        showActionToast("Auto Layout");
        break;

    case GamepadAction::CreateNode:
        createNode(editor);
        break;

    case GamepadAction::ExtractFunction:
        editor.showExpressionDialog();
        break;

    default:
        break;
    }
}

void GamepadActionDispatcher::handleMenuActions(GamepadAction action, ModelEditor & editor)
{
    switch (action)
    {
    case GamepadAction::OpenMenu:
        openNodeMenu(editor);
        break;

    case GamepadAction::CenterView:
        centerView(editor);
        break;

    default:
        break;
    }
}

void GamepadActionDispatcher::handleNavigationHistory(GamepadAction action, ModelEditor & editor)
{
    switch (action)
    {
    case GamepadAction::NavigateBack:
        if (editor.canGoBack())
        {
            editor.goBack();
        }
        break;

    case GamepadAction::NavigateForward:
        if (editor.canGoForward())
        {
            editor.goForward();
        }
        break;

    default:
        break;
    }
}

void GamepadActionDispatcher::selectFocusedNode(ModelEditor & editor)
{
    if (!hasValidFocus(m_focusManager))
    {
        return;
    }

    // TODO: Integrate with ModelEditor selection once the selection API is stabilised
    (void)editor;
}

void GamepadActionDispatcher::deselectAll(ModelEditor & editor)
{
    // TODO: Integrate with ModelEditor selection once the selection API is stabilised
    (void)editor;
}

void GamepadActionDispatcher::openNodeMenu(ModelEditor & editor)
{
    // This would open a context menu for the focused node
    // Implementation depends on how ModelEditor handles menus
}

void GamepadActionDispatcher::deleteSelectedNodes(ModelEditor & editor)
{
    // TODO: ModelEditor::onDeleteNode() is private; expose a public API when ready
    (void)editor;
}

void GamepadActionDispatcher::centerView(ModelEditor & editor)
{
    // TODO: ModelEditor::m_pendingCenterViewRequest is private; expose via public API
    (void)editor;
}

void GamepadActionDispatcher::createNode(ModelEditor & editor)
{
    editor.showCreateNodePopup();
}

void GamepadActionDispatcher::setVisualFeedback(GamepadVisualFeedback & vf)
{
    m_visualFeedback = &vf;
}

void GamepadActionDispatcher::showActionToast(std::string const & message)
{
    if (m_visualFeedback != nullptr)
    {
        m_visualFeedback->showToast(message, 1.5f);
    }
}

} // namespace gladius::ui
