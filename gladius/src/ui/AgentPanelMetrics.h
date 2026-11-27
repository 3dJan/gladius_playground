#pragma once

namespace gladius::ui
{
    /**
     * @brief Layout tokens shared by the Design Agent panel: rail, sections, cards and composer.
     * @details Every length is a multiple of the font size so the panel keeps its proportions at any
     *          UI scale. Add a token here instead of repeating a hard-coded pixel value.
     */
    namespace agentMetrics
    {
        // --- Phase rail ---
        constexpr float railRadius = 0.60f;
        /// @brief Extra ring radius the active phase breathes with
        constexpr float railActiveRingGap = 0.16f;
        constexpr float railLineWidth = 0.13f;
        constexpr float railDashOn = 0.26f;
        constexpr float railDashOff = 0.20f;
        /// @brief Vertical gap between two phase rows
        constexpr float railRowSpacing = 0.50f;
        /// @brief Distance from the window's left edge to the rail line
        constexpr float railOffset = 0.85f;
        /// @brief Gap between the rail line and the phase text block
        constexpr float railTextGap = 1.25f;
        /// @brief Space the expand/collapse chevron takes in front of a phase title
        constexpr float chevronGutter = 1.15f;
        /// @brief Space below a collapsed row before the next phase starts
        constexpr float rowPadding = 0.20f;
        constexpr float glowRingRadius = 0.26f;

        // --- Artifact sections ---
        /// @brief Icon column width at the start of an artifact section row
        constexpr float sectionIconGutter = 1.15f;
        /// @brief Indent of artifact content relative to the phase text block
        constexpr float sectionIndent = 0.90f;
        constexpr float sectionSpacing = 0.45f;
        /// @brief Width of the right-aligned key column of a key/value row
        constexpr float keyColumnWidth = 6.5f;

        // --- Cards (variants and configurations) ---
        constexpr float cardPadding = 0.45f;
        constexpr float cardRounding = 0.35f;
        constexpr float cardThumbSize = 2.30f;
        /// @brief Rounding of the selection band behind list rows
        constexpr float rowRounding = 0.28f;

        // --- Conversation transcript ---
        // The chips sit on the rail column (railOffset) and the turn bodies on the phase title
        // column (railOffset + railTextGap + chevronGutter), so the transcript reads as one
        // continuation of the rail above it instead of a second, unrelated list.
        /// @brief Radius of the role chip left of a turn
        constexpr float roleChipRadius = 0.56f;
        /// @brief Space above and below the turn content inside its hover band
        constexpr float turnPadding = 0.30f;
        /// @brief Space between two turns of the same speaker
        constexpr float turnSpacing = 0.45f;
        /// @brief Extra space where the speaker changes
        constexpr float turnBreakSpacing = 0.55f;
        /// @brief Gap between a turn's role line and its body
        constexpr float turnRoleLineGap = 0.16f;
        /// @brief Opacity of muted turn bodies (system notes, tool traces)
        constexpr float mutedBodyAlpha = 0.72f;

        // --- Header ---
        constexpr float headerPadding = 0.55f;
        constexpr float headerTitleLine = 1.35f;

        // --- Composer and footer ---
        constexpr float composerMinHeight = 1.95f;
        constexpr float sendButtonSize = 1.55f;
        constexpr float compactButtonHeight = 1.25f;

        // --- Panel window (viewport relative / pixels, not font relative) ---
        /// @brief Share of the dockspace width the panel asks for when docked
        constexpr float panelDefaultWidthRatio = 0.30f;
        constexpr float panelMinWidthPx = 360.f;
        constexpr float panelMaxWidthPx = 520.f;
    } // namespace agentMetrics
} // namespace gladius::ui
