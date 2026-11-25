#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace gladius::ui
{
    /**
     * @brief Holds the Design Agent prompt draft and renders it as an auto-growing
     *        multiline input plus an optional large-form editor window.
     *
     * The draft is an unbounded std::string (rendered through the imgui_stdlib
     * InputTextMultiline overload), so long instruction blocks are never truncated.
     * All geometry/line math is exposed as ImGui-free statics for unit testing.
     */
    class PromptInput
    {
      public:
        /// @brief Layout and import limits for the prompt input
        struct Config
        {
            std::size_t minLines = 2;
            std::size_t maxLines = 10;
            float minHeight = 52.f;
            float maxHeight = 200.f;
            float sendButtonWidth = 65.f;
            float expandButtonWidth = 30.f;
            std::size_t maxImportBytes = 256u * 1024u;
        };

        /// @brief Result of a prompt input render pass
        struct RenderResult
        {
            bool sendRequested = false;
            bool focused = false;
        };

        PromptInput() = default;
        explicit PromptInput(Config config);

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

        /// @brief Appends the text content of a .txt/.md file to the draft
        /// @return false when the file is unsupported, unreadable, or oversized
        bool appendTextFile(std::filesystem::path const & path, std::string & outError);

        /// @brief Usable text width of the inline field, leaving room for the buttons
        [[nodiscard]] float inlineFieldWidth() const;

        /// @brief Estimates the inline field height in pixels for the current draft
        /// @param fieldWidth Usable text width of the field in pixels
        /// @param maxHeightLimit Upper bound in pixels; 0 uses config.maxHeight
        [[nodiscard]] float measureInlineHeight(float fieldWidth, float maxHeightLimit = 0.f) const;

        /// @brief Renders the inline multiline field plus expand and Send buttons
        /// @param fieldWidth Width returned by inlineFieldWidth for this frame
        /// @param fieldHeight Height returned by measureInlineHeight for this frame
        /// @param sendEnabled False while the agent run is active
        RenderResult renderInline(char const * label,
                                  char const * sendLabel,
                                  float fieldWidth,
                                  float fieldHeight,
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
        Config m_config;
        std::string m_draft;
        bool m_lastInlineFocused = false;
        bool m_lastExpandedFocused = false;
        bool m_expandedOpen = false;
        std::uint32_t m_activeFieldId = 0u;
    };
} // namespace gladius::ui
