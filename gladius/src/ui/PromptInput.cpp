#include "PromptInput.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>

namespace gladius::ui
{
    namespace
    {
        char const * const expandedWindowTitle = "Agent Prompt Editor###GladiusAgentPromptEditor";
        char const * const sendChordHelp = "Ctrl/Cmd+Enter sends";

        /// @brief True when the prompt send chord (Ctrl/Cmd+Enter) went down this frame
        bool isSendChordPressed()
        {
            ImGuiIO const & io = ImGui::GetIO();
            return (io.KeyCtrl || io.KeySuper)
              && (ImGui::IsKeyPressed(ImGuiKey_Enter, false)
                  || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        }

        /// @brief Mean glyph width of a representative sentence, for soft-wrap estimation
        float averageGlyphWidth()
        {
            constexpr char const * reference =
              "The quick brown fox jumps over 13 lazy dogs, 0123456789.";
            float const referenceWidth = ImGui::CalcTextSize(reference).x;
            if (referenceWidth <= 0.f)
            {
                return ImGui::GetFontSize() * 0.5f;
            }
            return referenceWidth / static_cast<float>(PromptInput::countUtf8Characters(reference));
        }

        std::size_t readTextFile(std::filesystem::path const & path,
                                 std::size_t maxBytes,
                                 std::string & outContent,
                                 std::string & outError)
        {
            std::error_code ec;
            auto const size = std::filesystem::file_size(path, ec);
            if (ec)
            {
                outError = "Cannot read file: " + path.string();
                return 0u;
            }
            if (size > maxBytes)
            {
                outError = "File is larger than " + std::to_string(maxBytes / 1024u)
                         + " KiB: " + path.filename().string();
                return 0u;
            }

            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                outError = "Cannot open file: " + path.string();
                return 0u;
            }

            outContent.assign(size, '\0');
            in.read(outContent.data(), static_cast<std::streamsize>(size));
            return static_cast<std::size_t>(in.gcount());
        }
    } // namespace

    PromptInput::PromptInput(Config config)
      : m_config(config)
    {
    }

    std::string PromptInput::takeDraft()
    {
        return std::exchange(m_draft, std::string{});
    }

    void PromptInput::setDraft(std::string draft)
    {
        m_draft = std::move(draft);
    }

    bool PromptInput::appendTextFile(std::filesystem::path const & path, std::string & outError)
    {
        outError.clear();
        if (!isSupportedTextFile(path))
        {
            outError = "Unsupported file type: " + path.filename().string();
            return false;
        }

        std::string content;
        if (readTextFile(path, m_config.maxImportBytes, content, outError) == 0u)
        {
            if (outError.empty())
            {
                outError = "File is empty: " + path.filename().string();
            }
            return false;
        }
        if (!isTextContent(content))
        {
            outError = "File is not plain text: " + path.filename().string();
            return false;
        }

        appendToDraft(m_draft, content);
        return true;
    }

    float PromptInput::inlineFieldWidth() const
    {
        // Measured from the window work rect, not the cursor: the width is needed before
        // the footer line is submitted, while the cursor still sits after other items.
        float const spacing = ImGui::GetStyle().ItemSpacing.x;
        float const buttons = m_config.sendButtonWidth + m_config.expandButtonWidth + spacing * 2.f;
        return std::max(40.f, ImGui::GetCurrentWindow()->WorkRect.GetWidth() - buttons);
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
                             ? std::min(m_config.maxHeight, maxHeightLimit)
                             : m_config.maxHeight;
        return computeFieldHeight(lines,
                                  lineHeight,
                                  ImGui::GetStyle().FramePadding.y,
                                  m_config.minHeight,
                                  maxHeight);
    }

    PromptInput::RenderResult PromptInput::renderInline(char const * label,
                                                        char const * sendLabel,
                                                        float fieldWidth,
                                                        float fieldHeight,
                                                        bool sendEnabled)
    {
        RenderResult result;
        bool const chordPressed = m_lastInlineFocused && sendEnabled && isSendChordPressed();

        ImGui::PushID(label);
        ImGui::InputTextMultiline("##field", &m_draft, ImVec2(fieldWidth, fieldHeight));
        result.focused = ImGui::IsItemFocused();
        if (result.focused)
        {
            m_activeFieldId = ImGui::GetItemID();
        }

        ImGui::SameLine();
        if (ImGui::Button(m_expandedOpen ? ICON_FA_WINDOW_MINIMIZE : ICON_FA_WINDOW_MAXIMIZE,
                          ImVec2(m_config.expandButtonWidth, fieldHeight)))
        {
            m_expandedOpen = !m_expandedOpen;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Large prompt editor\nDrop .txt/.md files for long instructions");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!sendEnabled);
        bool const sendClicked =
          ImGui::Button(sendLabel, ImVec2(m_config.sendButtonWidth, fieldHeight));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", sendChordHelp);
        }
        ImGui::PopID();

        result.sendRequested = sendClicked || chordPressed;
        m_lastInlineFocused = result.focused;
        return result;
    }

    std::optional<PromptInput::RenderResult> PromptInput::renderExpandedWindow(bool sendEnabled)
    {
        if (!m_expandedOpen)
        {
            return std::nullopt;
        }

        ImGuiViewport const * viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.2f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.15f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.6f,
                                        viewport->WorkSize.y * 0.7f),
                                 ImGuiCond_FirstUseEver);

        RenderResult result;
        bool open = true;
        if (!ImGui::Begin(expandedWindowTitle, &open))
        {
            m_expandedOpen = open;
            ImGui::End();
            return result;
        }

        ImGuiStyle const & style = ImGui::GetStyle();
        float const footerHeight =
          ImGui::GetFrameHeight() + ImGui::CalcTextSize(sendChordHelp).y + style.ItemSpacing.y * 3.f;
        ImGui::InputTextMultiline("##expandedField",
                                  &m_draft,
                                  ImVec2(-1.f, -footerHeight));
        result.focused = ImGui::IsItemFocused();
        if (result.focused)
        {
            m_activeFieldId = ImGui::GetItemID();
        }
        bool const chordPressed = m_lastExpandedFocused && sendEnabled && isSendChordPressed();

        float const innerWidth =
          std::max(1.f, ImGui::GetItemRectSize().x - style.FramePadding.x * 2.f);
        std::size_t const charsPerLine =
          std::max<std::size_t>(1u, static_cast<std::size_t>(innerWidth / averageGlyphWidth()));

        ImGui::TextDisabled("%zu chars, ~%zu lines - %s",
                            m_draft.size(),
                            countVisualLines(m_draft, charsPerLine),
                            sendChordHelp);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sendEnabled);
        bool const sendClicked = ImGui::Button("Send");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            open = false;
        }

        ImGui::End();
        m_expandedOpen = open;

        result.sendRequested = sendClicked || chordPressed;
        m_lastExpandedFocused = result.focused;
        return result;
    }

    std::string PromptInput::consumeDraft()
    {
        // While focused, InputText owns a private copy of the text and re-applies it to
        // our buffer, so the submitted prompt must be dropped from there as well.
        if (ImGuiInputTextState * state = ImGui::GetInputTextState(m_activeFieldId))
        {
            state->ClearText();
        }
        ImGui::ClearActiveID();

        // Discard the snapshot InputText stores on deactivation, otherwise the cleared
        // prompt is written back into the draft on the next frame.
        ImGuiContext & g = *GImGui;
        if (g.InputTextDeactivatedState.ID == m_activeFieldId)
        {
            g.InputTextDeactivatedState.ID = 0;
        }

        return takeDraft();
    }

    std::size_t PromptInput::countUtf8Characters(std::string const & text)
    {
        std::size_t count = 0u;
        for (char const c : text)
        {
            auto const byte = static_cast<unsigned char>(c);
            if ((byte & 0xC0u) != 0x80u)
            {
                ++count;
            }
        }
        return count;
    }

    std::size_t PromptInput::countVisualLines(std::string const & text, std::size_t charsPerLine)
    {
        std::size_t const wrapWidth = std::max<std::size_t>(1u, charsPerLine);
        std::size_t lines = 0u;
        std::size_t lineChars = 0u;
        std::size_t const size = text.size();

        for (std::size_t i = 0u; i < size; ++i)
        {
            if (text[i] == '\n')
            {
                lines += lineChars == 0u ? 1u : (lineChars + wrapWidth - 1u) / wrapWidth;
                lineChars = 0u;
                continue;
            }
            auto const byte = static_cast<unsigned char>(text[i]);
            if ((byte & 0xC0u) != 0x80u)
            {
                ++lineChars;
            }
        }

        bool const trailingEmptyLine = !text.empty() && text.back() == '\n';
        if (trailingEmptyLine)
        {
            return lines + 1u;
        }
        return lines + (lineChars == 0u ? 0u : (lineChars + wrapWidth - 1u) / wrapWidth);
    }

    float PromptInput::computeFieldHeight(std::size_t lineCount,
                                          float lineHeightWithSpacing,
                                          float framePaddingY,
                                          float minHeight,
                                          float maxHeight)
    {
        float const contentHeight = static_cast<float>(lineCount) * lineHeightWithSpacing
                                  + framePaddingY * 2.f;
        float const upper = std::max(minHeight, maxHeight);
        return std::clamp(contentHeight, minHeight, upper);
    }

    bool PromptInput::isSupportedTextFile(std::filesystem::path const & path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".txt" || extension == ".md" || extension == ".markdown"
          || extension == ".text";
    }

    void PromptInput::appendToDraft(std::string & draft, std::string const & text)
    {
        if (text.empty())
        {
            return;
        }

        std::string body = text;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
        {
            body.pop_back();
        }

        if (draft.empty())
        {
            draft = std::move(body);
            return;
        }
        if (draft.back() != '\n')
        {
            draft += '\n';
        }
        draft += body;
    }

    bool PromptInput::isTextContent(std::string const & content)
    {
        for (char const c : content)
        {
            if (c == '\0')
            {
                return false;
            }
            if (c == '\x1A') // EOF marker used by some Windows text files
            {
                return false;
            }
        }
        return true;
    }
} // namespace gladius::ui
