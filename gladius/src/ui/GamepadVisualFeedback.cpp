#include "GamepadVisualFeedback.h"

#include <imgui.h>

namespace gladius::ui
{

namespace
{

/// @brief Get the current time for animation purposes.
/// @return Time in seconds (approximate)
float currentTime()
{
    return static_cast<float>(ImGui::GetTime());
}

} // anonymous namespace

void GamepadVisualFeedback::update(float deltaTime)
{
    updateToasts(deltaTime);
}

void GamepadVisualFeedback::renderNodeHoverRing(nodes::NodeId focusedNode, ImVec2 center, ImVec2 size, bool isSelected)
{
    if (focusedNode == 0)
    {
        return;
    }

    // Calculate pulse based on time
    float pulse = 1.0f + sin(currentTime() * m_ringPulseSpeed) * m_ringPulseAmount;
    ImVec4 color = isSelected ? m_ringColorSelected : m_ringColor;
    color = ImVec4(color.x, color.y, color.z, pulse);

    float radius = 8.0f * pulse;

    // Draw the hover ring
    drawHoverRing(center, size, radius, color);
}

void GamepadVisualFeedback::showToast(std::string const & message, float duration)
{
    if (message.empty())
    {
        return;
    }

    // Remove oldest toast if at capacity
    if (static_cast<int>(m_toasts.size()) >= MAX_TOASTS)
    {
        m_toasts.erase(m_toasts.begin());
    }

    // Add new toast
    ToastNotification toast;
    toast.message = message;
    toast.duration = duration;
    toast.lifetime = 0.0f;
    toast.active = true;
    toast.textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    toast.position = ImVec2(20.0f, 20.0f);

    m_toasts.push_back(toast);
}

void GamepadVisualFeedback::renderContextIndicator(bool connected) const
{
    if (!connected)
    {
        return;
    }

    // Position in bottom-right corner
    ImGuiIO & io = ImGui::GetIO();
    ImVec2 position(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 40.0f);

    // Draw a small circle with gamepad icon/text
    ImVec2 textPos = position;

    // Background circle
    ImDrawList * drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(ImVec2(position.x, position.y), 16.0f,
                              ImGui::GetColorU32(m_indicatorColor));

    // Text "GP" inside
    char buf[4] = "GP";
    ImFont * fontSmall = ImGui::GetFont();
    ImVec2 textSize = fontSmall->CalcTextSizeA(12.0f, 1000.0f, -1, buf);
    ImVec2 textScreenPos = ImVec2(position.x - textSize.x * 0.5f,
                                  position.y - textSize.y * 0.5f);
    drawList->AddText(textScreenPos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)), buf);
}

void GamepadVisualFeedback::renderMenuItemHighlight(int menuItemIndex, int itemCount) const
{
    // This would highlight menu items in a popup menu
    // Implementation depends on how the menu system is structured
    // For now, this is a placeholder for integration
}

bool GamepadVisualFeedback::isActive() const
{
    for (auto const & toast : m_toasts)
    {
        if (toast.active)
        {
            return true;
        }
    }
    return false;
}

void GamepadVisualFeedback::clear()
{
    m_toasts.clear();
}

void GamepadVisualFeedback::drawHoverRing(ImVec2 center, ImVec2 size, float radius, ImVec4 color)
{
    ImDrawList * drawList = ImGui::GetWindowDrawList();

    // Calculate rectangle corners
    ImVec2 min = ImVec2(center.x - size.x * 0.5f - radius,
                        center.y - size.y * 0.5f - radius);
    ImVec2 max = ImVec2(center.x + size.x * 0.5f + radius,
                        center.y + size.y * 0.5f + radius);

    // Draw rounded rectangle outline
    float rounding = 8.0f;
    float thickness = 3.0f;

    // Convert color to uint32 for draw list
    ImU32 colorUint = ImGui::GetColorU32(color);

    drawRoundedRectOutline(min, max, rounding, thickness, colorUint);
}

void GamepadVisualFeedback::drawRoundedRectOutline(ImVec2 min, ImVec2 max, float rounding, float thickness, ImU32 color)
{
    ImDrawList * drawList = ImGui::GetWindowDrawList();

    // Expand rect outward by half thickness
    ImVec2 halfThickness = ImVec2(thickness * 0.5f, thickness * 0.5f);
    min = ImVec2(min.x - halfThickness.x, min.y - halfThickness.y);
    max = ImVec2(max.x + halfThickness.x, max.y + halfThickness.y);

    // Draw rounded rectangle outline
    drawList->AddRectFilled(min, max, color, rounding);

    // Draw inner rect with background color to create outline effect
    ImVec2 innerMin = ImVec2(min.x + thickness, min.y + thickness);
    ImVec2 innerMax = ImVec2(max.x - thickness, max.y - thickness);

    // Use background color for the inner fill
    ImU32 bgColor = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
    drawList->AddRectFilled(innerMin, innerMax, bgColor, rounding);
}

void GamepadVisualFeedback::renderToast(ToastNotification const & toast) const
{
    if (!toast.active || toast.lifetime >= toast.duration)
    {
        return;
    }

    // Calculate alpha based on lifetime (fade in/out)
    float fadeInTime = 0.3f;
    float fadeOutStart = toast.duration - 0.5f;
    float alpha = 1.0f;

    if (toast.lifetime < fadeInTime)
    {
        alpha = toast.lifetime / fadeInTime;
    }
    else if (toast.lifetime > fadeOutStart)
    {
        alpha = (toast.duration - toast.lifetime) / (toast.duration - fadeOutStart);
    }

    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // Position at top-center of screen
    ImGuiIO & io = ImGui::GetIO();
    ImVec2 textSize = ImGui::CalcTextSize(toast.message.c_str());
    ImVec2 position(io.DisplaySize.x * 0.5f - textSize.x * 0.5f, 60.0f);

    // Draw background
    ImVec2 padding(20.0f, 10.0f);
    ImVec2 rectMin = ImVec2(position.x - padding.x, position.y - padding.y);
    ImVec2 rectMax = ImVec2(position.x + textSize.x + padding.x,
                            position.y + textSize.y + padding.y);

    ImDrawList * drawList = ImGui::GetWindowDrawList();
    ImU32 bgColor = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.7f * alpha));
    ImU32 textColor = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));

    drawList->AddRectFilled(rectMin, rectMax, bgColor, 8.0f);
    drawList->AddText(position, textColor, toast.message.c_str());
}

void GamepadVisualFeedback::updateToasts(float deltaTime)
{
    for (auto it = m_toasts.begin(); it != m_toasts.end();)
    {
        it->lifetime += deltaTime;
        if (it->lifetime >= it->duration)
        {
            it = m_toasts.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace gladius::ui
