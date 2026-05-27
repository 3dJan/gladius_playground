/**
 * @file Gamepad_tests.cpp
 * @brief Unit tests for gamepad support classes.
 *
 * These tests cover the pure-logic portions of the gamepad subsystem that
 * do not require a live ImGui or node-editor context:
 *   - GamepadState button held-state management and configuration
 *   - GamepadActionMap default bindings, remapping, action names
 *   - buttonToString / stringToButton round-trips
 *   - CanvasPanController state, zoom display and clamping
 *   - NodeFocusManager focus state management
 */

#include "ui/CanvasPanController.h"
#include "ui/GamepadActionMap.h"
#include "ui/GamepadState.h"
#include "ui/NodeFocusManager.h"

#include <gtest/gtest.h>

namespace gladius::ui::tests
{

// ============================================================================
// Helpers
// ============================================================================

/// RAII guard that resets the GamepadState singleton on construction and
/// destruction so tests do not bleed state into each other.
struct GamepadStateGuard
{
    GamepadStateGuard() { GamepadState::instance().reset(); }
    ~GamepadStateGuard() { GamepadState::instance().reset(); }
};

/// RAII guard that resets the GamepadActionMap singleton on destruction.
struct ActionMapGuard
{
    ~ActionMapGuard() { GamepadActionMap::instance().resetToDefaults(); }
};

// ============================================================================
// buttonToString / stringToButton
// ============================================================================

class ButtonStringConversionTest : public ::testing::Test
{
};

TEST_F(ButtonStringConversionTest, ButtonToString_AllDefinedButtons_ReturnNonEmptyString)
{
    // All concrete buttons (not Count) must yield a non-empty string.
    for (int i = 0; i < static_cast<int>(GamepadButton::Count); ++i)
    {
        auto btn = static_cast<GamepadButton>(i);
        EXPECT_FALSE(buttonToString(btn).empty())
            << "buttonToString returned empty string for button index " << i;
    }
}

TEST_F(ButtonStringConversionTest, StringToButton_ValidName_ReturnsMatchingButton)
{
    EXPECT_EQ(stringToButton("A"),          GamepadButton::A);
    EXPECT_EQ(stringToButton("B"),          GamepadButton::B);
    EXPECT_EQ(stringToButton("X"),          GamepadButton::X);
    EXPECT_EQ(stringToButton("Y"),          GamepadButton::Y);
    EXPECT_EQ(stringToButton("LB"),         GamepadButton::LB);
    EXPECT_EQ(stringToButton("RB"),         GamepadButton::RB);
    EXPECT_EQ(stringToButton("LStick"),     GamepadButton::LStick);
    EXPECT_EQ(stringToButton("RStick"),     GamepadButton::RStick);
    EXPECT_EQ(stringToButton("DPad Up"),    GamepadButton::DPadUp);
    EXPECT_EQ(stringToButton("DPad Down"),  GamepadButton::DPadDown);
    EXPECT_EQ(stringToButton("DPad Left"),  GamepadButton::DPadLeft);
    EXPECT_EQ(stringToButton("DPad Right"), GamepadButton::DPadRight);
    EXPECT_EQ(stringToButton("LT"),         GamepadButton::LT);
    EXPECT_EQ(stringToButton("RT"),         GamepadButton::RT);
}

TEST_F(ButtonStringConversionTest, RoundTrip_AllButtons_PreservesValue)
{
    // Every button must survive a buttonToString → stringToButton round-trip.
    for (int i = 0; i < static_cast<int>(GamepadButton::Count); ++i)
    {
        auto btn = static_cast<GamepadButton>(i);
        std::string const str = buttonToString(btn);
        EXPECT_EQ(stringToButton(str), btn)
            << "Round-trip failed for button index " << i << " (string: \"" << str << "\")";
    }
}

TEST_F(ButtonStringConversionTest, StringToButton_UnknownString_ReturnsCount)
{
    EXPECT_EQ(stringToButton(""), GamepadButton::Count);
    EXPECT_EQ(stringToButton("INVALID_BUTTON"), GamepadButton::Count);
}

// ============================================================================
// GamepadState – held-button state management
// ============================================================================

class GamepadStateTest : public ::testing::Test
{
  protected:
    GamepadStateGuard m_guard;
    GamepadState & m_state = GamepadState::instance();
};

TEST_F(GamepadStateTest, IsButtonHeld_AfterSetButtonHeldTrue_ReturnsTrue)
{
    // Arrange
    m_state.setButtonHeld(GamepadButton::A, true);

    // Act & Assert
    EXPECT_TRUE(m_state.isButtonHeld(GamepadButton::A));
}

TEST_F(GamepadStateTest, IsButtonHeld_AfterSetButtonHeldFalse_ReturnsFalse)
{
    // Arrange
    m_state.setButtonHeld(GamepadButton::A, true);
    m_state.setButtonHeld(GamepadButton::A, false);

    // Act & Assert
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::A));
}

TEST_F(GamepadStateTest, IsButtonHeld_DefaultState_ReturnsFalse)
{
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::LB));
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::DPadUp));
}

TEST_F(GamepadStateTest, IsButtonHeld_MultipleButtonsIndependent_DoNotAffectEachOther)
{
    // Arrange
    m_state.setButtonHeld(GamepadButton::A, true);
    m_state.setButtonHeld(GamepadButton::B, false);

    // Assert
    EXPECT_TRUE(m_state.isButtonHeld(GamepadButton::A));
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::B));
}

TEST_F(GamepadStateTest, Reset_AfterSettingHeldButtons_ClearsAllState)
{
    // Arrange
    m_state.setButtonHeld(GamepadButton::A, true);
    m_state.setButtonHeld(GamepadButton::LB, true);

    // Act
    m_state.reset();

    // Assert
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::A));
    EXPECT_FALSE(m_state.isButtonHeld(GamepadButton::LB));
}

TEST_F(GamepadStateTest, SetStickDeadzone_SetsConfigValue)
{
    m_state.setStickDeadzone(0.4f);
    EXPECT_FLOAT_EQ(m_state.stickDeadzone(), 0.4f);
}

TEST_F(GamepadStateTest, SetTriggerDeadzone_SetsConfigValue)
{
    m_state.setTriggerDeadzone(0.2f);
    EXPECT_FLOAT_EQ(m_state.triggerDeadzone(), 0.2f);
}

// ============================================================================
// GamepadActionMap – default bindings and remapping
// ============================================================================

class GamepadActionMapTest : public ::testing::Test
{
  protected:
    ActionMapGuard m_guard;
    GamepadActionMap & m_map = GamepadActionMap::instance();
};

TEST_F(GamepadActionMapTest, GetPrimaryButton_NavigateActions_MappedToDPad)
{
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::NavigateUp),    GamepadButton::DPadUp);
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::NavigateDown),  GamepadButton::DPadDown);
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::NavigateLeft),  GamepadButton::DPadLeft);
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::NavigateRight), GamepadButton::DPadRight);
}

TEST_F(GamepadActionMapTest, GetPrimaryButton_SelectAction_MappedToA)
{
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::Select), GamepadButton::A);
}

TEST_F(GamepadActionMapTest, GetPrimaryButton_OpenMenuAction_MappedToY)
{
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::OpenMenu), GamepadButton::Y);
}

TEST_F(GamepadActionMapTest, RemapAction_AfterRemap_GetPrimaryButtonReturnsNewButton)
{
    // Arrange
    m_map.remapAction(GamepadAction::Select, GamepadButton::X);

    // Assert
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::Select), GamepadButton::X);
}

TEST_F(GamepadActionMapTest, ResetToDefaults_AfterRemap_RestoresOriginalBinding)
{
    // Arrange
    m_map.remapAction(GamepadAction::Select, GamepadButton::X);

    // Act
    m_map.resetToDefaults();

    // Assert
    EXPECT_EQ(m_map.getPrimaryButton(GamepadAction::Select), GamepadButton::A);
}

TEST_F(GamepadActionMapTest, GetActionName_AllDefinedActions_ReturnNonEmptyString)
{
    for (int i = 0; i < static_cast<int>(GamepadAction::Count); ++i)
    {
        auto action = static_cast<GamepadAction>(i);
        EXPECT_FALSE(m_map.getActionName(action).empty())
            << "getActionName returned empty string for action index " << i;
    }
}

TEST_F(GamepadActionMapTest, GetAllActions_ReturnsNonEmptyList)
{
    auto const actions = m_map.getAllActions();
    EXPECT_FALSE(actions.empty());
}

TEST_F(GamepadActionMapTest, IsComboPressed_ShoulderNotHeld_ReturnsFalse)
{
    // Arrange
    GamepadStateGuard stateGuard;
    GamepadState & state = GamepadState::instance();
    state.setButtonHeld(GamepadButton::LB, false);

    // Act & Assert – combo can never fire when shoulder is not held
    EXPECT_FALSE(GamepadActionMap::isComboPressed(state, GamepadButton::LB, GamepadButton::A));
}

TEST_F(GamepadActionMapTest, IsActionHeld_NavigateUpAfterSetHeld_ReturnsTrue)
{
    // Arrange
    GamepadStateGuard stateGuard;
    GamepadState & state = GamepadState::instance();
    state.setButtonHeld(GamepadButton::DPadUp, true);

    // Act & Assert
    EXPECT_TRUE(m_map.isActionHeld(state, GamepadAction::NavigateUp));
}

TEST_F(GamepadActionMapTest, IsActionHeld_NavigateUpNotHeld_ReturnsFalse)
{
    GamepadStateGuard stateGuard;
    GamepadState & state = GamepadState::instance();
    state.setButtonHeld(GamepadButton::DPadUp, false);

    EXPECT_FALSE(m_map.isActionHeld(state, GamepadAction::NavigateUp));
}

// ============================================================================
// CanvasPanController – state management
// ============================================================================

class CanvasPanControllerTest : public ::testing::Test
{
  protected:
    CanvasPanController m_controller;
};

TEST_F(CanvasPanControllerTest, DefaultState_PanOffsetIsZero)
{
    EXPECT_FLOAT_EQ(m_controller.panOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(m_controller.panOffset().y, 0.0f);
}

TEST_F(CanvasPanControllerTest, DefaultState_ZoomLevelIsOne)
{
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 1.0f);
}

TEST_F(CanvasPanControllerTest, DefaultState_IsNotPanning)
{
    EXPECT_FALSE(m_controller.isPanning());
}

TEST_F(CanvasPanControllerTest, DefaultState_IsNotZooming)
{
    EXPECT_FALSE(m_controller.isZooming());
}

TEST_F(CanvasPanControllerTest, ZoomDisplay_DefaultZoom_Returns100Percent)
{
    EXPECT_EQ(m_controller.zoomDisplay(), "100%");
}

TEST_F(CanvasPanControllerTest, ZoomDisplay_AfterSetZoomLevel_ReturnsCorrectString)
{
    m_controller.setZoomLevel(1.5f);
    EXPECT_EQ(m_controller.zoomDisplay(), "150%");
}

TEST_F(CanvasPanControllerTest, SetZoomLevel_AboveMax_ClampsToMax)
{
    m_controller.setZoomLevel(999.0f);
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 5.0f);
}

TEST_F(CanvasPanControllerTest, SetZoomLevel_BelowMin_ClampsToMin)
{
    m_controller.setZoomLevel(-1.0f);
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 0.1f);
}

TEST_F(CanvasPanControllerTest, SetZoomLevel_WithinRange_SetsExactValue)
{
    m_controller.setZoomLevel(2.5f);
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 2.5f);
}

TEST_F(CanvasPanControllerTest, SetPanOffset_RoundTrip_RetainsValue)
{
    ImVec2 const offset{123.0f, -456.0f};
    m_controller.setPanOffset(offset);
    EXPECT_FLOAT_EQ(m_controller.panOffset().x, 123.0f);
    EXPECT_FLOAT_EQ(m_controller.panOffset().y, -456.0f);
}

TEST_F(CanvasPanControllerTest, Reset_AfterModifications_RestoresDefaults)
{
    // Arrange
    m_controller.setPanOffset(ImVec2{100.0f, 200.0f});
    m_controller.setZoomLevel(3.0f);

    // Act
    m_controller.reset();

    // Assert
    EXPECT_FLOAT_EQ(m_controller.panOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(m_controller.panOffset().y, 0.0f);
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 1.0f);
    EXPECT_FALSE(m_controller.isPanning());
    EXPECT_FALSE(m_controller.isZooming());
}

TEST_F(CanvasPanControllerTest, Update_WithZeroStickInput_DoesNotStartPanning)
{
    // Arrange – GamepadState defaults to zero analog values (no ImGui context)
    GamepadStateGuard guard;
    GamepadState & state = GamepadState::instance();

    // Act
    m_controller.update(state, 1.0f / 60.0f);

    // Assert
    EXPECT_FALSE(m_controller.isPanning());
    EXPECT_FLOAT_EQ(m_controller.panOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(m_controller.panOffset().y, 0.0f);
}

TEST_F(CanvasPanControllerTest, UpdateZoom_WithNoTriggerInput_DoesNotStartZooming)
{
    // Arrange – GamepadState defaults to zero trigger values
    GamepadStateGuard guard;
    GamepadState & state = GamepadState::instance();

    // Act
    m_controller.updateZoom(state);

    // Assert
    EXPECT_FALSE(m_controller.isZooming());
    EXPECT_FLOAT_EQ(m_controller.zoomLevel(), 1.0f);
}

// ============================================================================
// NodeFocusManager – focus state management
// ============================================================================

class NodeFocusManagerTest : public ::testing::Test
{
  protected:
    NodeFocusManager m_manager;
};

TEST_F(NodeFocusManagerTest, DefaultState_FocusedNodeIsZero)
{
    EXPECT_EQ(m_manager.focusedNode(), static_cast<nodes::NodeId>(0));
}

TEST_F(NodeFocusManagerTest, DefaultState_HasNoFocus)
{
    EXPECT_FALSE(m_manager.hasFocus());
}

TEST_F(NodeFocusManagerTest, DefaultState_HasNoSelection)
{
    EXPECT_FALSE(m_manager.hasSelection());
    EXPECT_EQ(m_manager.selectionCount(), 0u);
}

TEST_F(NodeFocusManagerTest, SetFocusedNode_WithValidId_UpdatesFocusedNode)
{
    // Act – setFocusedNode internally checks for editor context and skips ImGui calls
    // when ed::GetCurrentEditor() is nullptr (no active editor in test environment).
    m_manager.setFocusedNode(42);

    // Assert
    EXPECT_EQ(m_manager.focusedNode(), 42);
    EXPECT_TRUE(m_manager.hasFocus());
}

TEST_F(NodeFocusManagerTest, ClearFocus_AfterSettingFocus_RemovesFocus)
{
    // Arrange
    m_manager.setFocusedNode(42);

    // Act
    m_manager.clearFocus();

    // Assert
    EXPECT_EQ(m_manager.focusedNode(), static_cast<nodes::NodeId>(0));
    EXPECT_FALSE(m_manager.hasFocus());
}

TEST_F(NodeFocusManagerTest, NavigateFocus_WithNoNodesLoaded_DoesNotChangeFocus)
{
    // Arrange – no nodes loaded (updateNodePositions not called)
    m_manager.setFocusedNode(5);

    // Act
    m_manager.navigateFocus(NavigationDirection::Right);

    // Assert – focus must remain unchanged when no neighbour is found
    EXPECT_EQ(m_manager.focusedNode(), 5);
}

TEST_F(NodeFocusManagerTest, NavigateFocus_WithNoFocusAndNoNodes_RemainsUnfocused)
{
    // Act
    m_manager.navigateFocus(NavigationDirection::Up);

    // Assert
    EXPECT_FALSE(m_manager.hasFocus());
}

TEST_F(NodeFocusManagerTest, IsNodeSelected_WithNullEditorContext_ReturnsFalse)
{
    // selectNode with nullptr editorContext is a no-op
    m_manager.selectNode(10, nullptr, false);

    EXPECT_FALSE(m_manager.isNodeSelected(10));
    EXPECT_EQ(m_manager.selectionCount(), 0u);
}

TEST_F(NodeFocusManagerTest, GetNodeCenter_UnknownNode_ReturnsZeroVector)
{
    ImVec2 center = m_manager.getNodeCenter(99);
    EXPECT_FLOAT_EQ(center.x, 0.0f);
    EXPECT_FLOAT_EQ(center.y, 0.0f);
}

} // namespace gladius::ui::tests
