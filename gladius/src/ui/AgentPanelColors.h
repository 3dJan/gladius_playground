#pragma once

#include <imgui.h>

namespace gladius::ui
{
    /// @brief Status colours shared by the Design Agent panel and its footer
    inline ImVec4 colorOk()
    {
        return {0.40f, 0.82f, 0.46f, 1.f};
    }

    inline ImVec4 colorWarn()
    {
        return {1.00f, 0.72f, 0.32f, 1.f};
    }

    inline ImVec4 colorInfo()
    {
        return {0.40f, 0.82f, 0.98f, 1.f};
    }

    inline ImVec4 colorError()
    {
        return {1.00f, 0.42f, 0.38f, 1.f};
    }
} // namespace gladius::ui
