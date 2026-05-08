#include "CanvasPanController.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace gladius::ui {

namespace {

// Helper: Apply deadzone to analog stick value
float applyDeadzone(float value, float deadzone) {
    if (std::abs(value) < deadzone) {
        return 0.0f;
    }
    
    // Remap value outside deadzone to [0, 1] or [-1, 0]
    if (value > 0.0f) {
        return (value - deadzone) / (1.0f - deadzone);
    } else {
        return (value + deadzone) / (1.0f - deadzone);
    }
}

} // anonymous namespace

// ============================================================================
// CanvasPanController implementation
// ============================================================================

CanvasPanController::CanvasPanController() = default;

void CanvasPanController::update(GamepadState & gamepad, float deltaTime) {
    // Left stick for panning
    Eigen::Vector2f leftStick = gamepad.getLeftStick();
    if (leftStick.squaredNorm() > 0.0f) {
        float deadzoneX = applyDeadzone(leftStick.x(), m_deadzone);
        float deadzoneY = applyDeadzone(leftStick.y(), m_deadzone);
        
        m_panOffset.x += deadzoneX * m_panSpeed * deltaTime;
        m_panOffset.y += deadzoneY * m_panSpeed * deltaTime;
    }
}

void CanvasPanController::applyPan() {
    // The pan offset is applied by the ModelEditor/MainWindow integration
    // This method exists for potential direct editor context access
}

void CanvasPanController::updateZoom(GamepadState & gamepad) {
    float zoomDelta = 0.0f;
    
    // Right stick up/down for zoom (Y axis inverted: up = zoom in)
    float rightStickY = gamepad.isButtonHeld(GamepadButton::RStick) ? 
                        applyDeadzone(gamepad.getRightStick().y(), m_deadzone) : 0.0f;
    
    if (rightStickY != 0.0f) {
        // Right stick is pressed, use it for zoom
        zoomDelta = -rightStickY * m_zoomSpeed; // Inverted: up = zoom in
    } else {
        // Triggers for zoom (LT = zoom out, RT = zoom in)
        float lt = gamepad.getLeftTrigger();
        float rt = gamepad.getRightTrigger();
        
        if (lt > m_deadzone) {
            zoomDelta = -lt * m_zoomSpeed * 0.5f; // Triggers have less range
        }
        if (rt > m_deadzone) {
            zoomDelta += rt * m_zoomSpeed * 0.5f;
        }
    }
    
    if (zoomDelta != 0.0f) {
        float targetZoom = m_zoomLevel * (1.0f + zoomDelta);
        targetZoom = std::max(m_minZoom, std::min(targetZoom, m_maxZoom));
        m_zoomLevel = targetZoom;
    }
}

void CanvasPanController::reset() {
    m_panOffset = ImVec2{0, 0};
    m_zoomLevel = 1.0f;
}

ImVec2 CanvasPanController::panOffset() const {
    return m_panOffset;
}

float CanvasPanController::zoomLevel() const {
    return m_zoomLevel;
}

std::string CanvasPanController::zoomDisplay() const {
    std::ostringstream oss;
    int percent = static_cast<int>(m_zoomLevel * 100);
    oss << percent << "%";
    return oss.str();
}

bool CanvasPanController::isPanning() const {
    // This would check if left stick is active
    // For now, we check if pan offset has changed recently
    return m_panOffset.x != 0 || m_panOffset.y != 0;
}

bool CanvasPanController::isZooming() const {
    // This would check if triggers or right stick are active
    // For now, we check if zoom level is not at default
    return m_zoomLevel != 1.0f;
}

void CanvasPanController::setPanOffset(ImVec2 offset) {
    m_panOffset = offset;
}

void CanvasPanController::setZoomLevel(float level) {
    m_zoomLevel = std::max(m_minZoom, std::min(level, m_maxZoom));
}

float CanvasPanController::applyDeadzone(float value, float deadzone) const {
    return applyDeadzone(value, deadzone);
}

float CanvasPanController::smoothZoom(float target, float current, float deltaTime) const {
    // Simple exponential smoothing for zoom transitions
    float smoothingFactor = 5.0f;
    float smoothed = current + (target - current) * smoothingFactor * deltaTime;
    return std::max(m_minZoom, std::min(smoothed, m_maxZoom));
}

} // namespace gladius::ui
