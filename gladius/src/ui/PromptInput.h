#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Holds the Design Agent prompt draft and renders it as a chat-style composer card:
     *        an auto-growing borderless multiline field with a placeholder, an attachment strip
     *        and a bottom toolbar that ends in the submit button. An optional large-form editor
     *        window is bound to the same draft.
     *
     * The draft is an unbounded std::string (rendered through the imgui_stdlib
     * InputTextMultiline overload), so long instruction blocks are never truncated. The card
     * leaves its attachment strip and toolbar controls to the host through render callbacks, so
     * provider and attachment state never leaks into this class. All geometry math is exposed as
     * ImGui-free statics for unit testing.
     */
    class PromptInput
    {
      public:
        /// @brief Layout and import limits for the prompt composer
        struct Config
        {
            std::size_t minLines = 2;
            std::size_t maxLines = 10;
            float minHeight = 52.f;
            float maxHeight = 200.f;
            std::size_t maxImportBytes = 256u * 1024u;
            /// @brief Shown inside the field while the draft is empty
            char const * placeholder = "What do you want to build?";
            float cardPadding = 8.f;
            float cardRounding = 8.f;
            /// @brief Edge of the square icon buttons in the toolbar (expand, submit)
            float iconButtonSize = 30.f;
            /// @brief Edge of the attachment thumbnails drawn above the toolbar
            float attachmentThumbSize = 34.f;
            /// @brief Width the host reserves at the toolbar start for its own leading controls
            float leadingToolbarWidth = 70.f;
            /// @brief Combos never shrink below this; below it they move to a second toolbar row
            float minComboWidth = 130.f;
        };

        /// @brief Result of a prompt input render pass
        struct RenderResult
        {
            bool sendRequested = false;
            bool focused = false;
        };

        /// @brief Toolbar geometry handed to the host renderer so it can size its own controls
        struct ToolbarLayout
        {
            float innerWidth = 0.f;
            /// @brief Width the host may use on the row, left of expand and submit
            float hostWidth = 0.f;
            /// @brief Width to give each host combo on the row
            float comboWidth = 0.f;
            /// @brief Width taken by the host's leading controls on this row
            float leadingWidth = 0.f;
            /// @brief Height of the row; host controls should match it
            float rowHeight = 0.f;
            /// @brief Width of the expand and submit icon buttons
            float iconButtonWidth = 0.f;
            /// @brief True when the host draws its leading controls on this row
            bool drawLeading = false;
            /// @brief True when the host draws its combos on this row
            bool drawCombos = false;
            /// @brief True when the combos moved to their own second row
            bool wrapped = false;
            /// @brief Toolbar rows the host must fill (1 or 2)
            int rowCount = 1;
        };

        /// @brief Draws the host controls of one toolbar row; row indices start at 0
        using ToolbarRenderer = std::function<void(ToolbarLayout const &, int row)>;
        /// @brief Draws the attachment strip between the field and the divider
        using SlotRenderer = std::function<void(float innerWidth)>;

        /// @brief Composer heights the host needs before it submits the card
        struct CardMetrics
        {
            float fieldHeight = 0.f;
            float cardHeight = 0.f;
            float attachmentsHeight = 0.f;
            int toolbarRows = 1;
        };

        PromptInput() = default;
        explicit PromptInput(Config config);

        /**
         * @brief Replaces the layout config of this instance.
         * @details The defaults suit a wide chat panel; a host that wants a single-line composer
         *          derives the numbers from its own font size and applies them here.
         */
        void setConfig(Config config) noexcept
        {
            m_config = config;
        }

        /// @brief Moves the draft out for submission and clears it
        std::string takeDraft();

        /**
         * @brief Takes the draft and also clears the live text of the focused field.
         *
         * ImGui keeps a private copy of the text while a field has focus and writes it
         * back to our buffer, so submitting a prompt must reset that copy as well.
         */
        std::string consumeDraft();

        /// @brief Replaces the current draft (used by file import and restore)
        void setDraft(std::string draft);
        [[nodiscard]] std::string const & draft() const noexcept
        {
            return m_draft;
        }
        [[nodiscard]] std::size_t draftBytes() const noexcept
        {
            return m_draft.size();
        }

        /// @brief True when either prompt field had focus during the previous frame
        [[nodiscard]] bool isFocused() const noexcept
        {
            return m_lastInlineFocused || m_lastExpandedFocused;
        }

        [[nodiscard]] Config const & config() const noexcept
        {
            return m_config;
        }

        /// @brief Lets the host declare how much toolbar width its own leading controls need
        void setLeadingToolbarWidth(float width) noexcept
        {
            m_config.leadingToolbarWidth = std::max(0.f, width);
        }

        /// @brief Appends the text content of a .txt/.md file to the draft
        /// @return false when the file is unsupported, unreadable, or oversized
        bool appendTextFile(std::filesystem::path const & path, std::string & outError);

        /// @brief Outer card width: the full content width of the host window
        [[nodiscard]] float cardOuterWidth() const;

        /// @brief Usable text width of the field: the full inner width of the card
        [[nodiscard]] float innerFieldWidth() const;

        /// @brief Estimates the field height in pixels for the current draft
        /// @param fieldWidth Width returned by innerFieldWidth for this frame
        /// @param maxHeightLimit Upper bound in pixels; 0 uses config.maxHeight
        [[nodiscard]] float measureInlineHeight(float fieldWidth,
                                                float maxHeightLimit = 0.f) const;

        /// @brief Height of one toolbar row
        [[nodiscard]] float toolbarHeight() const;

        /// @brief Height of the attachment strip for the given number of thumbnails
        [[nodiscard]] float attachmentStripHeight(std::size_t attachmentCount) const;

        /**
         * @brief Measures the whole composer for the current draft at the current panel width.
         * @param maxHeightLimit Total card height the panel can spare; 0 leaves it unbounded
         * @param attachmentCount Thumbnails the host will draw in the attachment strip
         */
        [[nodiscard]] CardMetrics measureCard(float maxHeightLimit,
                                              std::size_t attachmentCount = 0u) const;

        /**
         * @brief Renders the composer card: field, attachment strip, divider and toolbar.
         * @param metrics Metrics returned by measureCard for this frame
         * @param attachments Draws the attachment strip; empty when there are none
         * @param toolbar Draws the host toolbar controls, once per toolbar row
         * @param sendEnabled False while the agent run is active
         */
        RenderResult renderCard(CardMetrics const & metrics,
                                SlotRenderer const & attachments,
                                ToolbarRenderer const & toolbar,
                                bool sendEnabled);

        /// @brief Opens or closes the large-form editor window
        void setExpandedOpen(bool open) noexcept
        {
            m_expandedOpen = open;
        }
        [[nodiscard]] bool isExpandedOpen() const noexcept
        {
            return m_expandedOpen;
        }

        /// @brief Renders the large-form editor window; returns nullopt while it is closed
        std::optional<RenderResult> renderExpandedWindow(bool sendEnabled);

        // --- ImGui-free helpers ---

        /// @brief True when the empty-field placeholder should be drawn
        static bool shouldShowPlaceholder(std::string const & draft, char const * placeholder);

        /**
         * @brief Splits a toolbar row into the host's leading controls, its combos and the
         *        expand/submit buttons, wrapping the combos to a second row when they do not fit.
         */
        static ToolbarLayout computeToolbarLayout(float innerWidth,
                                                  float iconButtonSize,
                                                  float leadingWidth,
                                                  float minComboWidth,
                                                  float itemSpacingX);

        /// @brief Card height from the field height plus the card chrome
        static float computeCardHeight(float fieldHeight,
                                       float attachmentsHeight,
                                       float toolbarRowHeight,
                                       int toolbarRows,
                                       float padding,
                                       float separatorThickness,
                                       float verticalSpacing);

        /// @brief Width to give each of two combos sharing a row of the given host width
        static float computeComboWidth(float hostWidth, float leadingWidth, float itemSpacingX);

        /**
         * @brief Height of the attachment strip: thumbnails packed into rows of availableWidth.
         * @param extraThumbWidth Extra width each thumbnail needs besides its image (remove button)
         */
        static float computeAttachmentStripHeight(std::size_t attachmentCount,
                                                  float thumbSize,
                                                  float extraThumbWidth,
                                                  float availableWidth,
                                                  float itemSpacingX);

        /**
         * @brief Counts rows used by left-to-right items that wrap the way ImGui's SameLine does.
         * @details Conservative by design: over-reserving only costs blank space, while
         *          under-reserving clips the widget the rows belong to.
         */
        static std::size_t countWrappedRows(std::vector<float> const & itemWidths,
                                            float availableWidth,
                                            float itemSpacingX);

        /// @brief Counts rendered lines: hard newlines plus estimated soft wraps
        static std::size_t countVisualLines(std::string const & text, std::size_t charsPerLine);

        /// @brief Converts a line count into a widget height in pixels
        static float computeFieldHeight(std::size_t lineCount,
                                        float lineHeightWithSpacing,
                                        float framePaddingY,
                                        float minHeight,
                                        float maxHeight);

        /// @brief True when the extension is an accepted plain-text instruction file
        static bool isSupportedTextFile(std::filesystem::path const & path);

        /// @brief Appends text to the draft, separating it from existing content
        static void appendToDraft(std::string & draft, std::string const & text);

        /// @brief True when the payload looks like text (no NUL, no lone CR/LF control noise)
        static bool isTextContent(std::string const & content);

        /// @brief Counts Unicode code points in a UTF-8 string
        static std::size_t countUtf8Characters(std::string const & text);

      private:
        /// @brief Card border thickness, independent of the active theme's frame borders
        static constexpr float kCardBorderThickness = 1.f;
        /// @brief Divider thickness between the field and the toolbar
        static constexpr float kSeparatorThickness = 1.f;

        /// @brief Width the expand and submit buttons occupy at the toolbar end
        [[nodiscard]] float trailingToolbarWidth() const noexcept;

        /// @brief Width of the remove button drawn beside one attachment thumbnail
        [[nodiscard]] float attachmentRemoveButtonWidth() const;

        Config m_config;
        std::string m_draft;
        bool m_lastInlineFocused = false;
        bool m_lastExpandedFocused = false;
        bool m_expandedOpen = false;
        std::uint32_t m_activeFieldId = 0u;
    };
} // namespace gladius::ui
