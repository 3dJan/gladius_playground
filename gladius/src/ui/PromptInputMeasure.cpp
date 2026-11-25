// Measurement and ImGui-free geometry math for the prompt composer card. The card itself is
// rendered in PromptInput.cpp, the text helpers live in PromptInputText.cpp and the large-form
// editor in PromptInputExpanded.cpp.

#include "PromptInput.h"
#include "PromptInputShared.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <vector>

namespace gladius::ui
{
    float PromptInput::trailingToolbarWidth() const noexcept
    {
        return m_config.iconButtonSize * 2.f + ImGui::GetStyle().ItemSpacing.x;
    }
    float PromptInput::cardOuterWidth() const
    {
        // Measured from the window work rect, not the cursor: the width is needed before the
        // footer is submitted, while the cursor still sits after other items.
        return std::max(80.f, ImGui::GetCurrentWindow()->WorkRect.GetWidth());
    }
    float PromptInput::innerFieldWidth() const
    {
        float const inner
          = cardOuterWidth() - 2.f * (m_config.cardPadding + kCardBorderThickness);
        return std::max(40.f, inner);
    }
    float PromptInput::measureInlineHeight(float fieldWidth, float maxHeightLimit) const
    {
        float const lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float const innerWidth =
          std::max(1.f, fieldWidth - ImGui::GetStyle().FramePadding.x * 2.f);
        std::size_t const charsPerLine =
          std::max<std::size_t>(1u, static_cast<std::size_t>(innerWidth / averageGlyphWidth()));
        std::size_t const lines = countVisualLines(m_draft, charsPerLine);
        float const maxHeight = maxHeightLimit > 0.f
                                  ? std::max(m_config.minHeight,
                                             std::min(m_config.maxHeight, maxHeightLimit))
                                  : m_config.maxHeight;
        return computeFieldHeight(lines,
                                  lineHeight,
                                  ImGui::GetStyle().FramePadding.y,
                                  m_config.minHeight,
                                  maxHeight);
    }
    float PromptInput::toolbarHeight() const
    {
        return std::max(m_config.iconButtonSize, ImGui::GetFrameHeight());
    }
    float PromptInput::attachmentStripHeight(std::size_t attachmentCount) const
    {
        return computeAttachmentStripHeight(attachmentCount,
                                            m_config.attachmentThumbSize,
                                            attachmentRemoveButtonWidth(),
                                            innerFieldWidth(),
                                            ImGui::GetStyle().ItemSpacing.x);
    }
    float PromptInput::attachmentRemoveButtonWidth() const
    {
        ImGuiStyle const & style = ImGui::GetStyle();
        return ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f;
    }

    PromptInput::CardMetrics PromptInput::measureCard(float maxHeightLimit,
                                                      std::size_t attachmentCount) const
    {
        CardMetrics metrics;
        float const innerWidth = innerFieldWidth();
        ToolbarLayout const layout
          = computeToolbarLayout(innerWidth,
                                 m_config.iconButtonSize,
                                 m_config.leadingToolbarWidth,
                                 m_config.minComboWidth,
                                 ImGui::GetStyle().ItemSpacing.x);
        metrics.toolbarRows = layout.rowCount;
        metrics.attachmentsHeight = attachmentStripHeight(attachmentCount);

        float const chrome = computeCardHeight(0.f,
                                               metrics.attachmentsHeight,
                                               toolbarHeight(),
                                               metrics.toolbarRows,
                                               m_config.cardPadding,
                                               kSeparatorThickness,
                                               ImGui::GetStyle().ItemSpacing.y);
        metrics.fieldHeight = measureInlineHeight(innerWidth, 0.f);
        metrics.cardHeight = chrome + metrics.fieldHeight;

        if (maxHeightLimit > 0.f && metrics.cardHeight > maxHeightLimit)
        {
            metrics.fieldHeight = measureInlineHeight(innerWidth,
                                                      std::max(0.f, maxHeightLimit - chrome));
            metrics.cardHeight = chrome + metrics.fieldHeight;
        }
        return metrics;
    }

    bool PromptInput::shouldShowPlaceholder(std::string const & draft, char const * placeholder)
    {
        return placeholder != nullptr && placeholder[0] != '\0' && draft.empty();
    }

    float PromptInput::computeComboWidth(float hostWidth, float leadingWidth, float itemSpacingX)
    {
        // hostWidth is what is left of the row once the icon buttons and their gap are subtracted.
        // The remaining chain is [leading gap] combo gap combo, so a leading control adds one more
        // gap.
        float const gaps = leadingWidth > 0.f ? 2.f : 1.f;
        return std::max(1.f, (hostWidth - leadingWidth - gaps * itemSpacingX) * 0.5f);
    }

    PromptInput::ToolbarLayout PromptInput::computeToolbarLayout(float innerWidth,
                                                                 float iconButtonSize,
                                                                 float leadingWidth,
                                                                 float minComboWidth,
                                                                 float itemSpacingX)
    {
        ToolbarLayout layout;
        layout.innerWidth = innerWidth;
        layout.hostWidth = std::max(0.f, innerWidth - (iconButtonSize * 2.f + itemSpacingX));
        layout.leadingWidth = leadingWidth;
        layout.iconButtonWidth = iconButtonSize;
        layout.drawLeading = true;
        layout.drawCombos = true;

        // Row 0 carries the leading controls plus both combos; if the combos cannot keep their
        // minimum width there, they get a row of their own and split the full host width.
        float const sharedWidth = computeComboWidth(layout.hostWidth, leadingWidth, itemSpacingX);
        layout.wrapped = sharedWidth < minComboWidth;
        layout.rowCount = layout.wrapped ? 2 : 1;
        // The combos live on the last row: row 0 when they fit, otherwise their own row.
        float const combosLeading = layout.wrapped ? 0.f : leadingWidth;
        layout.comboWidth = computeComboWidth(layout.hostWidth, combosLeading, itemSpacingX);
        return layout;
    }

    float PromptInput::computeCardHeight(float fieldHeight,
                                         float attachmentsHeight,
                                         float toolbarRowHeight,
                                         int toolbarRows,
                                         float padding,
                                         float separatorThickness,
                                         float verticalSpacing)
    {
        int const rows = 1 + (attachmentsHeight > 0.f ? 1 : 0) + 1 + std::max(1, toolbarRows);
        float const content = fieldHeight + attachmentsHeight + separatorThickness
                            + static_cast<float>(std::max(1, toolbarRows)) * toolbarRowHeight;
        return padding * 2.f + content + verticalSpacing * static_cast<float>(rows - 1);
    }

    float PromptInput::computeAttachmentStripHeight(std::size_t attachmentCount,
                                                    float thumbSize,
                                                    float extraThumbWidth,
                                                    float availableWidth,
                                                    float itemSpacingX)
    {
        if (attachmentCount == 0u)
        {
            return 0.f;
        }

        float const cellWidth = thumbSize + extraThumbWidth;
        std::vector<float> cells(attachmentCount, cellWidth);
        std::size_t const rows = countWrappedRows(cells, availableWidth, itemSpacingX);
        return static_cast<float>(rows) * thumbSize
             + static_cast<float>(rows - 1u) * itemSpacingX;
    }

    std::size_t PromptInput::countWrappedRows(std::vector<float> const & itemWidths,
                                              float availableWidth,
                                              float itemSpacingX)
    {
        if (itemWidths.empty())
        {
            return 0u;
        }

        float const width = std::max(1.f, availableWidth);
        std::size_t rows = 1u;
        float used = 0.f;
        for (float itemWidth : itemWidths)
        {
            float const needed = used > 0.f ? itemSpacingX + itemWidth : itemWidth;
            if (used > 0.f && used + needed > width)
            {
                ++rows;
                used = itemWidth;
                continue;
            }
            used += needed;
        }
        return rows;
    }
} // namespace gladius::ui
