#include "Theme.h"

namespace gladius
{
    namespace
    {
        void applyClassicTheme()
        {
            ImGui::StyleColorsDark();
            ImGuiStyle & style = ImGui::GetStyle();

            style.FrameRounding = 12.0f;
            style.Alpha = 1.0f;
            style.FrameBorderSize = 1.0f;
            style.ItemSpacing = {9.f, 7.f};
            style.FramePadding.x = 20;
            style.WindowPadding.x = 20;
            style.WindowBorderSize = 0;

            ImVec4 * colors = style.Colors;
            colors[ImGuiCol_FrameBg] = {0.1f, 0.1f, 0.1f, 1.0f};
            colors[ImGuiCol_FrameBgHovered] = {0.4f, 0.4f, 0.4f, 1.0f};
            colors[ImGuiCol_FrameBgActive] = {0.6f, 0.6f, 0.6f, 1.0f};

            colors[ImGuiCol_TitleBgActive] = {0.32f, 0.32f, 0.32f, 1.00f};
            colors[ImGuiCol_CheckMark] = {0.97f, 0.97f, 0.97f, 1.00f};
            colors[ImGuiCol_SliderGrab] = {1.0f, 0.f, 0.f, 1.0f};
            colors[ImGuiCol_SliderGrabActive] = {1.0f, 0.1f, 0.1f, 1.0f};
            colors[ImGuiCol_Button] = {0.94f, 0.94f, 0.94f, 0.30f};
            colors[ImGuiCol_ButtonHovered] = {0.8f, 0.8f, 0.8f, 0.70f};
            colors[ImGuiCol_ButtonActive] = {1.0f, 0.00f, 0.00f, 1.00f};
            colors[ImGuiCol_Header] = {0.97f, 0.97f, 0.97f, 0.31f};
            colors[ImGuiCol_HeaderHovered] = {1.00f, 0.00f, 0.00f, 0.80f};
            colors[ImGuiCol_HeaderActive] = {1.0f, 0.f, 0.f, 1.0f};
            colors[ImGuiCol_SeparatorHovered] = {0.75f, 0.10f, 0.10f, 0.78f};
            colors[ImGuiCol_SeparatorActive] = {0.75f, 0.10f, 0.10f, 1.00f};
            colors[ImGuiCol_ResizeGrip] = {0.97f, 0.97f, 0.97f, 0.25f};
            colors[ImGuiCol_ResizeGripHovered] = {0.99f, 0.99f, 0.99f, 0.67f};
            colors[ImGuiCol_ResizeGripActive] = {1.00f, 1.00f, 1.00f, 0.95f};
            colors[ImGuiCol_Tab] = {0.25f, 0.25f, 0.26f, 0.86f};
            colors[ImGuiCol_TabHovered] = {0.71f, 0.00f, 0.00f, 0.80f};
            colors[ImGuiCol_TabActive] = {1.00f, 0.01f, 0.01f, 1.00f};
            colors[ImGuiCol_TabUnfocused] = {0.16f, 0.16f, 0.17f, 0.97f};
            colors[ImGuiCol_TabUnfocusedActive] = {0.41f, 0.41f, 0.41f, 1.00f};
            colors[ImGuiCol_TextSelectedBg] = {1.00f, 0.00f, 0.00f, 0.35f};
            colors[ImGuiCol_NavHighlight] = {1.00f, 0.27f, 0.27f, 1.00f};
            colors[ImGuiCol_PlotHistogram] = {1.0f, 0.f, 0.f, 1.0f};
            colors[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.8f};
        }

        void applyModernTheme()
        {
            ImGui::StyleColorsDark();
            ImGuiStyle & style = ImGui::GetStyle();

            // ── Geometry ───────────────────────────────────────────────
            style.FrameRounding = 6.0f;
            style.GrabRounding = 4.0f;
            style.WindowRounding = 6.0f;
            style.ChildRounding = 4.0f;
            style.PopupRounding = 4.0f;
            style.ScrollbarRounding = 4.0f;
            style.TabRounding = 4.0f;
            style.Alpha = 1.0f;

            // ── Spacing & Padding ──────────────────────────────────────
            style.ItemSpacing = {8.f, 6.f};
            style.ItemInnerSpacing = {6.f, 4.f};
            style.FramePadding = {10.f, 6.f};
            style.WindowPadding = {12.f, 8.f};
            style.CellPadding = {6.f, 3.f};
            style.IndentSpacing = 18.f;
            style.ScrollbarSize = 12.f;
            style.GrabMinSize = 10.f;

            // ── Borders ────────────────────────────────────────────────
            style.WindowBorderSize = 1.0f;
            style.FrameBorderSize = 0.0f;
            style.ChildBorderSize = 1.0f;
            style.PopupBorderSize = 1.0f;
            style.TabBorderSize = 0.0f;
            style.SeparatorTextBorderSize = 1.0f;

            // ── Accent palette ─────────────────────────────────────────
            ImVec4 const accent = {0.85f, 0.18f, 0.18f, 1.00f};
            ImVec4 const accentHover = {0.92f, 0.25f, 0.25f, 1.00f};
            ImVec4 const accentActive = {0.75f, 0.12f, 0.12f, 1.00f};
            ImVec4 const accentDim = {0.55f, 0.12f, 0.12f, 0.80f};

            // Surface layers (dark to light for visual depth)
            ImVec4 const surface0 = {0.10f, 0.10f, 0.11f, 1.00f};
            ImVec4 const surface1 = {0.14f, 0.14f, 0.15f, 1.00f};
            ImVec4 const surface2 = {0.18f, 0.18f, 0.20f, 1.00f};
            ImVec4 const surface3 = {0.24f, 0.24f, 0.26f, 1.00f};
            ImVec4 const border = {0.28f, 0.28f, 0.30f, 0.60f};

            // Text hierarchy
            ImVec4 const textPrimary = {0.92f, 0.92f, 0.93f, 1.00f};
            ImVec4 const textDisabled = {0.42f, 0.42f, 0.44f, 1.00f};

            // ── Color assignments ──────────────────────────────────────
            ImVec4 * colors = style.Colors;

            colors[ImGuiCol_Text] = textPrimary;
            colors[ImGuiCol_TextDisabled] = textDisabled;

            colors[ImGuiCol_WindowBg] = surface1;
            colors[ImGuiCol_ChildBg] = {0.0f, 0.0f, 0.0f, 0.0f};
            colors[ImGuiCol_PopupBg] = {0.12f, 0.12f, 0.13f, 0.96f};
            colors[ImGuiCol_FrameBg] = surface0;
            colors[ImGuiCol_FrameBgHovered] = surface2;
            colors[ImGuiCol_FrameBgActive] = surface3;

            colors[ImGuiCol_Border] = border;
            colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};

            colors[ImGuiCol_TitleBg] = surface0;
            colors[ImGuiCol_TitleBgActive] = surface2;
            colors[ImGuiCol_TitleBgCollapsed] = {0.10f, 0.10f, 0.11f, 0.50f};

            colors[ImGuiCol_MenuBarBg] = surface0;

            colors[ImGuiCol_ScrollbarBg] = {0.08f, 0.08f, 0.09f, 0.60f};
            colors[ImGuiCol_ScrollbarGrab] = surface3;
            colors[ImGuiCol_ScrollbarGrabHovered] = {0.34f, 0.34f, 0.36f, 1.00f};
            colors[ImGuiCol_ScrollbarGrabActive] = {0.42f, 0.42f, 0.44f, 1.00f};

            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = accentHover;
            colors[ImGuiCol_Button] = surface2;
            colors[ImGuiCol_ButtonHovered] = surface3;
            colors[ImGuiCol_ButtonActive] = accent;

            colors[ImGuiCol_Header] = {0.22f, 0.22f, 0.24f, 0.60f};
            colors[ImGuiCol_HeaderHovered] = accentDim;
            colors[ImGuiCol_HeaderActive] = accent;

            colors[ImGuiCol_Separator] = border;
            colors[ImGuiCol_SeparatorHovered] = accentDim;
            colors[ImGuiCol_SeparatorActive] = accent;

            colors[ImGuiCol_ResizeGrip] = {0.40f, 0.40f, 0.42f, 0.25f};
            colors[ImGuiCol_ResizeGripHovered] = accentDim;
            colors[ImGuiCol_ResizeGripActive] = accent;

            colors[ImGuiCol_Tab] = surface2;
            colors[ImGuiCol_TabHovered] = accentHover;
            colors[ImGuiCol_TabActive] = accent;
            colors[ImGuiCol_TabUnfocused] = surface1;
            colors[ImGuiCol_TabUnfocusedActive] = surface3;

            colors[ImGuiCol_TextSelectedBg] = {accent.x, accent.y, accent.z, 0.30f};
            colors[ImGuiCol_NavHighlight] = accent;
            colors[ImGuiCol_DragDropTarget] = {accent.x, accent.y, accent.z, 0.90f};

            colors[ImGuiCol_PlotHistogram] = accent;
            colors[ImGuiCol_PlotHistogramHovered] = accentHover;
            colors[ImGuiCol_PlotLines] = {0.62f, 0.62f, 0.64f, 1.00f};
            colors[ImGuiCol_PlotLinesHovered] = accent;

            colors[ImGuiCol_TableHeaderBg] = surface2;
            colors[ImGuiCol_TableBorderStrong] = border;
            colors[ImGuiCol_TableBorderLight] = {0.22f, 0.22f, 0.24f, 0.40f};
            colors[ImGuiCol_TableRowBg] = {0.0f, 0.0f, 0.0f, 0.0f};
            colors[ImGuiCol_TableRowBgAlt] = {0.08f, 0.08f, 0.09f, 0.40f};

            colors[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.70f};
        }
    } // anonymous namespace

    ThemeId themeIdFromString(std::string const & name)
    {
        auto const & names = themeNames();
        for (int i = 0; i < THEME_COUNT; ++i)
        {
            if (name == names[i])
            {
                return static_cast<ThemeId>(i);
            }
        }
        return ThemeId::Modern;
    }

    char const * themeIdToString(ThemeId id)
    {
        auto idx = static_cast<int>(id);
        if (idx >= 0 && idx < THEME_COUNT)
        {
            return themeNames()[idx];
        }
        return themeNames()[static_cast<int>(ThemeId::Modern)];
    }

    void applyTheme(ThemeId id)
    {
        switch (id)
        {
        case ThemeId::Classic:
            applyClassicTheme();
            break;
        case ThemeId::Modern:
            applyModernTheme();
            break;
        default:
            applyModernTheme();
            break;
        }
    }
} // namespace gladius
