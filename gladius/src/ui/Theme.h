#pragma once

#include "imgui.h"

#include <array>
#include <string>

namespace gladius
{
    enum class ThemeId
    {
        Classic = 0,
        Modern,
        Count
    };

    /// Number of available themes
    constexpr int THEME_COUNT = static_cast<int>(ThemeId::Count);

    /// Human-readable names (indexed by ThemeId)
    inline std::array<char const *, THEME_COUNT> const & themeNames()
    {
        static std::array<char const *, THEME_COUNT> const names = {"Classic", "Modern"};
        return names;
    }

    /// Convert a persisted string back to ThemeId (defaults to Modern)
    ThemeId themeIdFromString(std::string const & name);

    /// Get the persisted name for a ThemeId
    char const * themeIdToString(ThemeId id);

    /// Apply the given theme to ImGui
    void applyTheme(ThemeId id);
} // namespace gladius
