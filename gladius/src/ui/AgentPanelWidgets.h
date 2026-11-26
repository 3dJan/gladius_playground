#pragma once

#include <cstddef>
#include <imgui.h>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Layout and drawing helpers that give the Design Agent panel its look.
     * @details Drawing only: they never read design state, so sections stay readable and the
     *          headless tests can keep asserting on geometry. Text that needs a stable row height
     *          is measured up front and drawn through the draw list.
     */
    namespace agentWidgets
    {
        /// @brief Accent colour of the active theme, shared with the rail
        [[nodiscard]] ImVec4 accent();

        /// @brief One soft-wrapped line of a string, as a [begin,end) span into the original text
        struct TextSpan
        {
            char const * begin = nullptr;
            char const * end = nullptr;
        };

        /**
         * @brief Splits text into soft-wrapped lines for the current font at the given width.
         * @param text Text to wrap; may be empty
         * @param wrapWidth Available width in pixels
         * @param maxLines 0 keeps every line, otherwise the result is capped and the last visible
         *                 line is truncated with an ellipsis
         */
        [[nodiscard]] std::vector<TextSpan> measureWrappedLines(char const * text,
                                                                float wrapWidth,
                                                                std::size_t maxLines = 0u);

        /// @brief Draws wrapped lines top-down at a fixed line height
        void drawWrappedLines(ImDrawList & drawList,
                              ImVec2 pos,
                              std::vector<TextSpan> const & lines,
                              ImVec4 color,
                              float lineHeight);

        /// @brief Draws one clipped line at pos, eliding it when it does not fit
        void drawClippedText(ImDrawList & drawList,
                             ImVec2 pos,
                             ImVec4 color,
                             char const * text,
                             float maxWidth);

        /// @brief Pseudo-bold: the loaded font has no bold face, so a small offset overdraw does it
        void drawEmphasized(ImDrawList & drawList, ImVec2 pos, ImVec4 color, char const * text);

        /// @brief Section heading with an icon; chevronDown marks a collapsible heading
        void sectionLabel(char const * icon, char const * label, bool chevronDown = false);

        /// @brief Flat chevron row that replaces a heavy CollapsingHeader
        /// @param open In/out open flag owned by the caller
        /// @return True when the row is open
        bool toggleRow(char const * id, char const * icon, char const * label, bool & open);

        /// @brief Right-aligned muted key column; returns the width left for the value control
        float beginKeyColumn(char const * key);

        /**
         * @brief One key/value row: right-aligned muted key, value control filling the rest.
         * @return True when the value text was edited
         */
        bool keyValueRow(char const * key, char * valueBuf, std::size_t valueBufSize);

        /// @brief Same as keyValueRow with placeholder text while the value is empty
        bool keyValueRowWithHint(char const * key,
                                  char const * hint,
                                  char * valueBuf,
                                  std::size_t valueBufSize);


        /// @brief Rect reserved for one row or card, with the hit area ImGui already knows about
        struct ReservedRow
        {
            ImVec2 origin{};
            ImVec2 max{};
            bool hovered = false;
            [[nodiscard]] ImVec2 size() const noexcept
            {
                return ImVec2(max.x - origin.x, max.y - origin.y);
            }
        };

        /**
         * @brief Reserves a row rect of the given height and leaves the cursor at its top-left.
         * @details Submitting the hit area first is what lets the selection band be painted under
         *          the widgets that follow; the caller then draws its widgets in normal flow.
         */
        ReservedRow reserveRow(float height, char const * id = "##row");

        /// @brief Moves the cursor below a previously reserved row
        void endReservedRow(ReservedRow const & row);

        /// @brief Rounded band behind a reserved row when it is selected or hovered
        void drawRowBand(ReservedRow const & row, bool selected, bool hovered);

        /// @brief Rounded band behind an arbitrary rect for selected or hovered list rows
        void drawBand(ImDrawList & drawList, ImVec2 min, ImVec2 max, bool selected, bool hovered);

        /// @brief Rounded band behind the current item for selected or hovered list rows
        void drawRowBand(bool selected, bool hovered);

        /**
         * @brief Draws a preview inside a circle, or an honest placeholder ring when there is none.
         * @param textureId 0 when no preview bytes produced a texture
         */
        void drawCircularThumb(ImDrawList & drawList,
                               ImVec2 center,
                               float radius,
                               unsigned int textureId);

        /// @brief Filled circle chip with a centred capital letter (A, B, C ...)
        void drawLetterChip(ImDrawList & drawList,
                            ImVec2 center,
                            float radius,
                            char letter,
                            ImVec4 color);
    } // namespace agentWidgets
} // namespace gladius::ui
