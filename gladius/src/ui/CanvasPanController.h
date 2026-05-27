#pragma once

#include "GamepadState.h"

#include <imgui.h>
#include <string>

namespace gladius::ui {

/**
 * @brief Controls canvas panning and zooming via gamepad input.
 *
 * This class handles analog stick-based canvas panning and trigger/stick-based zoom.
 * It integrates with imguinodeeditor's pan and zoom system.
 *
 * Integration:
 * - Left stick for canvas panning (when not navigating nodes)
 * - Right stick or triggers for zoom
 * - Pass pan offset to ed::SetOffset() equivalent
 */
class CanvasPanController {
  public:
    CanvasPanController();
    ~CanvasPanController() = default;

    // Non-copyable
    CanvasPanController(CanvasPanController const &) = delete;
    CanvasPanController & operator=(CanvasPanController const &) = delete;

    /**
     * @brief Update controller state from gamepad input.
     * @param gamepad Current gamepad state
     * @param deltaTime Delta time in seconds (for frame-rate independent movement)
     */
    void update(GamepadState & gamepad, float deltaTime);

    /**
     * @brief Apply pan offset to the node editor.
     *        Call this after updating from gamepad input.
     */
    void applyPan();

    /**
     * @brief Update zoom level from gamepad input.
     * @param gamepad Current gamepad state
     */
    void updateZoom(GamepadState & gamepad);

    /**
     * @brief Reset pan and zoom to defaults.
     */
    void reset();

    // --- State queries ---

    /**
     * @brief Get the current pan offset.
     * @return ImVec2 containing (x, y) pan offset
     */
    [[nodiscard]] ImVec2 panOffset() const;

    /**
     * @brief Get the current zoom level.
     * @return Current zoom factor (1.0 = 100%)
     */
    [[nodiscard]] float zoomLevel() const;

    /**
     * @brief Get the recommended zoom level for displaying.
     * @return Zoom level as percentage string (e.g., "150%")
     */
    [[nodiscard]] std::string zoomDisplay() const;

    /**
     * @brief Check if panning is active (left stick within deadzone).
     * @return true if left stick is active for panning
     */
    [[nodiscard]] bool isPanning() const;

    /**
     * @brief Check if zooming is active (triggers or right stick).
     * @return true if zoom input is detected
     */
    [[nodiscard]] bool isZooming() const;

    // --- Direct state setters (for integration with editor) ---

    /**
     * @brief Set the pan offset directly.
     * @param offset New pan offset
     */
    void setPanOffset(ImVec2 offset);

    /**
     * @brief Set the zoom level directly.
     * @param level New zoom level (1.0 = 100%)
     */
    void setZoomLevel(float level);

  private:
    ImVec2 m_panOffset{0, 0};
    float m_zoomLevel{1.0f};
    bool m_isPanning{false};
    bool m_isZooming{false};

    // Configuration
    float m_deadzone{0.25f};       // Deadzone for analog sticks
    float m_panSpeed{200.0f};      // Pixels per second for panning
    float m_zoomSpeed{0.5f};       // Zoom speed for triggers/stick

    // Min/max zoom limits
    float m_minZoom{0.1f};         // Minimum zoom (10%)
    float m_maxZoom{5.0f};         // Maximum zoom (500%)
};

} // namespace gladius::ui
